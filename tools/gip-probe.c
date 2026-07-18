#include <libusb.h>
#include <stdio.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#define FLAG_FRAGMENT 0x80
#define FLAG_INIT_FRAG 0x40
#define FLAG_SYSTEM 0x20
#define FLAG_ACME 0x10

static libusb_device_handle *h;
static unsigned char ep_in, ep_out;
static unsigned char seqs[256];

static int dec_varint(const unsigned char *b, int n, unsigned long *v) {
    *v = 0; int i;
    for (i = 0; i < n; i++) { *v |= (unsigned long)(b[i] & 0x7f) << (i * 7); if (!(b[i] & 0x80)) { i++; break; } }
    return i;
}
static int enc_varint(unsigned long v, unsigned char *b) {
    int i = 0;
    do { unsigned char byte = v & 0x7f; v >>= 7; if (v) byte |= 0x80; b[i++] = byte; } while (v);
    return i;
}
static int send_msg(unsigned char type, unsigned char flags, unsigned char seq, const unsigned char *p, int n, const char *what) {
    unsigned char buf[64] = { type, flags, seq };
    int off = 3;
    off += enc_varint(n, buf + off);
    if (n) memcpy(buf + off, p, n);
    int sent = 0;
    int r = libusb_interrupt_transfer(h, ep_out, buf, off + n, &sent, 1000);
    printf("  -> %s r=%d\n", what, r);
    usleep(5000);
    return r;
}
static void ack(unsigned char type, unsigned char flags, unsigned char seq, unsigned long off, unsigned int rem) {
    unsigned char p[9] = { 0x00, type, (unsigned char)(flags & FLAG_SYSTEM),
        (unsigned char)off, (unsigned char)(off >> 8), (unsigned char)(off >> 16), (unsigned char)(off >> 24),
        (unsigned char)rem, (unsigned char)(rem >> 8) };
    send_msg(0x01, FLAG_SYSTEM, seq, p, sizeof(p), "ack");
}
static void init_sequence(void) {
    unsigned char start[] = {0x00};                 // SET_DEVICE_STATE: START
    unsigned char led[] = {0x00, 0x01, 0x14};       // guide LED on, intensity 20
    unsigned char sec[] = {0x01, 0x00};             // security bypass
    unsigned char irr[] = {0x00, 0x00, 0x00};       // initial reports request (vendor)
    send_msg(0x05, FLAG_SYSTEM, ++seqs[0x05], start, sizeof(start), "set-state START");
    send_msg(0x0a, FLAG_SYSTEM, ++seqs[0x0a], led, sizeof(led), "led");
    send_msg(0x06, FLAG_SYSTEM, ++seqs[0x06], sec, sizeof(sec), "security");
    send_msg(0x0a, 0x00, ++seqs[0xaa], irr, sizeof(irr), "initial-reports-request");
}

int main(void) {
    libusb_context *ctx; libusb_init(&ctx);
    h = libusb_open_device_with_vid_pid(ctx, 0x20d6, 0x2062);
    if (!h) { printf("open failed\n"); return 1; }
    libusb_claim_interface(h, 0);
    libusb_device *dev = libusb_get_device(h);
    struct libusb_config_descriptor *cfg; libusb_get_active_config_descriptor(dev, &cfg);
    const struct libusb_interface_descriptor *ifd = &cfg->interface[0].altsetting[0];
    for (int i = 0; i < ifd->bNumEndpoints; i++) {
        unsigned char a = ifd->endpoint[i].bEndpointAddress;
        if (a & 0x80) ep_in = a; else ep_out = a;
    }
    printf("claimed\n");
    // kick things off in case HELLO was missed: request metadata
    send_msg(0x04, FLAG_SYSTEM, ++seqs[0x04], NULL, 0, "metadata-request");

    unsigned long total = 0, frag_off = 0;
    int meta_done = 0, inputs = 0, errors = 0;
    unsigned char lastb[2] = {0xFF, 0xFF};
    unsigned char buf[64];
    time_t t0 = time(NULL);
    while (time(NULL) - t0 < 40) {
        int got = 0;
        int r = libusb_interrupt_transfer(h, ep_in, buf, sizeof(buf), &got, 1200);
        if (r == LIBUSB_ERROR_TIMEOUT) continue;
        if (r != 0) { errors++; printf("READ ERROR %s t=%ld\n", libusb_error_name(r), time(NULL)-t0); if (errors > 3) break; continue; }
        unsigned char type = buf[0], flags = buf[1], seq = buf[2];
        int off = 3; unsigned long len;
        off += dec_varint(buf + off, got - off, &len);
        if (type == 0x20 && !(flags & FLAG_SYSTEM)) {
            inputs++;
            if (buf[off+0] != lastb[0] || buf[off+1] != lastb[1]) {
                printf("INPUT: buttons=%02x %02x (t=%ld)\n", buf[off+0], buf[off+1], time(NULL)-t0);
                lastb[0] = buf[off+0]; lastb[1] = buf[off+1];
            }
            continue;
        }
        if (type == 0x02) { printf("HELLO -> metadata request\n"); send_msg(0x04, FLAG_SYSTEM, ++seqs[0x04], NULL, 0, "metadata-request"); continue; }
        if (flags & FLAG_FRAGMENT) {
            unsigned long v;
            off += dec_varint(buf + off, got - off, &v);
            if (flags & FLAG_INIT_FRAG) { total = v; frag_off = len; }
            else if (len == 0) {
                printf("fragment stream complete (total=%lu)\n", total);
                if (flags & FLAG_ACME) ack(type, flags, seq, frag_off, 0);
                if (!meta_done) { meta_done = 1; printf("metadata done -> init sequence\n"); init_sequence(); }
                continue;
            }
            else frag_off = v + len;
            printf("frag type=%02x flags=%02x len=%lu now_at=%lu/%lu\n", type, flags, len, frag_off, total);
            if (flags & FLAG_ACME) ack(type, flags, seq, frag_off, (unsigned int)(total - frag_off));
            if (frag_off >= total && total && !meta_done) { meta_done = 1; printf("metadata complete -> init sequence\n"); init_sequence(); }
            continue;
        }
        printf("msg type=%02x flags=%02x len=%lu:", type, flags, len);
        for (int i = 0; i < (got > 14 ? 14 : got); i++) printf(" %02x", buf[i]);
        printf("\n");
        if (flags & FLAG_ACME) ack(type, flags, seq, len, 0);
    }
    printf("RESULT: inputs=%d errors=%d meta_done=%d\n", inputs, errors, meta_done);
    libusb_release_interface(h, 0); libusb_close(h); libusb_exit(ctx);
    return 0;
}
