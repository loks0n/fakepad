// libusb shim: hides PowerA GIP pad, presents a synthetic wired Xbox 360 pad to SDL,
// fed by live input from the real pad over one internal GIP connection.
#include <libusb.h>
#include <dlfcn.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <unistd.h>
#include <pthread.h>

#define PA_VID 0x20d6
#define PA_PID 0x2062
#define X360_VID 0x045e
#define X360_PID 0x028e

static FILE *L;
static void lg(const char *f, ...){ if(!L)return; va_list a; va_start(a,f); vfprintf(L,f,a); va_end(a); fputc('\n',L); fflush(L); }

// ---- real libusb (for our internal connection to the physical pad) ----
static void *R;
#define P(n) static __typeof__(n) *p_##n;
P(libusb_init) P(libusb_exit) P(libusb_get_device_list) P(libusb_free_device_list)
P(libusb_get_device_descriptor) P(libusb_open) P(libusb_close) P(libusb_claim_interface)
P(libusb_release_interface) P(libusb_interrupt_transfer) P(libusb_get_active_config_descriptor)
P(libusb_free_config_descriptor)

// ---- shared synthetic X360 state ----
static pthread_mutex_t slk = PTHREAD_MUTEX_INITIALIZER;
static unsigned char x360[20];          // current 20-byte wired-360 report
static int pad_live = 0;
static unsigned char rmb_big, rmb_small; static int rmb_dirty;

// ---- sentinels for the fake device/handle ----
static int FAKE_DEV, FAKE_HANDLE;
#define FDEV  ((libusb_device*)&FAKE_DEV)
#define FHND  ((libusb_device_handle*)&FAKE_HANDLE)

// ================= internal GIP reader (proven handshake) =================
static int dec_varint(const unsigned char*b,int n,unsigned long*v){*v=0;int i;for(i=0;i<n;i++){*v|=(unsigned long)(b[i]&0x7f)<<(i*7);if(!(b[i]&0x80)){i++;break;}}return i;}
static int enc_varint(unsigned long v,unsigned char*b){int i=0;do{unsigned char x=v&0x7f;v>>=7;if(v)x|=0x80;b[i++]=x;}while(v);return i;}

static libusb_device_handle *gh; static unsigned char gin, gout; static unsigned char gseq[256];
static void gsend(unsigned char t,unsigned char fl,unsigned char sq,const unsigned char*p,int n){
    unsigned char b[64]={t,fl,sq}; int o=3; o+=enc_varint(n,b+o); if(n)memcpy(b+o,p,n); int s; p_libusb_interrupt_transfer(gh,gout,b,o+n,&s,1000); usleep(4000);
}
static void ginit_seq(void){
    unsigned char start[]={0x00}, led[]={0x00,0x01,0x14}, sec[]={0x01,0x00}, irr[]={0,0,0};
    gsend(0x05,0x20,++gseq[5],start,1); gsend(0x0a,0x20,++gseq[10],led,3);
    gsend(0x06,0x20,++gseq[6],sec,2);   gsend(0x0a,0x00,++gseq[11],irr,3);
}
// map a GIP gamepad body -> 20-byte wired-360 report
static void map_report(const unsigned char *b){
    unsigned char r[20]={0}; r[1]=0x14;
    unsigned char b0=b[0], b1=b[1];
    // data[2]: dpad(b1 0..3) + start/back(b0 2,3) + stick clicks(b1 6,7)
    if(b1&0x01)r[2]|=0x01; if(b1&0x02)r[2]|=0x02; if(b1&0x04)r[2]|=0x04; if(b1&0x08)r[2]|=0x08;
    if(b0&0x04)r[2]|=0x10; if(b0&0x08)r[2]|=0x20; if(b1&0x40)r[2]|=0x40; if(b1&0x80)r[2]|=0x80;
    // data[3]: LB/RB(b1 5,4) guide(b0 1) A/B/X/Y(b0 4,5,6,7)
    if(b1&0x10)r[3]|=0x01; if(b1&0x20)r[3]|=0x02; if(b0&0x02)r[3]|=0x04;
    if(b0&0x10)r[3]|=0x10; if(b0&0x20)r[3]|=0x20; if(b0&0x40)r[3]|=0x40; if(b0&0x80)r[3]|=0x80;
    // triggers: GIP 10-bit -> 360 8-bit
    unsigned lt=(b[2]|(b[3]<<8))&0x3ff, rt=(b[4]|(b[5]<<8))&0x3ff;
    r[4]=lt>>2; r[5]=rt>>2;
    // sticks: int16 pass-through
    memcpy(r+6, b+6, 8);
    pthread_mutex_lock(&slk); memcpy(x360,r,20); pthread_mutex_unlock(&slk);
}
static void *gip_thread(void *unused){
    (void)unused;
    libusb_context *ic=NULL; p_libusb_init(&ic);
    for(;;){
        // open by scanning list (we didn't bind open_by_vidpid)
        libusb_device **list; ssize_t n=p_libusb_get_device_list(ic,&list); gh=NULL;
        for(ssize_t i=0;i<n;i++){ struct libusb_device_descriptor d; if(!p_libusb_get_device_descriptor(list[i],&d)&&d.idVendor==PA_VID&&d.idProduct==PA_PID){ if(!p_libusb_open(list[i],&gh))break; } }
        p_libusb_free_device_list(list,1);
        if(!gh){ usleep(500000); continue; }
        gin=0x81; gout=0x01;
        p_libusb_claim_interface(gh,0);
        memset(gseq,0,sizeof(gseq));
        gsend(0x04,0x20,++gseq[4],NULL,0); // metadata request kicks negotiation
        int meta=0; unsigned char buf[64]; unsigned long total=0,foff=0;
        lg("[gip] opened real pad, negotiating");
        for(;;){
            if(rmb_dirty){ pthread_mutex_lock(&slk); unsigned char bg=rmb_big,sm=rmb_small; rmb_dirty=0; pthread_mutex_unlock(&slk); unsigned char m[8]={0x0f,0,0,bg,sm,0xff,0,0}; gsend(0x09,0x00,++gseq[9],m,8); }
            int got=0; int rr=p_libusb_interrupt_transfer(gh,gin,buf,sizeof(buf),&got,10);
            if(rr!=0){ if(rr==LIBUSB_ERROR_TIMEOUT)continue; lg("[gip] read err %d, reopening",rr); break; }
            unsigned char ty=buf[0],fl=buf[1],sq=buf[2]; int o=3; unsigned long ln; o+=dec_varint(buf+o,got-o,&ln);
            if(ty==0x20&&!(fl&0x20)){ pad_live=1; map_report(buf+o); continue; }
            if(ty==0x02){ memset(gseq,0,sizeof(gseq)); gsend(0x04,0x20,++gseq[4],NULL,0); continue; }
            if(fl&0x80){ unsigned long v; o+=dec_varint(buf+o,got-o,&v);
                if(fl&0x40){total=v;foff=ln;} else if(ln==0){ if(!meta){meta=1;ginit_seq();} if(fl&0x10){unsigned char a[9]={0,ty,0x20,(unsigned char)foff,foff>>8,foff>>16,foff>>24,0,0};gsend(0x01,0x20,sq,a,9);} continue;} else foff=v+ln;
                if(fl&0x10){unsigned int rem=total-foff; unsigned char a[9]={0,ty,0x20,(unsigned char)foff,foff>>8,foff>>16,foff>>24,(unsigned char)rem,rem>>8};gsend(0x01,0x20,sq,a,9);}
                if(foff>=total&&total&&!meta){meta=1;ginit_seq();}
                continue;
            }
            if(fl&0x10){unsigned char a[9]={0,ty,0x20,(unsigned char)ln,ln>>8,0,0,0,0};gsend(0x01,0x20,sq,a,9);}
        }
        p_libusb_close(gh); gh=NULL; pad_live=0; usleep(300000);
    }
    return NULL;
}

// need open_by_vid_pid too
P(libusb_open_device_with_vid_pid) P(libusb_get_device)
static pthread_once_t g_once=PTHREAD_ONCE_INIT;
static void start_gip(void){ pthread_t t; pthread_create(&t,NULL,gip_thread,NULL); pthread_detach(t); lg("[lazy] gip thread started in pid=%d",getpid()); }
static void ensure_gip(void){ pthread_once(&g_once,start_gip); }

__attribute__((constructor)) static void boot(void){
    if(getenv("FAKEPAD_DEBUG")) L=fopen("/tmp/fakepad.log","a");
    lg("=== fakepad constructor pid=%d ===",getpid());
    Dl_info di; dladdr((void*)&boot,&di); char pth[2048]; strncpy(pth,di.dli_fname,sizeof(pth)-1);
    char *s=strrchr(pth,'/'); if(s)strcpy(s+1,"libusb-real.dylib");
    R=dlopen(pth,RTLD_NOW|RTLD_LOCAL); lg("dlopen real=%p (%s)",R,pth);
    if(!R){lg("FATAL no real libusb");return;}
#define B(n) p_##n=(void*)dlsym(R,#n);
    B(libusb_init)B(libusb_exit)B(libusb_get_device_list)B(libusb_free_device_list)
    B(libusb_get_device_descriptor)B(libusb_open)B(libusb_close)B(libusb_claim_interface)
    B(libusb_release_interface)B(libusb_interrupt_transfer)B(libusb_get_active_config_descriptor)
    B(libusb_free_config_descriptor)B(libusb_open_device_with_vid_pid)B(libusb_get_device)
    // neutral centered sticks
    pthread_mutex_lock(&slk); x360[1]=0x14; pthread_mutex_unlock(&slk);
}

// ===================== libusb API presented to SDL =====================
int libusb_init(libusb_context **ctx){ if(ctx)*ctx=(libusb_context*)0x1; lg("init"); ensure_gip(); return 0; }
void libusb_exit(libusb_context *c){ (void)c; }
int libusb_has_capability(uint32_t c){ (void)c; return 0; } // no hotplug -> SDL polls get_device_list

ssize_t libusb_get_device_list(libusb_context *c,libusb_device ***list){ (void)c; ensure_gip();
    libusb_device **a=calloc(2,sizeof(*a)); a[0]=FDEV; a[1]=NULL; *list=a; return 1;
}
void libusb_free_device_list(libusb_device **l,int u){ (void)u; if(l)free(l); }

int libusb_get_device_descriptor(libusb_device *d,struct libusb_device_descriptor *x){
    if(d!=FDEV) return LIBUSB_ERROR_NO_DEVICE;
    memset(x,0,sizeof(*x)); x->bLength=18; x->bDescriptorType=1; x->bcdUSB=0x0200;
    x->bDeviceClass=0xff; x->bDeviceSubClass=0xff; x->bDeviceProtocol=0xff; x->bMaxPacketSize0=8;
    x->idVendor=X360_VID; x->idProduct=X360_PID; x->bcdDevice=0x0114; x->bNumConfigurations=1;
    x->iManufacturer=0; x->iProduct=0; x->iSerialNumber=0; return 0;
}
static struct libusb_endpoint_descriptor eps[2];
static struct libusb_interface_descriptor ifd;
static struct libusb_interface iface;
static struct libusb_config_descriptor cfgd;
int libusb_get_active_config_descriptor(libusb_device *d,struct libusb_config_descriptor **c){
    if(d!=FDEV) return LIBUSB_ERROR_NO_DEVICE;
    memset(eps,0,sizeof(eps)); memset(&ifd,0,sizeof(ifd)); memset(&iface,0,sizeof(iface)); memset(&cfgd,0,sizeof(cfgd));
    eps[0].bLength=7; eps[0].bDescriptorType=5; eps[0].bEndpointAddress=0x81; eps[0].bmAttributes=3; eps[0].wMaxPacketSize=32; eps[0].bInterval=4;
    eps[1].bLength=7; eps[1].bDescriptorType=5; eps[1].bEndpointAddress=0x01; eps[1].bmAttributes=3; eps[1].wMaxPacketSize=32; eps[1].bInterval=8;
    ifd.bLength=9; ifd.bDescriptorType=4; ifd.bInterfaceNumber=0; ifd.bNumEndpoints=2;
    ifd.bInterfaceClass=0xff; ifd.bInterfaceSubClass=0x5d; ifd.bInterfaceProtocol=0x01; ifd.endpoint=eps;
    iface.altsetting=&ifd; iface.num_altsetting=1;
    cfgd.bLength=9; cfgd.bDescriptorType=2; cfgd.wTotalLength=0x20; cfgd.bNumInterfaces=1; cfgd.bConfigurationValue=1; cfgd.bmAttributes=0x80; cfgd.MaxPower=250; cfgd.interface=&iface;
    *c=&cfgd; return 0;
}
int libusb_get_config_descriptor(libusb_device *d,uint8_t i,struct libusb_config_descriptor **c){ (void)i; return libusb_get_active_config_descriptor(d,c); }
void libusb_free_config_descriptor(struct libusb_config_descriptor *c){ (void)c; } // static, nothing to free

uint8_t libusb_get_bus_number(libusb_device *d){ (void)d; return 1; }
uint8_t libusb_get_device_address(libusb_device *d){ (void)d; return 42; }
int libusb_get_port_numbers(libusb_device *d,uint8_t *p,int n){ (void)d; if(n>0){p[0]=1;return 1;} return LIBUSB_ERROR_OVERFLOW; }
libusb_device *libusb_get_device(libusb_device_handle *h){ (void)h; return FDEV; }

int libusb_open(libusb_device *d,libusb_device_handle **h){ if(d!=FDEV)return LIBUSB_ERROR_NO_DEVICE; *h=FHND; lg("open fake"); return 0; }
void libusb_close(libusb_device_handle *h){ (void)h; }
int libusb_claim_interface(libusb_device_handle *h,int i){ (void)h;(void)i; return 0; }
int libusb_release_interface(libusb_device_handle *h,int i){ (void)h;(void)i; return 0; }
int libusb_kernel_driver_active(libusb_device_handle *h,int i){ (void)h;(void)i; return 0; }
int libusb_detach_kernel_driver(libusb_device_handle *h,int i){ (void)h;(void)i; return 0; }
int libusb_attach_kernel_driver(libusb_device_handle *h,int i){ (void)h;(void)i; return 0; }
int libusb_set_auto_detach_kernel_driver(libusb_device_handle *h,int e){ (void)h;(void)e; return 0; }
int libusb_set_interface_alt_setting(libusb_device_handle *h,int i,int a){ (void)h;(void)i;(void)a; return 0; }
const char *libusb_error_name(int e){ (void)e; return "FAKE"; }
int libusb_control_transfer(libusb_device_handle *h,uint8_t rt,uint8_t rq,uint16_t v,uint16_t idx,unsigned char *data,uint16_t len,unsigned int to){
    (void)h;(void)rt;(void)rq;(void)v;(void)idx;(void)to; if(data&&len)memset(data,0,len); return len; }

// ---- async transfers ----
struct libusb_transfer *libusb_alloc_transfer(int iso){ struct libusb_transfer *t=calloc(1,sizeof(*t)+iso*sizeof(struct libusb_iso_packet_descriptor)); return t; }
void libusb_free_transfer(struct libusb_transfer *t){ if(t)free(t); }
#define MAXQ 64
static struct libusb_transfer *q[MAXQ]; static int qn=0; static pthread_mutex_t qlk=PTHREAD_MUTEX_INITIALIZER;
int libusb_submit_transfer(struct libusb_transfer *t){
    if((t->endpoint&0x80)==0){ if(t->length>=5&&t->buffer&&t->buffer[1]==0x08){ pthread_mutex_lock(&slk); rmb_big=t->buffer[3]; rmb_small=t->buffer[4]; rmb_dirty=1; pthread_mutex_unlock(&slk);}
        t->actual_length=t->length; t->status=LIBUSB_TRANSFER_COMPLETED;
        pthread_mutex_lock(&qlk); if(qn<MAXQ)q[qn++]=t; pthread_mutex_unlock(&qlk); return 0;
    }
    pthread_mutex_lock(&qlk); if(qn<MAXQ)q[qn++]=t; pthread_mutex_unlock(&qlk); return 0;
}
int libusb_cancel_transfer(struct libusb_transfer *t){
    pthread_mutex_lock(&qlk); for(int i=0;i<qn;i++)if(q[i]==t){t->status=LIBUSB_TRANSFER_CANCELLED;} pthread_mutex_unlock(&qlk); return 0;
}
static int handle(void){
    usleep(3000); // ~pace the read loop
    struct libusb_transfer *done[MAXQ]; int dn=0;
    pthread_mutex_lock(&qlk);
    for(int i=0;i<qn;i++){ struct libusb_transfer *t=q[i];
        if((t->endpoint&0x80)&&t->status!=LIBUSB_TRANSFER_CANCELLED){
            pthread_mutex_lock(&slk); int L20=t->length<20?t->length:20; memcpy(t->buffer,x360,L20); pthread_mutex_unlock(&slk);
            t->actual_length=L20; t->status=LIBUSB_TRANSFER_COMPLETED;
        }
        done[dn++]=t;
    }
    qn=0; pthread_mutex_unlock(&qlk);
    for(int i=0;i<dn;i++){ if(done[i]->callback) done[i]->callback(done[i]); }
    return 0;
}
int libusb_handle_events(libusb_context *c){ (void)c; return handle(); }
int libusb_handle_events_completed(libusb_context *c,int *x){ (void)c;(void)x; return handle(); }
int libusb_handle_events_timeout(libusb_context *c,struct timeval *tv){ (void)c;(void)tv; return handle(); }
int libusb_handle_events_timeout_completed(libusb_context *c,struct timeval *tv,int *x){ (void)c;(void)tv;(void)x; return handle(); }
void libusb_interrupt_event_handler(libusb_context *c){ (void)c; }

int libusb_interrupt_transfer(libusb_device_handle *h,unsigned char ep,unsigned char *data,int len,int *xfer,unsigned int to){
    (void)h;(void)to; if(ep&0x80){ usleep(3000); pthread_mutex_lock(&slk); int L20=len<20?len:20; memcpy(data,x360,L20); pthread_mutex_unlock(&slk); if(xfer)*xfer=L20; }
    else { if(len>=5&&data&&data[1]==0x08){ pthread_mutex_lock(&slk); rmb_big=data[3]; rmb_small=data[4]; rmb_dirty=1; pthread_mutex_unlock(&slk);} if(xfer)*xfer=len; } return 0;
}
int libusb_bulk_transfer(libusb_device_handle *h,unsigned char ep,unsigned char *d,int len,int *xfer,unsigned int to){ return libusb_interrupt_transfer(h,ep,d,len,xfer,to); }
int libusb_hotplug_register_callback(libusb_context*c,int e,int f,int v,int p,int dc,libusb_hotplug_callback_fn cb,void*ud,libusb_hotplug_callback_handle*hd){ (void)c;(void)e;(void)f;(void)v;(void)p;(void)dc;(void)cb;(void)ud; if(hd)*hd=1; return 0; }
void libusb_hotplug_deregister_callback(libusb_context*c,libusb_hotplug_callback_handle h){ (void)c;(void)h; }

// redirect SDL's dlopen("libusb-1.0.0.dylib") to us
static void *my_dlopen(const char *path,int mode){ if(path&&strstr(path,"libusb-1.0.0.dylib")){ Dl_info di; dladdr((void*)&my_dlopen,&di); return dlopen(di.dli_fname,mode); } return dlopen(path,mode); }
__attribute__((used)) static struct{const void*a,*b;} _ip __attribute__((section("__DATA,__interpose")))={(const void*)my_dlopen,(const void*)dlopen};
