// SPDX-License-Identifier: BSD-2-Clause-Patent
// Production GPT/FAT/MDT readers against synthetic corruptions and optional
// device firmware. No proprietary fixture or device data is stored in the tree.
#include "WlanStorage.h"
#include "WlanFirmware.h"
#include "WlanBoard.h"
#include <stdio.h>
#include <stdlib.h>
#define CHECK(x) do { if (!(x)) { fprintf(stderr,"FAIL line %d: %s\n",__LINE__,#x); exit(1); } } while (0)
static void put16(uint8_t *p, uint16_t v) { p[0]=(uint8_t)v; p[1]=(uint8_t)(v>>8); }
static void put32(uint8_t *p, uint32_t v) { put16(p,(uint16_t)v); put16(p+2,(uint16_t)(v>>16)); }
static void put64(uint8_t *p, uint64_t v) { put32(p,(uint32_t)v); put32(p+4,(uint32_t)(v>>32)); }
static int memory_read(void *ctx, uint64_t lba, uint32_t blocks, void *out) {
    memcpy(out, (uint8_t *)ctx + lba*4096, blocks*4096u); return 0;
}
static int file_read(void *ctx, uint64_t lba, uint32_t blocks, void *out) {
    FILE *f=(FILE *)ctx;
    if (fseek(f,(long)(lba*4096),SEEK_SET)) return -5;
    return fread(out,4096,blocks,f)==blocks ? 0 : -5;
}
static void header_crc(uint8_t *p) { put32(p+16,wf_crc(p,92,1)); }
static void gpt_crc(uint8_t *disk) {
    put32(disk+4096+88,wf_crc(disk+8192,256,0)); header_crc(disk+4096);
}
static void gpt_tests(void) {
    uint8_t disk[16384]={0}, scratch[16384]; uint8_t *h=disk+4096,*p=disk+8192;
    memcpy(h,"EFI PART",8); put32(h+8,0x10000); put32(h+12,92);
    put64(h+24,1); put64(h+32,99999); put64(h+40,6); put64(h+48,99990);
    put64(h+72,2); put32(h+80,2); put32(h+84,128);
    p[0]=1; put64(p+32,100); put64(p+40,82019);
    for(unsigned i=0;i<5;++i)p[56+2*i]=(uint8_t)"modem"[i];
    gpt_crc(disk); struct wlan_disk d={disk,memory_read,100000}; struct wlan_partition part;
    CHECK(!wlan_modem_partition(&d,scratch,&part)); CHECK(part.first==100 && part.blocks==81920);
    h[16]^=1; CHECK(wlan_modem_partition(&d,scratch,&part)==-22); h[16]^=1;
    p[0]^=2; CHECK(wlan_modem_partition(&d,scratch,&part)==-22); p[0]^=2;
    memcpy(p+128,p,128); gpt_crc(disk); CHECK(wlan_modem_partition(&d,scratch,&part)==-22);
    memset(p+128,0,128); put64(p+40,100000); gpt_crc(disk); CHECK(wlan_modem_partition(&d,scratch,&part)==-22);
    put64(p+40,82019); gpt_crc(disk); d.blocks=90000; CHECK(wlan_modem_partition(&d,scratch,&part)==-22);
    puts("PASS GPT: CRC, capacity, overlap/duplicate modem, partition bounds");
}
static void mdt_tests(void) {
    uint8_t mdt[9420]={0}; struct wlan_mbn m;
    memcpy(mdt,"\177ELF\1\1\1",7); put16(mdt+16,2); put16(mdt+18,164); put32(mdt+20,1);
    put32(mdt+28,52); put16(mdt+40,52); put16(mdt+42,32); put16(mdt+44,35);
    put32(mdt+52+16,1172); put32(mdt+52+24,0x7000000);
    uint8_t *hash=mdt+84; put32(hash+4,0x1000); put32(hash+16,8248); put32(hash+20,0x3000); put32(hash+24,0x2200000);
    const uint8_t *parts[64]={0}; size_t sizes[64]={0}; uint8_t payload[2]={1,2};
    for(unsigned i=2;i<35;++i) {
        uint8_t *p=mdt+52+i*32; put32(p,1); put32(p+4,0x5000000+i*4096);
        put32(p+12,(uint32_t)WLAN_MPSS_BASE+(i-2)*8192); put32(p+16,i<32 ? 2 : 0);
        put32(p+20,4096); put32(p+24,0x8000006);
        if(i<32){parts[i]=payload;sizes[i]=2;}
    }
    CHECK(!wlan_mbn_parse_header(&m,mdt,sizeof mdt)); CHECK(m.segments==35 && m.qcom_signed && m.hash_index==1);
    CHECK(m.hash_offset==1172 && m.hash_bytes==8248 && m.image_extent>64u*1024*1024);
    unsigned loads=0; uint64_t bytes=0,span=0;
    CHECK(!wlan_mbn_memory_plan(&m,WLAN_MPSS_BASE,WLAN_MPSS_SIZE,&loads,&bytes,&span));
    CHECK(loads==33 && bytes==60 && span==32*8192+4096);
    CHECK(wlan_mbn_verify_hashes(&m,mdt,sizeof mdt)==-45);
    CHECK(!wlan_mbn_parse_split(&m,mdt,sizeof mdt,parts,sizes,35));
    CHECK(wlan_mbn_verify_hashes_split(&m,parts,sizes,35)==-45);
    sizes[2]=1; CHECK(wlan_mbn_parse_split(&m,mdt,sizeof mdt,parts,sizes,35)==-22); sizes[2]=2;
    CHECK(wlan_mbn_parse_header(&m,mdt,sizeof mdt-1)==-22);
    put16(mdt+44,65); CHECK(wlan_mbn_parse_header(&m,mdt,sizeof mdt)==-22); put16(mdt+44,35);
    CHECK(!wlan_mbn_parse_header(&m,mdt,sizeof mdt)); m.seg[2].paddr=(uint32_t)WLAN_MPSS_BASE-1;
    CHECK(wlan_mbn_memory_plan(&m,WLAN_MPSS_BASE,WLAN_MPSS_SIZE,&loads,&bytes,&span)==-22);
    CHECK(!wlan_mbn_parse_header(&m,mdt,sizeof mdt)); m.seg[2].filesz=4097;
    CHECK(wlan_mbn_memory_plan(&m,WLAN_MPSS_BASE,WLAN_MPSS_SIZE,&loads,&bytes,&span)==-22);
    CHECK(!wlan_mbn_parse_header(&m,mdt,sizeof mdt)); m.seg[3].paddr=m.seg[2].paddr;
    CHECK(wlan_mbn_memory_plan(&m,WLAN_MPSS_BASE,WLAN_MPSS_SIZE,&loads,&bytes,&span)==-22);
    CHECK(!wlan_mbn_parse_header(&m,mdt,sizeof mdt)); m.seg[34].paddr=(uint32_t)(WLAN_MPSS_BASE+WLAN_MPSS_SIZE-1);
    CHECK(wlan_mbn_memory_plan(&m,WLAN_MPSS_BASE,WLAN_MPSS_SIZE,&loads,&bytes,&span)==-22);
    put32(mdt+52+24,WLAN_MBN_QCOM_HASH); CHECK(wlan_mbn_parse_header(&m,mdt,sizeof mdt)==-22);
    puts("PASS MDT: 35 headers, packed signature, >64MiB extent, BSS, missing part, truncation, overlap, carveout, no false authentication");
}
static void fat_tests(void) {
    uint8_t *disk=calloc(4800,4096); CHECK(disk); struct wlan_fat f; struct wlan_file file;
    struct wlan_disk d={disk,memory_read,4800};
    put16(disk+11,4096); disk[13]=1; put16(disk+14,1); disk[16]=2; put16(disk+17,128);
    put16(disk+19,4800); put16(disk+22,3); put16(disk+510,0xaa55);
    uint8_t *fat=disk+4096,*root=disk+7*4096,*entry=root+32;
    memcpy(entry,"RANDOM~1   ",11); entry[11]=0x20; put16(entry+26,2); put32(entry+28,5000);
    root[0]=0x41; root[11]=15; root[13]=wf_lfn_checksum(entry);
    const unsigned off[]={1,3,5,7,9,14,16,18,20,22,24,28,30};
    for(unsigned i=0;i<13;++i)put16(root+off[i],i<9 ? (uint8_t)"modem.mdt"[i] : i==9 ? 0 : 0xffff);
    put16(fat+4,3); put16(fat+6,0xffff); memcpy(disk+4*4096,fat,3*4096);
    memset(disk+8*4096,0x5a,4096); memset(disk+9*4096,0xa5,4096);
    CHECK(!wlan_fat_open(&f,&d)); CHECK(!wlan_fat_find(&f,0,"modem.mdt",&file));
    uint8_t result[5000]; CHECK(!wlan_fat_read(&f,&file,0,5000,result));
    CHECK(result[0]==0x5a && result[4095]==0x5a && result[4096]==0xa5 && result[4999]==0xa5);
    CHECK(!wlan_fat_read(&f,&file,4090,20,result)); CHECK(result[5]==0x5a && result[6]==0xa5);
    CHECK(wlan_fat_read(&f,&file,4999,2,result)==-22);
    root[13]^=1; f.cached=UINT32_MAX; CHECK(wlan_fat_find(&f,0,"modem.mdt",&file)==-2); root[13]^=1;
    f.cached=UINT32_MAX; CHECK(!wlan_fat_find(&f,0,"modem.mdt",&file));
    put16(fat+4,2); memcpy(disk+4*4096,fat,3*4096); f.cached=UINT32_MAX;
    CHECK(wlan_fat_read(&f,&file,0,5000,result)==-22); // cycle
    put16(fat+4,0xfff7); memcpy(disk+4*4096,fat,3*4096); f.cached=UINT32_MAX;
    CHECK(wlan_fat_read(&f,&file,0,5000,result)==-22); // bad cluster
    put16(fat+4,3); f.cached=UINT32_MAX; CHECK(wlan_fat_read(&f,&file,0,5000,result)==-22); // mirrors disagree
    put16(disk+11,512); CHECK(wlan_fat_open(&f,&d)==-22); put16(disk+11,4096);
    d.blocks=1000; CHECK(wlan_fat_open(&f,&d)==-22);
    free(disk); puts("PASS FAT16: LFN checksum, cross-cluster read, partial read, cycles, bad clusters, mirrors, bounds, sector geometry");
}
static void real_test(const char *path) {
    FILE *disk=fopen(path,"rb"); CHECK(disk); CHECK(!fseek(disk,0,SEEK_END)); long bytes=ftell(disk); CHECK(bytes>0 && !(bytes%4096));
    struct wlan_disk io={disk,file_read,(uint64_t)bytes/4096}; struct wlan_fat f; struct wlan_file dir,mdt,part;
    CHECK(!wlan_fat_open(&f,&io)); CHECK(!wlan_fat_find(&f,0,"image",&dir) && dir.directory);
    CHECK(!wlan_fat_find(&f,dir.cluster,"modem.mdt",&mdt)); CHECK(mdt.size<=16384);
    uint8_t raw[16384]; CHECK(!wlan_fat_read(&f,&mdt,0,mdt.size,raw)); struct wlan_mbn m;
    CHECK(!wlan_mbn_parse_header(&m,raw,mdt.size)); unsigned loads; uint64_t payload,span;
    CHECK(!wlan_mbn_memory_plan(&m,WLAN_MPSS_BASE,WLAN_MPSS_SIZE,&loads,&payload,&span));
    CHECK(m.qcom_signed && m.segments==35 && loads==33 && span==WLAN_MPSS_SIZE);
    unsigned files=0;
    for(unsigned i=0;i<m.segments;++i) {
        if(m.seg[i].type!=1 || i==m.hash_index || !m.seg[i].filesz)continue;
        char name[16]; snprintf(name,sizeof name,"modem.b%02u",i);
        CHECK(!wlan_fat_find(&f,dir.cluster,name,&part)); CHECK(!part.directory && part.size==m.seg[i].filesz); ++files;
        // Traverse each full part, including fragmented FAT chains and the tail.
        uint8_t *content=malloc(part.size); CHECK(content); CHECK(!wlan_fat_read(&f,&part,0,part.size,content)); free(content);
    }
    printf("PASS real modem: mdt=%u headers=%u loads=%u files=%u payload=%llu span=%llx auth=not-run\n",mdt.size,m.segments,loads,files,(unsigned long long)payload,(unsigned long long)span);
    fclose(disk);
}
int main(int argc,char **argv) {
    gpt_tests(); mdt_tests(); fat_tests(); if(argc>=2)real_test(argv[1]);
    if(argc==4) {
        FILE *gpt=fopen(argv[2],"rb"); CHECK(gpt);
        struct wlan_disk io={gpt,file_read,strtoull(argv[3],NULL,0)};
        uint8_t scratch[16384]; struct wlan_partition part;
        CHECK(!wlan_modem_partition(&io,scratch,&part));
        printf("PASS real GPT: modem first=%llu blocks=%llu lun_blocks=%llu\n",
            (unsigned long long)part.first,(unsigned long long)part.blocks,(unsigned long long)io.blocks);
        fclose(gpt);
    }
    return 0;
}
