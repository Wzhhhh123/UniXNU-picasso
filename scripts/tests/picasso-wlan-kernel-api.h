// SPDX-License-Identifier: BSD-2-Clause-Patent
// Explicit host-owned API stand-ins; not an XNU SDK or hardware qualification.
#include <stdint.h>
#include <stddef.h>
extern "C" void *memset(void *, int, size_t);
extern "C" void *memcpy(void *, const void *, size_t);
extern "C" int memcmp(const void *, const void *, size_t);
extern "C" int printf(const char *, ...) __attribute__((format(printf, 1, 2)));
extern "C" int snprintf(char *, size_t, const char *, ...) __attribute__((format(printf, 3, 4)));
struct SHA256_CTX { uint64_t opaque[16]; };
extern "C" void SHA256_Init(SHA256_CTX *);
extern "C" void SHA256_Update(SHA256_CTX *, const void *, size_t);
extern "C" void SHA256_Final(uint8_t *, SHA256_CTX *);
extern "C" bool PE_parse_boot_argn(const char *, void *, int);
class IOMemoryMap { public: uintptr_t getVirtualAddress(); };
class IODeviceMemory {
public:
    static IODeviceMemory *withRange(uint64_t, uint64_t);
    IOMemoryMap *map(unsigned);
    void release();
};
enum { kIOMapAnywhere = 1, kIOMapInhibitCache = 2, OSNoRetain = 0 };
template<class T> class OSSharedPtr {
    T *pointer;
public:
    OSSharedPtr() : pointer(nullptr) {}
    OSSharedPtr(T *value, int) : pointer(value) {}
    explicit operator bool() const { return pointer != nullptr; }
    T *operator->() const { return pointer; }
};
