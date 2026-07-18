// fakepad CrossOver helper (native macOS).
// Reads the GIP pad over libusb, maps input to an XInput gamepad, and publishes
// it to a small file that the Wine-side xinput shim reads (Wine maps macOS / to Z:).
// Also reads back a rumble byte pair the shim writes, and drives the pad motors.
#include <libusb.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define PA_VID 0x20d6
#define PA_PID 0x2062
#define STATE_PATH "/tmp/fakepad_state"   // Z:\tmp\fakepad_state on the Wine side
#define RUMBLE_PATH "/tmp/fakepad_rumble"

// XInput gamepad layout published to the state file (little-endian):
//   u8 connected; u8 pad; u16 buttons; u8 lt; u8 rt; s16 lx; s16 ly; s16 rx; s16 ry; u32 packet
#pragma pack(push,1)
typedef struct { unsigned char connected, _pad; unsigned short buttons; unsigned char lt, rt;
                 short lx, ly, rx, ry; unsigned int packet; } xstate;
#pragma pack(pop)

static libusb_device_handle *gh; static unsigned char gseq[256];
static int dec(const unsigned char*b,int n,unsigned long*v){*v=0;int i;for(i=0;i<n;i++){*v|=(unsigned long)(b[i]&0x7f)<<(i*7);if(!(b[i]&0x80)){i++;break;}}return i;}
static int enc(unsigned long v,unsigned char*b){int i=0;do{unsigned char x=v&0x7f;v>>=7;if(v)x|=0x80;b[i++]=x;}while(v);return i;}
static void snd(unsigned char t,unsigned char fl,unsigned char sq,const unsigned char*p,int n){
    unsigned char b[64]={t,fl,sq};int o=3;o+=enc(n,b+o);if(n)memcpy(b+o,p,n);int s;libusb_interrupt_transfer(gh,0x01,b,o+n,&s,1000);usleep(3000);}
static void initseq(void){unsigned char st[]={0},led[]={0,1,0x14},sec[]={1,0},irr[]={0,0,0};
    snd(5,0x20,++gseq[5],st,1);snd(0x0a,0x20,++gseq[10],led,3);snd(6,0x20,++gseq[6],sec,2);snd(0x0a,0,++gseq[11],irr,3);}

static void publish(xstate *x){ FILE*f=fopen(STATE_PATH".tmp","wb"); if(!f)return; fwrite(x,sizeof*x,1,f); fclose(f); rename(STATE_PATH".tmp",STATE_PATH); }

static void map(const unsigned char*b, xstate*x){
    unsigned short btn=0; unsigned char b0=b[0],b1=b[1];
    if(b1&0x01)btn|=0x0001; if(b1&0x02)btn|=0x0002; if(b1&0x04)btn|=0x0004; if(b1&0x08)btn|=0x0008; // dpad
    if(b0&0x04)btn|=0x0010; if(b0&0x08)btn|=0x0020;                                                   // start/back
    if(b1&0x40)btn|=0x0040; if(b1&0x80)btn|=0x0080;                                                   // stick clicks
    if(b1&0x10)btn|=0x0100; if(b1&0x20)btn|=0x0200;                                                   // LB/RB
    if(b0&0x02)btn|=0x0400;                                                                            // guide
    if(b0&0x10)btn|=0x1000; if(b0&0x20)btn|=0x2000; if(b0&0x40)btn|=0x4000; if(b0&0x80)btn|=0x8000;    // A/B/X/Y
    x->buttons=btn;
    x->lt=((b[2]|(b[3]<<8))&0x3ff)>>2; x->rt=((b[4]|(b[5]<<8))&0x3ff)>>2;
    x->lx=(short)(b[6]|(b[7]<<8));  x->ly=(short)(b[8]|(b[9]<<8));
    x->rx=(short)(b[10]|(b[11]<<8)); x->ry=(short)(b[12]|(b[13]<<8));
    x->connected=1; x->packet++;
}

int main(void){
    libusb_context*c; libusb_init(&c);
    xstate x; memset(&x,0,sizeof x); publish(&x);
    for(;;){
        gh=NULL; libusb_device**l; ssize_t n=libusb_get_device_list(c,&l);
        for(ssize_t i=0;i<n;i++){struct libusb_device_descriptor d;if(!libusb_get_device_descriptor(l[i],&d)&&d.idVendor==PA_VID&&d.idProduct==PA_PID){if(!libusb_open(l[i],&gh))break;}}
        libusb_free_device_list(l,1);
        if(!gh){ x.connected=0; publish(&x); usleep(500000); continue; }
        libusb_claim_interface(gh,0); memset(gseq,0,sizeof gseq);
        snd(4,0x20,++gseq[4],NULL,0);
        int meta=0; unsigned char buf[64]; unsigned long total=0,foff=0;
        for(;;){
            int got=0,r=libusb_interrupt_transfer(gh,0x81,buf,sizeof buf,&got,1000);
            // apply rumble if the shim requested it
            FILE*rf=fopen(RUMBLE_PATH,"rb"); if(rf){unsigned char rb[2]={0,0};if(fread(rb,1,2,rf)==2){unsigned char m[8]={0x0f,0,0,rb[0],rb[1],0xff,0,0};snd(9,0,++gseq[9],m,8);}fclose(rf);remove(RUMBLE_PATH);}
            if(r==LIBUSB_ERROR_TIMEOUT)continue; if(r!=0)break;
            unsigned char ty=buf[0],fl=buf[1],sq=buf[2];int o=3;unsigned long ln;o+=dec(buf+o,got-o,&ln);
            if(ty==0x20&&!(fl&0x20)){ map(buf+o,&x); publish(&x); continue; }
            if(ty==0x02){memset(gseq,0,sizeof gseq);snd(4,0x20,++gseq[4],NULL,0);continue;}
            if(fl&0x80){unsigned long v;o+=dec(buf+o,got-o,&v);
                if(fl&0x40){total=v;foff=ln;}else if(ln==0){if(!meta){meta=1;initseq();}if(fl&0x10){unsigned char a[9]={0,ty,0x20,(unsigned char)foff,foff>>8,foff>>16,foff>>24,0,0};snd(1,0x20,sq,a,9);}continue;}else foff=v+ln;
                if(fl&0x10){unsigned int rem=total-foff;unsigned char a[9]={0,ty,0x20,(unsigned char)foff,foff>>8,foff>>16,foff>>24,(unsigned char)rem,rem>>8};snd(1,0x20,sq,a,9);}
                if(foff>=total&&total&&!meta){meta=1;initseq();} continue; }
        }
        libusb_close(gh); x.connected=0; publish(&x); usleep(300000);
    }
}
