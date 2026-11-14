// SPDX-License-Identifier: BSD-2-Clause-Patent
// Bounded, allocation-free readers for Picasso's modem GPT partition and FAT16.
// Only 4096-byte sectors are supported, as measured on the device. No writes.
#ifndef PICASSO_WLAN_STORAGE_H
#define PICASSO_WLAN_STORAGE_H
#include <stdint.h>
#include <stddef.h>
#include <string.h>

struct wlan_disk {
    void *ctx;
    int (*read)(void *, uint64_t, uint32_t, void *); // 0 success; exact whole blocks
    uint64_t blocks;
};
struct wlan_partition { uint64_t first, blocks; };
static inline uint16_t wf16(const uint8_t *p) { return (uint16_t)(p[0] | (p[1] << 8)); }
static inline uint32_t wf32(const uint8_t *p) {
    return (uint32_t)wf16(p) | ((uint32_t)wf16(p + 2) << 16);
}
static inline uint64_t wf64(const uint8_t *p) { return wf32(p) | ((uint64_t)wf32(p + 4) << 32); }
static inline int wf_read(const struct wlan_disk *d, uint64_t lba, uint32_t n, void *out) {
    if (!d || !d->read || !out || !n || lba >= d->blocks || n > d->blocks - lba) return -22;
    return d->read(d->ctx, lba, n, out);
}
static inline uint32_t wf_crc(const uint8_t *p, unsigned n, int header) {
    uint32_t crc = ~0u;
    for (unsigned i = 0; i < n; ++i) {
        crc ^= header && i >= 16 && i < 20 ? 0 : p[i];
        for (unsigned b = 0; b < 8; ++b) crc = (crc >> 1) ^ (0xedb88320u & (0u - (crc & 1)));
    }
    return ~crc;
}
static inline int wf_used(const uint8_t *p) {
    unsigned v = 0; for (unsigned i = 0; i < 16; ++i) v |= p[i]; return v != 0;
}
// scratch is caller-owned, at least 16384 bytes; never shared with root GPT state.
static inline int wlan_modem_partition(const struct wlan_disk *d, uint8_t *scratch,
                                        struct wlan_partition *out) {
    if (!out || !scratch || !d || d->blocks < 8 || d->blocks > UINT32_MAX) return -22;
    memset(out, 0, sizeof *out);
    int rc = wf_read(d, 1, 1, scratch); if (rc) return rc;
    uint32_t hs = wf32(scratch + 12), count = wf32(scratch + 80), crc = wf32(scratch + 88);
    uint64_t first = wf64(scratch + 40), last = wf64(scratch + 48), entries = wf64(scratch + 72);
    if (memcmp(scratch, "EFI PART", 8) || wf32(scratch + 8) != 0x10000 || hs < 92 || hs > 4096 ||
        wf32(scratch + 20) || wf64(scratch + 24) != 1 || wf64(scratch + 32) != d->blocks - 1 ||
        wf32(scratch + 84) != 128 || !count || count > 128 ||
        wf_crc(scratch, hs, 1) != wf32(scratch + 16)) return -22;
    uint32_t n = (count * 128 + 4095) / 4096;
    if (entries < 2 || entries > 64 || first < entries + n || last < first ||
        last >= d->blocks - 1 - n) return -22;
    rc = wf_read(d, entries, n, scratch); if (rc) return rc;
    if (wf_crc(scratch, count * 128, 0) != crc) return -22;
    for (unsigned i = 0; i < count; ++i) {
        const uint8_t *p = scratch + i * 128;
        if (!wf_used(p)) continue;
        uint64_t a = wf64(p + 32), b = wf64(p + 40);
        if (a < first || b < a || b > last) return -22;
        for (unsigned j = 0; j < i; ++j) {
            const uint8_t *q = scratch + j * 128;
            if (wf_used(q) && a <= wf64(q + 40) && wf64(q + 32) <= b) return -22;
        }
        static const uint8_t name[] = {'m',0,'o',0,'d',0,'e',0,'m',0,0,0};
        if (!memcmp(p + 56, name, sizeof name)) {
            if (out->blocks || b - a + 1 > (512u << 20) / 4096) return -22;
            out->first = a; out->blocks = b - a + 1;
        }
    }
    return out->blocks ? 0 : -2;
}

struct wlan_file { uint32_t cluster, size; uint8_t directory; };
struct wlan_fat {
    struct wlan_disk disk;
    uint32_t total, reserved, fatsize, root, roots, data, clusters, spc, fats;
    uint32_t cached;
    uint8_t cache[4096], visited[8192];
};
static inline int wf_sector(struct wlan_fat *f, uint32_t lba) {
    if (f->cached == lba) return 0;
    int rc = wf_read(&f->disk, lba, 1, f->cache);
    f->cached = rc ? UINT32_MAX : lba;
    return rc;
}
static inline int wlan_fat_open(struct wlan_fat *f, const struct wlan_disk *d) {
    if (!f || !d) return -22;
    memset(f, 0, sizeof *f); f->disk = *d; f->cached = UINT32_MAX;
    int rc = wf_sector(f, 0); if (rc) return rc;
    const uint8_t *p = f->cache;
    if (wf16(p + 510) != 0xaa55 || wf16(p + 11) != 4096) return -22;
    f->spc = p[13]; f->reserved = wf16(p + 14); f->fats = p[16]; f->roots = wf16(p + 17);
    f->total = wf16(p + 19) ? wf16(p + 19) : wf32(p + 32); f->fatsize = wf16(p + 22);
    if (!f->spc || f->spc > 32 || (f->spc & (f->spc - 1)) || !f->reserved ||
        !f->fats || f->fats > 2 || !f->fatsize || !f->roots || f->roots > 4096 ||
        !f->total || f->total > d->blocks) return -22;
    f->root = f->reserved + f->fats * f->fatsize;
    f->data = f->root + (f->roots * 32 + 4095) / 4096;
    if (f->data >= f->total) return -22;
    f->clusters = (f->total - f->data) / f->spc;
    if (f->clusters < 4085 || f->clusters >= 65525 ||
        (f->clusters + 2) * 2 > f->fatsize * 4096) return -22;
    return 0;
}
static inline int wf_cluster(const struct wlan_fat *f, uint32_t c) {
    return c >= 2 && c < f->clusters + 2 && c < 0xfff0;
}
static inline int wf_visit(struct wlan_fat *f, uint32_t c) {
    if (!wf_cluster(f, c) || (f->visited[c / 8] & (1u << (c % 8)))) return -22;
    f->visited[c / 8] |= (uint8_t)(1u << (c % 8)); return 0;
}
static inline int wf_next(struct wlan_fat *f, uint32_t c, uint32_t *next) {
    if (!wf_cluster(f, c)) return -22;
    int rc = wf_sector(f, f->reserved + c / 2048); if (rc) return rc;
    *next = wf16(f->cache + (c % 2048) * 2);
    if (f->fats == 2) {
        rc = wf_sector(f, f->reserved + f->fatsize + c / 2048); if (rc) return rc;
        if (*next != wf16(f->cache + (c % 2048) * 2)) return -22;
    }
    return *next >= 0xfff8 || wf_cluster(f, *next) ? 0 : -22;
}
static inline unsigned wf_lower(unsigned c) { return c >= 'A' && c <= 'Z' ? c + 32 : c; }
static inline int wf_equal(const char *a, const char *b) {
    while (*a && *b && wf_lower((uint8_t)*a) == wf_lower((uint8_t)*b)) { ++a; ++b; }
    return !*a && !*b;
}
static inline uint8_t wf_lfn_checksum(const uint8_t *p) {
    uint8_t s = 0;
    for (unsigned i = 0; i < 11; ++i) s = (uint8_t)(((s & 1) << 7) + (s >> 1) + p[i]);
    return s;
}
// ASCII long names up to 65 characters, with sequence/type/checksum validation.
// A damaged LFN cannot select an unrelated short entry. At most 4096 entries.
static inline int wlan_fat_find(struct wlan_fat *f, uint32_t dir, const char *name,
                                struct wlan_file *out) {
    if (!f || !name || !*name || !out) return -22;
    memset(out, 0, sizeof *out); memset(f->visited, 0, sizeof f->visited);
    char longname[66] = {0}; unsigned remaining = 0, checksum = 0, seen = 0;
    uint32_t c = dir;
    do {
        if (c && wf_visit(f, c)) return -22;
        uint32_t start = c ? f->data + (c - 2) * f->spc : f->root;
        uint32_t sectors = c ? f->spc : (f->roots * 32 + 4095) / 4096;
        for (uint32_t s = 0; s < sectors; ++s) {
            int rc = wf_sector(f, start + s); if (rc) return rc;
            for (unsigned at = 0; at < 4096; at += 32) {
                if (++seen > 4096) return -22;
                const uint8_t *p = f->cache + at;
                if (!p[0]) return -2;
                if (p[0] == 0xe5) { remaining = 0; longname[0] = 0; continue; }
                if (p[11] == 15) {
                    unsigned seq = p[0] & 31;
                    if (p[0] & 64) { memset(longname, 0, sizeof longname); remaining = seq; checksum = p[13]; }
                    if (!seq || seq > 5 || seq != remaining || p[13] != checksum || p[12] || wf16(p + 26) || (p[0] & 0xa0)) {
                        remaining = 0; longname[0] = 0; continue;
                    }
                    static const uint8_t offsets[] = {1,3,5,7,9,14,16,18,20,22,24,28,30};
                    int valid = 1;
                    for (unsigned k = 0; k < 13; ++k) {
                        unsigned v = wf16(p + offsets[k]);
                        if (v != 0 && v != 0xffff && (v < 32 || v > 126 || v == '/')) valid = 0;
                        longname[(seq - 1) * 13 + k] = v == 0xffff ? 0 : (char)v;
                    }
                    --remaining;
                    if (!valid) { remaining = 0; longname[0] = 0; }
                    continue;
                }
                char shortname[13] = {0}; unsigned n = 0;
                for (unsigned k = 0; k < 8 && p[k] != ' '; ++k) shortname[n++] = (char)p[k];
                if (p[8] != ' ') { shortname[n++] = '.'; for (unsigned k = 8; k < 11 && p[k] != ' '; ++k) shortname[n++] = (char)p[k]; }
                int match = wf_equal(shortname, name) || (!remaining && longname[0] &&
                            wf_lfn_checksum(p) == checksum && wf_equal(longname, name));
                remaining = 0; longname[0] = 0;
                if (p[11] & 8 || !match) continue;
                if (wf16(p + 20)) return -22;
                out->cluster = wf16(p + 26); out->size = wf32(p + 28); out->directory = !!(p[11] & 16);
                if ((out->size || out->directory) && !wf_cluster(f, out->cluster)) return -22;
                if (out->size > (uint64_t)f->clusters * f->spc * 4096) return -22;
                return 0;
            }
        }
        if (!c) break;
        int rc = wf_next(f, c, &c); if (rc) return rc;
    } while (c < 0xfff8);
    return -2;
}
static inline int wlan_fat_read(struct wlan_fat *f, const struct wlan_file *file,
                                uint32_t offset, uint32_t bytes, void *out) {
    if (!f || !file || !out || file->directory || offset > file->size || bytes > file->size - offset) return -22;
    if (!bytes) return 0;
    memset(f->visited, 0, sizeof f->visited);
    uint32_t c = file->cluster, cb = f->spc * 4096, skip = offset / cb, within = offset % cb;
    for (;;) {
        int rc = wf_visit(f, c); if (rc) return rc;
        if (!skip) break;
        --skip; rc = wf_next(f, c, &c); if (rc) return rc;
    }
    uint8_t *dst = (uint8_t *)out;
    while (bytes) {
        uint32_t take = 4096 - (within % 4096); if (take > bytes) take = bytes;
        int rc = wf_sector(f, f->data + (c - 2) * f->spc + within / 4096); if (rc) return rc;
        memcpy(dst, f->cache + within % 4096, take);
        dst += take; bytes -= take; within += take;
        if (within == cb && bytes) {
            rc = wf_next(f, c, &c); if (rc) return rc;
            rc = wf_visit(f, c); if (rc) return rc;
            within = 0;
        }
    }
    return 0;
}
#endif
