// SPDX-License-Identifier: BSD-2-Clause-Patent
#include <cassert>
#include <cstdio>
#include <cstring>
#include <vector>
#include "WlanPil.h"
#include "store-memory.inc"
struct Write { unsigned b,o,v; };
struct Device {
    std::vector<uint32_t> r = std::vector<uint32_t>(3*0x10000/4);
    std::vector<Write> writes;
    unsigned us=0,major=2,handshake=3;
    bool triggered=false,complete=true,consume=true;
    uint32_t &at(unsigned b,unsigned o) { assert(b<3 && o<0x10000 && !(o&3)); return r[b*(0x10000/4)+o/4]; }
    unsigned ctrl() const { return 0xd00+(major>=3 ? 0x24 : 0x14); }
    unsigned status() const { return ctrl()+4; }
    unsigned cmdstatus() const { return 0xd00+(major>=3 ? 0x40 : 0x3c); }
    explicit Device(unsigned version=2) : major(version) {
        at(0,0)=major<<16;at(0,status())=1;at(0,0xd00)=0xa5;
        at(1,0)=0x4d41494c;at(1,4)=1;at(1,0x0c)=0xffff;
        at(1,0x34)=128;at(1,0x38)=0x100;at(1,0x1c)=128;at(1,0x20)=0x200;
    }
};
static uint32_t rd(void *p,unsigned b,uint32_t o) { return ((Device *)p)->at(b,o); }
static void wr(void *p,unsigned b,uint32_t o,uint32_t v) {
    auto &d=*(Device *)p; d.writes.push_back({b,o,v});d.at(b,o)=v;
    if (b==0 && o==d.ctrl() && (v&(1u<<24))) { d.triggered=true;d.at(0,d.status())=0;d.at(0,d.cmdstatus())=0; }
    if (b==2 && o==0xc) {
        assert(v==0);
        if (d.handshake>=1) d.at(1,0x28)=d.at(1,0x24);
        if (d.handshake>=2) d.at(1,0x14)=d.at(1,0x2c);
        if (d.handshake>=3) d.at(1,0x30)=d.at(1,0x18);
        if (d.consume && d.at(1,0x100)==64) d.at(1,0x100)=0;
    }
}
static void wait(void *p,unsigned us) {
    auto &d=*(Device *)p;d.us+=us;
    if (d.triggered && d.complete) { d.at(0,d.status())=1;d.at(0,d.cmdstatus())=0x10100; }
}
static wlan_pil_io ops(Device &d) { return {&d,rd,wr,wait}; }
static void put16(uint8_t *p,unsigned n) { p[0]=n;p[1]=n>>8; }
static void put32(uint8_t *p,unsigned n) { put16(p,n);put16(p+2,n>>16); }
int main() {
    uint8_t db[512]={};put32(db+4,0x0c0330db);put16(db+8,3);put16(db+12,24);put16(db+14,1);
    memcpy(db+144,"cx.lvl",6);put32(db+160,0x30000);put16(db+164,12);
    unsigned levels[]={0,16,48,128,256,384};for (unsigned i=0;i<6;++i) put16(db+168+2*i,levels[i]);
    wlan_pil_resource r={};assert(wlan_pil_lookup(db,sizeof db,"cx.lvl",&r)==0);
    assert(r.address==0x30000 && wlan_pil_level(&r,384)==5 && wlan_pil_level(&r,385)==-22);
    assert(wlan_pil_level(&r,129)==4 && wlan_pil_lookup(db,sizeof db,"mss.lvl",&r)==-2);
    for (unsigned n=0;n<180;++n) assert(wlan_pil_lookup(db,n,"cx.lvl",&r)==-22);
    put16(db+164,33);assert(wlan_pil_lookup(db,sizeof db,"cx.lvl",&r)==-22);put16(db+164,12);
    put16(db+178,128);assert(wlan_pil_lookup(db,sizeof db,"cx.lvl",&r)==-22);
    for (unsigned v : {2u,3u}) {
        Device d(v);auto io=ops(d);assert(wlan_pil_vote(&io,0x30000,5)==0);
        assert(d.triggered && d.us==100 && d.at(0,0xd00)==0xa5 && d.at(0,d.ctrl())==0);
        unsigned msg=0xd00+(v>=3 ? 0x34 : 0x30);assert(d.at(0,msg+4)==0x30000 && d.at(0,msg+8)==5);
        Device busy(v);auto b=ops(busy);busy.at(0,busy.status())=0;
        assert(wlan_pil_vote(&b,0x30000,5)==-16 && busy.writes.empty());
        Device timeout(v);auto t=ops(timeout);timeout.complete=false;
        assert(wlan_pil_vote(&t,0x30000,5)==-110 && timeout.us==100000);
        assert(timeout.at(0,timeout.ctrl())==(1u<<16|1u<<24));
    }
    Device q;auto qi=ops(q);assert(wlan_pil_aop(&qi,1)==0);
    assert(!memcmp((char *)&q.at(1,0x104),"{class: image, res: load_state, name: modem, val: on}",51));
    assert(wlan_pil_aop(&qi,0)==0);
    for (unsigned phase=0;phase<3;++phase) {
        Device d;d.handshake=phase;auto io=ops(d);assert(wlan_pil_aop(&io,1)==-110);assert(!d.at(1,0x100));
    }
    Device bad;bad.at(1,0x38)=4092;auto bi=ops(bad);assert(wlan_pil_aop(&bi,1)==-22 && bad.writes.empty());
    Device occupied;occupied.at(1,0x100)=12;auto oi=ops(occupied);assert(wlan_pil_aop(&oi,1)==-16 && occupied.at(1,0x100)==12);
    Device timeout;timeout.consume=false;auto ti=ops(timeout);assert(wlan_pil_aop(&ti,1)==-110 && timeout.at(1,0x100)==64);
    alignas(8) uint8_t target[144],data[128];for (unsigned i=0;i<128;++i) data[i]=(uint8_t)(i+1);
    for (unsigned off=0;off<8;++off) for (unsigned len=0;len<=128;++len) {
        memset(target,0x5a,sizeof target);storeMemory((uintptr_t)(target+off),data,len);
        for (unsigned j=0;j<sizeof target;++j) assert(target[j]==(j>=off && j<off+len ? data[j-off] : 0x5a));
        storeMemory((uintptr_t)(target+off),nullptr,len);
        for (unsigned j=0;j<sizeof target;++j) assert(target[j]==(j>=off && j<off+len ? 0 : 0x5a));
    }
    puts("PASS: ARC levels/bounds, both RSC layouts, busy/timeout retention, QMP handshake/mailbox bounds, scalar staging/BSS");
}
