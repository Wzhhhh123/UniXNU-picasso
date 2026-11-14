// SPDX-License-Identifier: BSD-2-Clause-Patent
#ifndef PICASSO_WLAN_MPSS_H
#define PICASSO_WLAN_MPSS_H
struct wlan_fat;
struct wlan_mbn;
// 2: PAS init/setup + all segments, then shutdown. 3: additionally auth/reset.
// Asynchronous, one attempt per boot; data remains owned by WlanProbe statics.
void native_wlan_mpss_start(unsigned stage, struct wlan_fat *, unsigned directory,
    const struct wlan_mbn *);
#endif
