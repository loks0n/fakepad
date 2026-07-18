// fakepad — libusb shim that makes third-party GIP ("Xbox One/Series") wired
// controllers work with Steam on macOS.
//
// SDL on macOS refuses to run its Xbox One/GIP driver for a wired controller
// UNLESS the device (a) is offered through the libusb HIDAPI backend (path not
// starting with "DevSrvsID"), (b) presents the GIP interface signature
// (class 0xFF / subclass 0x47 / protocol 0xD0), and (c) has a VID/PID SDL maps
// to the XBOXONE type. Real third-party pads fail (c): SDL doesn't recognise
// their VID/PID, so it never engages the Xbox One driver and falls back to a
// generic HID path that stalls — the connect/disconnect loop.
//
// This shim presents a synthetic device with a genuine Microsoft Xbox One VID/PID
// and the GIP interface, then transparently RELAYS the raw GIP byte stream
// between SDL's own Xbox One driver and the real pad. SDL performs the handshake
// (single consumer, no contention); we just pipe. All buttons (incl. Share),
// rumble, and Xbox One glyphs work natively.
#include <libusb.h>
#include <dlfcn.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <unistd.h>
#include <pthread.h>

#define PA_VID   0x20d6   // PowerA (override with FAKEPAD_VID / FAKEPAD_PID)
#define PA_PID   0x2062
#define ONE_VID  0x045e   // Microsoft Xbox One S controller — SDL maps this to XBOXONE
#define ONE_PID  0x02ea

static FILE *L;
static void lg(const char *f, ...){ if(!L)return; va_list a; va_start(a,f); vfprintf(L,f,a); va_end(a); fputc('\n',L); fflush(L); }

// ---- real libusb (our private connection to the physical pad) ----
static void *R;
#define P(n) static __typeof__(n) *p_##n;
P(libusb_init) P(libusb_exit) P(libusb_get_device_list) P(libusb_free_device_list)
P(libusb_get_device_descriptor) P(libusb_open) P(libusb_close) P(libusb_claim_interface)
P(libusb_release_interface) P(libusb_interrupt_transfer)

static unsigned pa_vid = PA_VID, pa_pid = PA_PID;

// ---- fake device/handle sentinels ----
static int FAKE_DEV, FAKE_HANDLE;
#define FDEV ((libusb_device*)&FAKE_DEV)
#define FHND ((libusb_device_handle*)&FAKE_HANDLE)

// ---- IN backlog: raw GIP packets read from the real pad, awaiting SDL ----
#define RING 256
typedef struct { int len; unsigned char data[64]; } pkt_t;
static pkt_t ring[RING]; static int r_head, r_tail;
static pthread_mutex_t rlk = PTHREAD_MUTEX_INITIALIZER;
static libusb_device_handle *gh;              // real pad handle (owned by reader thread)
static int pad_open;

static void ring_push(const unsigned char *d, int n){
    pthread_mutex_lock(&rlk);
    int nx = (r_head + 1) % RING;
    if (nx == r_tail) r_tail = (r_tail + 1) % RING;  // drop oldest on overflow
    ring[r_head].len = n > 64 ? 64 : n;
    memcpy(ring[r_head].data, d, ring[r_head].len);
    r_head = nx;
    pthread_mutex_unlock(&rlk);
}
static int ring_pop(unsigned char *out, int cap){
    pthread_mutex_lock(&rlk);
    if (r_head == r_tail){ pthread_mutex_unlock(&rlk); return -1; }
    int n = ring[r_tail].len; if (n > cap) n = cap;
    memcpy(out, ring[r_tail].data, n);
    r_tail = (r_tail + 1) % RING;
    pthread_mutex_unlock(&rlk);
    return n;
}

// ---- reader thread: open real pad, relay its IN stream into the ring ----
static void *reader(void *unused){
    (void)unused;
    libusb_context *ic = NULL; p_libusb_init(&ic);
    for(;;){
        gh = NULL;
        libusb_device **list; ssize_t n = p_libusb_get_device_list(ic, &list);
        for (ssize_t i=0;i<n;i++){ struct libusb_device_descriptor d;
            if (!p_libusb_get_device_descriptor(list[i],&d) && d.idVendor==pa_vid && d.idProduct==pa_pid){
                if (!p_libusb_open(list[i],&gh)) break;
            }
        }
        p_libusb_free_device_list(list,1);
        if (!gh){ usleep(500000); continue; }
        p_libusb_claim_interface(gh,0);
        pad_open = 1; lg("[relay] real pad open, relaying GIP");
        unsigned char buf[64];
        for(;;){
            int got=0; int rr=p_libusb_interrupt_transfer(gh,0x81,buf,sizeof(buf),&got,1000);
            if (rr==LIBUSB_ERROR_TIMEOUT) continue;
            if (rr!=0){ lg("[relay] read err %d, reopening",rr); break; }
            if (got>0) ring_push(buf,got);
        }
        pad_open = 0; p_libusb_close(gh); gh=NULL; usleep(300000);
    }
    return NULL;
}
static pthread_once_t once = PTHREAD_ONCE_INIT;
static void start_once(void){ pthread_t t; pthread_create(&t,NULL,reader,NULL); pthread_detach(t); lg("[lazy] reader started pid=%d",getpid()); }
static void ensure(void){ pthread_once(&once,start_once); }

// ---- write a GIP packet from SDL to the real pad (OUT relay) ----
static void relay_out(const unsigned char *d, int len){
    if (gh && len>0){ int s=0; p_libusb_interrupt_transfer(gh,0x01,(unsigned char*)d,len,&s,1000); }
}

__attribute__((constructor)) static void boot(void){
    if (getenv("FAKEPAD_DEBUG")) L=fopen("/tmp/fakepad.log","a");
    if (getenv("FAKEPAD_VID")) pa_vid=(unsigned)strtol(getenv("FAKEPAD_VID"),0,16);
    if (getenv("FAKEPAD_PID")) pa_pid=(unsigned)strtol(getenv("FAKEPAD_PID"),0,16);
    lg("=== fakepad (xbox one relay) pid=%d target=%04x:%04x ===",getpid(),pa_vid,pa_pid);
    Dl_info di; dladdr((void*)&boot,&di); char pth[2048]; strncpy(pth,di.dli_fname,sizeof(pth)-1);
    char *s=strrchr(pth,'/'); if(s)strcpy(s+1,"libusb-real.dylib");
    R=dlopen(pth,RTLD_NOW|RTLD_LOCAL); lg("dlopen real=%p",R);
    if(!R){ lg("FATAL: no libusb-real.dylib beside shim"); return; }
#define B(n) p_##n=(void*)dlsym(R,#n);
    B(libusb_init)B(libusb_exit)B(libusb_get_device_list)B(libusb_free_device_list)
    B(libusb_get_device_descriptor)B(libusb_open)B(libusb_close)B(libusb_claim_interface)
    B(libusb_release_interface)B(libusb_interrupt_transfer)
}

// ===================== libusb API presented to SDL =====================
int libusb_init(libusb_context **ctx){ if(ctx)*ctx=(libusb_context*)0x1; ensure(); return 0; }
void libusb_exit(libusb_context *c){ (void)c; }
int libusb_has_capability(uint32_t c){ (void)c; return 0; } // no hotplug -> SDL polls get_device_list

ssize_t libusb_get_device_list(libusb_context *c,libusb_device ***list){ (void)c; ensure();
    libusb_device **a=calloc(2,sizeof(*a)); a[0]=FDEV; a[1]=NULL; *list=a; return 1;
}
void libusb_free_device_list(libusb_device **l,int u){ (void)u; if(l)free(l); }

int libusb_get_device_descriptor(libusb_device *d,struct libusb_device_descriptor *x){
    if(d!=FDEV) return LIBUSB_ERROR_NO_DEVICE;
    memset(x,0,sizeof(*x)); x->bLength=18; x->bDescriptorType=1; x->bcdUSB=0x0200;
    x->bDeviceClass=0xff; x->bDeviceSubClass=0xff; x->bDeviceProtocol=0xff; x->bMaxPacketSize0=64;
    x->idVendor=ONE_VID; x->idProduct=ONE_PID; x->bcdDevice=0x0408; x->bNumConfigurations=1;
    return 0;
}
static struct libusb_endpoint_descriptor eps[2];
static struct libusb_interface_descriptor ifd;
static struct libusb_interface iface;
static struct libusb_config_descriptor cfgd;
int libusb_get_active_config_descriptor(libusb_device *d,struct libusb_config_descriptor **c){
    if(d!=FDEV) return LIBUSB_ERROR_NO_DEVICE;
    memset(eps,0,sizeof(eps)); memset(&ifd,0,sizeof(ifd)); memset(&iface,0,sizeof(iface)); memset(&cfgd,0,sizeof(cfgd));
    eps[0].bLength=7; eps[0].bDescriptorType=5; eps[0].bEndpointAddress=0x81; eps[0].bmAttributes=3; eps[0].wMaxPacketSize=64; eps[0].bInterval=4;
    eps[1].bLength=7; eps[1].bDescriptorType=5; eps[1].bEndpointAddress=0x01; eps[1].bmAttributes=3; eps[1].wMaxPacketSize=64; eps[1].bInterval=4;
    ifd.bLength=9; ifd.bDescriptorType=4; ifd.bInterfaceNumber=0; ifd.bNumEndpoints=2;
    ifd.bInterfaceClass=0xff; ifd.bInterfaceSubClass=0x47; ifd.bInterfaceProtocol=0xd0; ifd.endpoint=eps;
    iface.altsetting=&ifd; iface.num_altsetting=1;
    cfgd.bLength=9; cfgd.bDescriptorType=2; cfgd.wTotalLength=0x20; cfgd.bNumInterfaces=1; cfgd.bConfigurationValue=1; cfgd.bmAttributes=0x80; cfgd.MaxPower=250; cfgd.interface=&iface;
    *c=&cfgd; return 0;
}
int libusb_get_config_descriptor(libusb_device *d,uint8_t i,struct libusb_config_descriptor **c){ (void)i; return libusb_get_active_config_descriptor(d,c); }
void libusb_free_config_descriptor(struct libusb_config_descriptor *c){ (void)c; }

uint8_t libusb_get_bus_number(libusb_device *d){ (void)d; return 1; }
uint8_t libusb_get_device_address(libusb_device *d){ (void)d; return 42; }
int libusb_get_port_numbers(libusb_device *d,uint8_t *p,int n){ (void)d; if(n>0){p[0]=1;return 1;} return LIBUSB_ERROR_OVERFLOW; }
libusb_device *libusb_get_device(libusb_device_handle *h){ (void)h; return FDEV; }

int libusb_open(libusb_device *d,libusb_device_handle **h){ if(d!=FDEV)return LIBUSB_ERROR_NO_DEVICE; *h=FHND; return 0; }
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
struct libusb_transfer *libusb_alloc_transfer(int iso){ return calloc(1,sizeof(struct libusb_transfer)+iso*sizeof(struct libusb_iso_packet_descriptor)); }
void libusb_free_transfer(struct libusb_transfer *t){ if(t)free(t); }

#define MAXQ 64
static struct libusb_transfer *q[MAXQ]; static int qn=0; static pthread_mutex_t qlk=PTHREAD_MUTEX_INITIALIZER;

int libusb_submit_transfer(struct libusb_transfer *t){
    if((t->endpoint&0x80)==0){ // OUT: SDL -> real pad, complete now
        relay_out(t->buffer,t->length);
        t->actual_length=t->length; t->status=LIBUSB_TRANSFER_COMPLETED;
    }
    pthread_mutex_lock(&qlk); if(qn<MAXQ)q[qn++]=t; pthread_mutex_unlock(&qlk);
    return 0;
}
int libusb_cancel_transfer(struct libusb_transfer *t){
    pthread_mutex_lock(&qlk); for(int i=0;i<qn;i++)if(q[i]==t)t->status=LIBUSB_TRANSFER_CANCELLED; pthread_mutex_unlock(&qlk); return 0;
}
static int handle(void){
    usleep(1000);
    struct libusb_transfer *done[MAXQ]; int dn=0;
    pthread_mutex_lock(&qlk);
    int keep=0;
    for(int i=0;i<qn;i++){ struct libusb_transfer *t=q[i];
        if(t->status==LIBUSB_TRANSFER_CANCELLED){ done[dn++]=t; continue; }
        if(t->endpoint&0x80){ // IN: fill from ring if a packet is available
            int n=ring_pop(t->buffer, t->length);
            if(n<0){ q[keep++]=t; continue; }     // nothing yet, keep pending
            t->actual_length=n; t->status=LIBUSB_TRANSFER_COMPLETED; done[dn++]=t;
        } else { done[dn++]=t; }                   // OUT already completed
    }
    qn=keep; pthread_mutex_unlock(&qlk);
    for(int i=0;i<dn;i++) if(done[i]->callback) done[i]->callback(done[i]);
    return 0;
}
int libusb_handle_events(libusb_context *c){ (void)c; return handle(); }
int libusb_handle_events_completed(libusb_context *c,int *x){ (void)c;(void)x; return handle(); }
int libusb_handle_events_timeout(libusb_context *c,struct timeval *tv){ (void)c;(void)tv; return handle(); }
int libusb_handle_events_timeout_completed(libusb_context *c,struct timeval *tv,int *x){ (void)c;(void)tv;(void)x; return handle(); }
void libusb_interrupt_event_handler(libusb_context *c){ (void)c; }

int libusb_interrupt_transfer(libusb_device_handle *h,unsigned char ep,unsigned char *data,int len,int *xfer,unsigned int to){
    (void)h;
    if(ep&0x80){ // sync IN: wait briefly for a packet
        for(int i=0;i<((int)to/2+1);i++){ int n=ring_pop(data,len); if(n>=0){ if(xfer)*xfer=n; return 0; } usleep(2000); }
        if(xfer)*xfer=0; return LIBUSB_ERROR_TIMEOUT;
    }
    relay_out(data,len); if(xfer)*xfer=len; return 0;
}
int libusb_bulk_transfer(libusb_device_handle *h,unsigned char ep,unsigned char *d,int len,int *xfer,unsigned int to){ return libusb_interrupt_transfer(h,ep,d,len,xfer,to); }
int libusb_hotplug_register_callback(libusb_context*c,int e,int f,int v,int p,int dc,libusb_hotplug_callback_fn cb,void*ud,libusb_hotplug_callback_handle*hd){ (void)c;(void)e;(void)f;(void)v;(void)p;(void)dc;(void)cb;(void)ud; if(hd)*hd=1; return 0; }
void libusb_hotplug_deregister_callback(libusb_context*c,libusb_hotplug_callback_handle h){ (void)c;(void)h; }

// redirect SDL's dlopen("libusb-1.0.0.dylib") to us
static void *my_dlopen(const char *path,int mode){ if(path&&strstr(path,"libusb-1.0.0.dylib")){ Dl_info di; dladdr((void*)&my_dlopen,&di); return dlopen(di.dli_fname,mode); } return dlopen(path,mode); }
__attribute__((used)) static struct{const void*a,*b;} _ip __attribute__((section("__DATA,__interpose")))={(const void*)my_dlopen,(const void*)dlopen};
