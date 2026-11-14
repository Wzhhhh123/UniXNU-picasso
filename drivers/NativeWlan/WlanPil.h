// SPDX-License-Identifier: BSD-2-Clause-Patent
// Bounded command DB and MMIO sequences. Stock Picasso sources at
// MiCode/Xiaomi_Kernel_OpenSource 236f9eb092623c2bc69fa2e88eb647ad30562049:
// cmd-db.c, rpmh-rsc.c, clk-rpmh.c, msm_qmp.c and qcom_ipcc.c.
#ifndef PICASSO_WLAN_PIL_H
#define PICASSO_WLAN_PIL_H
#include <stdint.h>
#include <stddef.h>
struct wlan_pil_resource { uint32_t address; uint16_t levels[16]; unsigned count; };
static inline uint16_t wlan_pil_u16(const volatile uint8_t *p) { return p[0] | (uint16_t)p[1] << 8; }
static inline uint32_t wlan_pil_u32(const volatile uint8_t *p) { return wlan_pil_u16(p) | (uint32_t)wlan_pil_u16(p+2) << 16; }
static inline int wlan_pil_lookup(const volatile uint8_t *db, size_t size,
    const char *name, struct wlan_pil_resource *out) {
    if (!db || !name || !out || size < 144 || wlan_pil_u32(db+4) != 0x0c0330db) return -22;
    uint8_t key[8] = {0}; unsigned n = 0;
    while (n < 8 && name[n]) { key[n] = (uint8_t)name[n]; ++n; }
    if (!n || (n == 8 && name[n])) return -22;
    for (unsigned s = 0; s < 8; ++s) {
        const volatile uint8_t *slave = db+8+16*s;
        if (!wlan_pil_u16(slave)) continue;
        size_t headers = 144u+wlan_pil_u16(slave+2), data = 144u+wlan_pil_u16(slave+4);
        unsigned count = wlan_pil_u16(slave+6);
        if (headers > size || count > (size-headers)/24) return -22;
        for (unsigned i = 0; i < count; ++i) {
            const volatile uint8_t *e = db+headers+24*i;
            unsigned j = 0; for (; j < 8 && e[j] == key[j]; ++j) {}
            if (j != 8) continue;
            unsigned len = wlan_pil_u16(e+20); size_t aux = data+wlan_pil_u16(e+22);
            if ((len & 1) || len > 32 || aux > size || len > size-aux) return -22;
            struct wlan_pil_resource r = {0, {0}, 0}; r.address = wlan_pil_u32(e+16);
            if (!r.address || wlan_pil_u16(slave) != 3) return -22; // ARC
            for (unsigned k = 0; k < len/2; ++k) {
                uint16_t v = wlan_pil_u16(db+aux+2*k);
                if (k && !v) break;
                if (k && v <= r.levels[k-1]) return -22;
                r.levels[r.count++] = v;
            }
            *out = r; return 0;
        }
    }
    return -2;
}
static inline int wlan_pil_level(const struct wlan_pil_resource *r, unsigned level) {
    if (!r || !r->count || r->count > 16) return -22;
    for (unsigned i = 0; i < r->count; ++i) if (r->levels[i] >= level) return (int)i;
    return -22;
}
// Banks: 0=apps DRV2, 1=AOP message RAM, 2=IPCC. write must include a DSB.
struct wlan_pil_io {
    void *context;
    uint32_t (*read)(void *, unsigned, uint32_t);
    void (*write)(void *, unsigned, uint32_t, uint32_t);
    void (*delay)(void *, unsigned);
};
static inline uint32_t wlan_pil_read(const struct wlan_pil_io *io,unsigned b,uint32_t o) { return io->read(io->context,b,o); }
static inline void wlan_pil_write(const struct wlan_pil_io *io,unsigned b,uint32_t o,uint32_t v) { io->write(io->context,b,o,v); }
static inline int wlan_pil_wait(const struct wlan_pil_io *io,unsigned b,uint32_t o,uint32_t v,unsigned us) {
    for (unsigned t=0; t<us; t+=100) {
        if (wlan_pil_read(io,b,o)==v) return 0;
        io->delay(io->context,100);
    }
    return wlan_pil_read(io,b,o)==v ? 0 : -110;
}
// TCS0 is one of the two ACTIVE TCSs in Picasso's DT. Never overwrite a busy
// command and never clear a timed-out command whose completion is uncertain.
static inline int wlan_pil_vote(const struct wlan_pil_io *io,uint32_t address,uint32_t value) {
    uint32_t base=0xd00, major=(wlan_pil_read(io,0,0)>>16)&255;
    uint32_t wait=major>=3 ? 0x20 : 0x10, ctrl=wait+4, status=wait+8, enable=wait+12;
    uint32_t msg=major>=3 ? 0x34 : 0x30;
    if (!address || !wlan_pil_read(io,0,base+status)) return -16;
    uint32_t irq=wlan_pil_read(io,0,base);
    wlan_pil_write(io,0,base,irq&~1u);
    uint32_t c=wlan_pil_read(io,0,base+ctrl)&~(1u<<24);
    wlan_pil_write(io,0,base+ctrl,c);
    wlan_pil_write(io,0,base+ctrl,c&~(1u<<16));
    wlan_pil_write(io,0,base+enable,0);
    wlan_pil_write(io,0,base+8,1);
    wlan_pil_write(io,0,base+msg,8u|(1u<<8)|(1u<<16));
    wlan_pil_write(io,0,base+msg+4,address);
    wlan_pil_write(io,0,base+msg+8,value);
    wlan_pil_write(io,0,base+wait,1);
    wlan_pil_write(io,0,base+enable,1);
    wlan_pil_write(io,0,base+ctrl,1u<<16);
    (void)wlan_pil_read(io,0,base+ctrl);
    wlan_pil_write(io,0,base+ctrl,(1u<<16)|(1u<<24));
    (void)wlan_pil_read(io,0,base+ctrl);
    int rc=-110;
    for (unsigned t=0;t<1000;++t) {
        io->delay(io->context,100);
        if (wlan_pil_read(io,0,base+status) &&
            (wlan_pil_read(io,0,base+msg+12)&0x10100)==0x10100) { rc=0; break; }
    }
    if (!rc) {
        wlan_pil_write(io,0,base+8,1);
        wlan_pil_write(io,0,base+ctrl,1u<<16);
        wlan_pil_write(io,0,base+ctrl,0);
        wlan_pil_write(io,0,base+enable,0);
        wlan_pil_write(io,0,base+wait,0);
    }
    wlan_pil_write(io,0,base,irq);
    return rc;
}
static inline void wlan_pil_ring(const struct wlan_pil_io *io) { wlan_pil_write(io,2,0x0c,0); }
static inline int wlan_pil_aop(const struct wlan_pil_io *io,int on) {
    const uint32_t up=0xffff;
    if (wlan_pil_read(io,1,0)!=0x4d41494c || wlan_pil_read(io,1,4)!=1) return -22;
    uint32_t box=wlan_pil_read(io,1,0x38), size=wlan_pil_read(io,1,0x34);
    uint32_t remote=wlan_pil_read(io,1,0x20), rsize=wlan_pil_read(io,1,0x1c);
    if ((box&3) || box<64 || box>4096 || size<68 || size>4096-box ||
        (remote&3) || remote<64 || remote>4096 || rsize>4096-remote ||
        (box<remote+rsize && remote<box+size)) return -22;
    if (wlan_pil_read(io,1,0x0c)!=up) return -11;
    wlan_pil_write(io,1,0x10,up);
    wlan_pil_write(io,1,0x24,up); wlan_pil_ring(io);
    int rc=wlan_pil_wait(io,1,0x28,up,1000000); if (rc) return rc;
    wlan_pil_write(io,1,0x2c,up); wlan_pil_ring(io);
    rc=wlan_pil_wait(io,1,0x14,up,1000000); if (rc) return rc;
    wlan_pil_write(io,1,0x18,up); wlan_pil_ring(io);
    rc=wlan_pil_wait(io,1,0x30,up,1000000); if (rc) return rc;
    if (wlan_pil_read(io,1,box)) return -16;
    const char *text=on ? "{class: image, res: load_state, name: modem, val: on}" :
                         "{class: image, res: load_state, name: modem, val: off}";
    uint8_t data[64]={0}; unsigned i=0; for (;text[i];++i) data[i]=(uint8_t)text[i];
    for (i=0;i<64;i+=4) wlan_pil_write(io,1,box+4+i,wlan_pil_u32(data+i));
    wlan_pil_write(io,1,box,64); wlan_pil_ring(io);
    // A timed-out message stays owned by the mailbox. Do not overwrite/retract it.
    return wlan_pil_wait(io,1,box,0,1000000);
}
#endif
