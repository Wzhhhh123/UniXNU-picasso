// SPDX-License-Identifier: BSD-2-Clause-Patent
// Opt-in MPSS secure loader. Firmware comes only from the local modem partition.
// WCSS is never mapped here. PAS success is not WLFW/network readiness.
#include <IOKit/IOLib.h>
#include <IOKit/IODeviceMemory.h>
#include <IOKit/IOMemoryDescriptor.h>
#include <libkern/c++/OSSharedPtr.h>
#include <libkern/crypto/sha2.h>
#include <pexpert/pexpert.h>
#include <kern/thread.h>
#include <string.h>
#include "WlanMpss.h"
#include "WlanBoard.h"
#include "WlanFirmware.h"
#include "WlanStorage.h"
#include "WlanSmem.h"
#include "WlanPil.h"
#include "../NativeAdrenoPicasso/PicassoPil.h"

namespace {
struct Region {
    OSSharedPtr<IOMemoryMap> keep;
    uintptr_t address = 0;
    bool map(uint64_t phys, uint64_t bytes) {
        IODeviceMemory *m = IODeviceMemory::withRange(phys, bytes);
        if (!m) return false;
        keep = OSSharedPtr<IOMemoryMap>(m->map(kIOMapAnywhere | kIOMapInhibitCache), OSNoRetain);
        m->release();
        address = keep ? (uintptr_t)keep->getVirtualAddress() : 0;
        return address != 0;
    }
};
static Region banks[3], database, metadata, memory, smem;
static struct wlan_fat *filesystem;
static const struct wlan_mbn *image;
static unsigned directory, requested;
alignas(8) static uint8_t chunk[65536];
static bool uncertain, authenticated;
static uint32_t mmread(void *, unsigned bank, uint32_t offset) {
    return *(volatile uint32_t *)(banks[bank].address+offset);
}
static void mmwrite(void *, unsigned bank, uint32_t offset, uint32_t value) {
    *(volatile uint32_t *)(banks[bank].address+offset) = value;
    __asm__ volatile("dsb sy" ::: "memory");
}
static void delay(void *, unsigned us) { IODelay(us); }
static const struct wlan_pil_io io = {nullptr, mmread, mmwrite, delay};
static int smread(void *, uint32_t offset, void *out, uint32_t len) {
    for (uint32_t i=0;i<len;++i) ((uint8_t *)out)[i]=*(volatile uint8_t *)(smem.address+offset+i);
    __asm__ volatile("dmb sy" ::: "memory");
    return 0;
}
static int findItem(unsigned id, struct wlan_smem_item *item) {
    struct wlan_smem_io reader = {nullptr,smread};
    struct wlan_smem_partition p = {};
    int rc=wlan_smem_partition(&reader,0,1,&p);
    if (rc==-2) rc=wlan_smem_partition(&reader,0xfffe,0xfffe,&p);
    if (!rc) rc=wlan_smem_find(&reader,&p,(uint16_t)id,item);
    return rc;
}
static void remoteState() {
    struct wlan_smem_item item = {};
    int rc=findItem(WLAN_SMEM_IN,&item);
    printf("PICASSO_WLAN mpss smp2p rc=%d offset=%x bytes=%u\n",rc,item.offset,item.size);
    if (!rc && item.size>=20) {
        uint8_t h[20]; smread(nullptr,item.offset,h,20);
        unsigned total=wlan_sm16(h+12),valid=wlan_sm16(h+14);
        if (wlan_sm32(h)==0x504d5324 && h[4]==1 && total<=16 && valid<=total &&
            wlan_sm16(h+8)==1 && wlan_sm16(h+10)==0 && item.size>=20+20*total) {
            for (unsigned i=0;i<valid;++i) {
                uint8_t e[20]; char name[17]; smread(nullptr,item.offset+20+20*i,e,20);
                for (unsigned j=0;j<16;++j) name[j]=e[j]>=32 && e[j]<127 ? (char)e[j] : '.';
                name[16]=0;
                printf("PICASSO_WLAN mpss entry=%s value=%08x\n",name,wlan_sm32(e+16));
            }
        }
    }
    // Crash reason lives in global SMEM even when SMP2P uses a private partition.
    struct wlan_smem_io reader={nullptr,smread}; struct wlan_smem_partition p={};
    rc=wlan_smem_partition(&reader,0xfffe,0xfffe,&p);
    if (!rc) rc=wlan_smem_find(&reader,&p,421,&item);
    if (!rc && item.size) {
        char reason[193]; unsigned count=item.size<192 ? item.size : 192;
        smread(nullptr,item.offset,reason,count); reason[count]=0;
        for (unsigned i=0;i<count && reason[i];++i) if ((uint8_t)reason[i]<32 || (uint8_t)reason[i]>126) reason[i]='.';
        if (reason[0]) printf("PICASSO_WLAN mpss crash_reason=%s\n",reason);
    }
}
static int pas(const char *name, uint32_t command, uint32_t info, uint64_t a=0,uint64_t b=0) {
    printf("PICASSO_WLAN mpss pas begin=%s id=4 arg1=%llx arg2=%llx\n",name,
        (unsigned long long)a,(unsigned long long)b);
    struct picasso_pil_reply r=picasso_pil_call_locked(0x42000200u|command,info,WLAN_MPSS_PAS_ID,a,b);
    printf("PICASSO_WLAN mpss pas end=%s transport=%08x status=%08x\n",name,r.transport,r.status);
    if (r.transport==1) uncertain=true; // Suspended session is still owned by TZ.
    return r.transport || r.status ? WLAN_ERR_IO : 0;
}
// Device memory disallows DC ZVA and unaligned STP. Use aligned scalar stores;
// memcpy is used only to read an unaligned word from ordinary staging RAM.
static void storeMemory(uintptr_t target,const uint8_t *data,uint32_t size) {
    while (size && (target&7)) { *(volatile uint8_t *)target++=data ? *data++ : 0; --size; }
    while (size>=8) {
        uint64_t word=0; if (data) { memcpy(&word,data,8); data+=8; }
        *(volatile uint64_t *)target=word; target+=8; size-=8;
    }
    while (size--) *(volatile uint8_t *)target++=data ? *data++ : 0;
}
static int stageSegments() {
    SHA256_CTX sha; SHA256_Init(&sha); uint64_t payload=0,zeroed=0;
    for (unsigned i=0;i<image->segments;++i) {
        const struct wlan_mbn_segment *s=&image->seg[i];
        if (s->type!=1 || i==image->hash_index) continue;
        struct wlan_file part={}; int rc=0;
        if (s->filesz) {
            char name[16]; snprintf(name,sizeof name,"modem.b%02u",i);
            rc=wlan_fat_find(filesystem,directory,name,&part);
            if (!rc && (part.directory || part.size!=s->filesz)) rc=-22;
            if (rc) return rc;
        }
        uintptr_t target=memory.address+(s->paddr-WLAN_MPSS_BASE);
        for (uint32_t offset=0;offset<s->filesz;) {
            uint32_t count=s->filesz-offset; if (count>sizeof chunk) count=sizeof chunk;
            rc=wlan_fat_read(filesystem,&part,offset,count,chunk); if (rc) return rc;
            SHA256_Update(&sha,chunk,count);
            storeMemory(target+offset,chunk,count); offset+=count; payload+=count;
        }
        storeMemory(target+s->filesz,nullptr,s->memsz-s->filesz); zeroed+=s->memsz-s->filesz;
        __asm__ volatile("dsb sy" ::: "memory");
        printf("PICASSO_WLAN mpss staged ph=%u pa=%x file=%u zero=%u total=%llu\n",i,s->paddr,s->filesz,
            s->memsz-s->filesz,(unsigned long long)payload);
    }
    uint8_t digest[32]; char hex[65]; const char *digits="0123456789abcdef";
    SHA256_Final(digest,&sha);
    for (unsigned i=0;i<32;++i) { hex[2*i]=digits[digest[i]>>4]; hex[2*i+1]=digits[digest[i]&15]; }
    hex[64]=0;
    printf("PICASSO_WLAN mpss staged payload=%llu zeroed=%llu sha256=%s auth=not-run\n",
        (unsigned long long)payload,(unsigned long long)zeroed,hex);
    return 0;
}
static int load() {
    const boot_args *args=(const boot_args *)PE_state.bootArgs;
    if (!args || args->physBase < WLAN_MPSS_BASE+WLAN_MPSS_SIZE) {
        printf("PICASSO_WLAN mpss memory ownership check failed\n"); return -22;
    }
    unsigned loads=0; uint64_t payload=0,span=0;
    int rc=wlan_mbn_memory_plan(image,WLAN_MPSS_BASE,WLAN_MPSS_SIZE,&loads,&payload,&span);
    if (rc || !image->qcom_signed || image->size>WLAN_MPSS_METADATA_SIZE) return -22;
    if (!smem.map(WLAN_SMEM_BASE,WLAN_SMEM_SIZE)) return -12;
    struct wlan_smem_item existing={}; rc=findItem(WLAN_SMEM_IN,&existing);
    // Never overwrite a possibly running modem. A fresh cold boot has no item435.
    if (rc!=-2) { printf("PICASSO_WLAN mpss preexisting SMP2P rc=%d; refusing overwrite\n",rc); remoteState(); return -16; }
    if (!database.map(0x80860000,0x20000) || !banks[0].map(0x18220000,0x10000) ||
        !banks[1].map(0x0c300000,0x1000) || !banks[2].map(0x00408000,0x1000) ||
        !metadata.map(WLAN_MPSS_METADATA_BASE,WLAN_MPSS_METADATA_SIZE) ||
        !memory.map(WLAN_MPSS_BASE,WLAN_MPSS_SIZE)) return -12;
    const char *names[]={"cx.lvl","mss.lvl","xo.lvl"};
    struct wlan_pil_resource resources[3]={}; uint32_t values[3]={};
    for (unsigned i=0;i<3;++i) {
        rc=wlan_pil_lookup((volatile uint8_t *)database.address,0x20000,names[i],&resources[i]);
        int level=rc ? rc : (i==2 ? 3 : wlan_pil_level(&resources[i],384));
        if (rc || level<0) { printf("PICASSO_WLAN mpss resource=%s rc=%d level=%d\n",names[i],rc,level); return -22; }
        values[i]=(uint32_t)level;
        printf("PICASSO_WLAN mpss resource=%s addr=%x vote=%u\n",names[i],resources[i].address,values[i]);
    }
    rc=wlan_pil_aop(&io,1);
    printf("PICASSO_WLAN mpss aop=on rc=%d\n",rc); if (rc) return rc;
    for (unsigned i=0;i<3 && !rc;++i) {
        rc=wlan_pil_vote(&io,resources[i].address,values[i]);
        if (rc==-110) uncertain=true;
        printf("PICASSO_WLAN mpss vote=%s rc=%d\n",names[i],rc);
    }
    // Votes are retained for this diagnostic boot. There is no shared AP rail
    // aggregator yet; dropping cx.lvl or XO to zero could break other consumers.
    if (!rc && picasso_pil_crypto_locked()!=7) { rc=-5; uncertain=true; }
    bool initialized=false,auth=false;
    if (!rc) {
        storeMemory(metadata.address,nullptr,WLAN_MPSS_METADATA_SIZE);
        storeMemory(metadata.address,image->data,(uint32_t)image->size);
        __asm__ volatile("dsb sy" ::: "memory");
        rc=pas("init",1,0x82,WLAN_MPSS_METADATA_BASE); initialized=!rc;
    }
    if (!rc) rc=pas("mem-setup",2,3,WLAN_MPSS_BASE,span);
    if (!rc) rc=stageSegments();
    if (!rc && requested>=3) { rc=pas("auth-reset",5,1); auth=!rc; }
    if (auth) {
        authenticated=true;
        printf("PICASSO_WLAN mpss authenticated=1 services_ready=0 network_ready=0 proxy_votes=retained\n");
    }
    // Stage2 stops after staging. Failed transactions shut down only after a
    // completed init; never issue another SCM call while a session is suspended.
    if (initialized && !auth && !uncertain) {
        int shutdown=pas("shutdown",6,1); if (shutdown) uncertain=true;
    }
    if (!auth && !uncertain) {
        int off=wlan_pil_aop(&io,0); printf("PICASSO_WLAN mpss aop=off rc=%d\n",off);
    }
    printf("PICASSO_WLAN mpss complete rc=%d auth=%d uncertain=%d mappings=retained votes=retained\n",rc,auth,uncertain);
    return rc;
}
static void worker(void *, wait_result_t) {
    int rc=picasso_pil_lock();
    if (!rc) {
        rc=load();
        if (uncertain) picasso_pil_block_locked();
        picasso_pil_unlock();
    }
    if (authenticated) for (unsigned i=0;i<15;++i) { IOSleep(2000); remoteState(); }
    printf("PICASSO_WLAN mpss worker exit=%d stage=%u\n",rc,requested);
    thread_terminate(current_thread());
}
}
void native_wlan_mpss_start(unsigned stage,struct wlan_fat *fs,unsigned dir,const struct wlan_mbn *mbn) {
    if (requested || stage<2 || stage>3) return;
    requested=stage; filesystem=fs; directory=dir; image=mbn;
    thread_t thread=nullptr;
    if (kernel_thread_start(worker,nullptr,&thread)==KERN_SUCCESS) thread_deallocate(thread);
    else printf("PICASSO_WLAN mpss thread creation failed\n");
}
