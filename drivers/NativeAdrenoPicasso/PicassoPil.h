/* SPDX-License-Identifier: MIT */
#ifndef PICASSO_PIL_H
#define PICASSO_PIL_H
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif
/* Shared by GPU ZAP and MPSS. Hold the lock across the complete PIL transaction,
 * including RPMh votes. A suspended SCM session must not interleave with another
 * client. These entry points do not acquire the lock recursively. */
int picasso_pil_lock(void);
void picasso_pil_unlock(void);
void picasso_pil_block_locked(void);
unsigned picasso_pil_crypto_locked(void);
struct picasso_pil_reply { uint32_t transport, status; uint64_t session; };
struct picasso_pil_reply picasso_pil_call_locked(uint32_t function, uint32_t info,
    uint64_t arg0, uint64_t arg1, uint64_t arg2);
#ifdef __cplusplus
}
#endif
#endif
