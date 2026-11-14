// SPDX-License-Identifier: BSD-2-Clause-Patent
// Include the unmodified NativeUfs modem adapter; mock only transport/allocation.
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include "WlanStorage.h"
#define CHECK(x) do { if (!(x)) { std::fprintf(stderr,"FAIL %d: %s\n",__LINE__,#x); std::exit(1); } } while (0)
using IOReturn = int;
enum {kIOReturnSuccess=0,kIOReturnNotReady=1};
constexpr unsigned kDataOffset=0;
static uint8_t dma[8], disk[16384];
static bool locked, attention; static unsigned scsiCalls, reads, blockSize=4096;
static struct { void *lock; bool ready, poisoned; uint8_t *virt; } gHost={dma,true,false,dma};
static struct {uint32_t sense;} gRootDiagnostic;
static void IOLockLock(void *) {CHECK(!locked);locked=true;}
static void IOLockUnlock(void *) {CHECK(locked);locked=false;}
static void *IOMalloc(size_t n) {return std::malloc(n);}
static void IOFree(void *p,size_t) {std::free(p);}
#define IOLog(...) std::printf(__VA_ARGS__)
static uint32_t loadBe32(const uint8_t *p) {return (uint32_t(p[0])<<24)|(uint32_t(p[1])<<16)|(uint32_t(p[2])<<8)|p[3];}
static void putBe32(uint8_t *p,uint32_t v) {for(unsigned i=0;i<4;++i)p[i]=uint8_t(v>>(24-8*i));}
static void put32(uint8_t *p,uint32_t v) {for(unsigned i=0;i<4;++i)p[i]=uint8_t(v>>(8*i));}
static IOReturn hostScsi(uint8_t lun,const uint8_t *cdb,unsigned len,bool toHost,uint32_t bytes,uint32_t timeout,uint32_t *scsi) {
    CHECK(locked && lun==4 && timeout==200000); ++scsiCalls; *scsi=0;
    if(cdb[0]==0) {
        CHECK(len==6 && !toHost && !bytes);
        if(attention){attention=false;*scsi=2;gRootDiagnostic.sense=6<<16;return kIOReturnNotReady;}
    } else {
        CHECK(cdb[0]==0x25 && len==10 && toHost && bytes==8);
        putBe32(dma,99999);putBe32(dma+4,blockSize);
    }
    gRootDiagnostic.sense=0;return kIOReturnSuccess;
}
static IOReturn hostRead4k(uint8_t lun,uint64_t lba,uint32_t n,void *out) {
    CHECK(lun==4 && !locked);++reads;
    if(gHost.poisoned)return kIOReturnNotReady;
    if(lba<4) {CHECK(lba+n<=4);std::memcpy(out,disk+lba*4096,n*4096);}
    else {CHECK(lba>=100 && lba+n<=82020);std::memset(out,0x5a,n*4096);}
    return kIOReturnSuccess;
}
#include "modem-adapter.inc"
static void prepare() {
    gModemState=0;gModemPartition={};gHost.poisoned=false;gHost.ready=true;gHost.lock=dma;
    blockSize=4096;attention=false;scsiCalls=reads=0;std::memset(disk,0,sizeof disk);
    uint8_t *h=disk+4096,*p=disk+8192;std::memcpy(h,"EFI PART",8);
    put32(h+8,0x10000);put32(h+12,92);put32(h+24,1);put32(h+32,99999);
    put32(h+40,6);put32(h+48,99990);put32(h+72,2);put32(h+80,2);put32(h+84,128);
    p[0]=1;put32(p+32,100);put32(p+40,82019);for(unsigned i=0;i<5;++i)p[56+2*i]=uint8_t("modem"[i]);
    put32(h+88,wf_crc(p,256,0));put32(h+16,wf_crc(h,92,1));
}
int main() {
    uint64_t blocks=0;uint8_t data[4096];
    prepare();attention=true;CHECK(!native_ufs_modem_open(&blocks));CHECK(blocks==81920 && scsiCalls==3 && reads==2);
    CHECK(!native_ufs_modem_open(&blocks));CHECK(scsiCalls==3 && reads==2);
    CHECK(!native_ufs_modem_read(81919,1,data));CHECK(data[0]==0x5a);
    CHECK(native_ufs_modem_read(81920,1,data)==-22);CHECK(native_ufs_modem_read(81919,2,data)==-22);
    CHECK(native_ufs_modem_read(UINT64_MAX,1,data)==-22);CHECK(native_ufs_modem_read(0,0,data)==-22);
    CHECK(native_ufs_modem_read(0,1,nullptr)==-22);CHECK(reads==3);
    gHost.poisoned=true;CHECK(native_ufs_modem_read(0,1,data)==-5);
    prepare();blockSize=512;CHECK(native_ufs_modem_open(&blocks)==-22 && !blocks && !reads);
    prepare();disk[4096+16]^=1;CHECK(native_ufs_modem_open(&blocks)==-22 && !blocks);
    CHECK(native_ufs_modem_read(0,1,data)==-6);
    prepare();gHost.ready=false;CHECK(native_ufs_modem_open(&blocks)==-6 && !scsiCalls);
    prepare();gHost.lock=nullptr;CHECK(native_ufs_modem_open(&blocks)==-6 && !scsiCalls);
    CHECK(!locked);std::puts("PASS production UFS adapter: read-only command set, UNIT ATTENTION, GPT publication, bounds, no failed-open access, quarantine");
}
