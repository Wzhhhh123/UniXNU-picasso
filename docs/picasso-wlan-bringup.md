# Picasso WLAN bring-up

This driver does not yet scan, associate or carry packets. It performs a bounded,
read-only SMEM survey once during the existing PR13 initialization. It does not
start MPSS, alter modem NV, allocate remote SMEM items or create a network interface.

## Sources and board identity

- [UniXNU network tutorial](https://unixnu.com/porting/network): separate board,
  firmware/transport and OS network integration. Its Sheng WCN7850 PCIe/ath12k
  sequence is not the Picasso WCN399x integrated SNOC/ath10k sequence.
- [Raphael reference, aa215933](https://github.com/HeliumStudio-Dev/hackios-xnu-kernel/commit/aa215933e3e4904aa77ee82f1b7b5600d048e5a1):
  MPSS PAS 4, SMEM/SMP2P/GLINK/QRTR/QMI and ath10k SNOC. That revision reports
  successful connections but also unstable operation and failed Wi-Fi re-enable.
- Picasso stock `drivers/soc/qcom/smem.c`, `smp2p.c` and the live device tree:
  SMEM `0x80900000`, size `0x200000`; MPSS remote PID 1; incoming/outgoing SMP2P
  IDs 435/428. `WlanSmem.h` independently implements the documented wire layout.

Use Picasso's existing `WlanBoard.h`/`WlanBoard.cpp` for regulators, interrupts,
SID `0xc0` and MSA `0x8ba00000`/2 MiB. Raphael's MSA `0x8bc00000`/1.5 MiB and its
CE interrupt numbering must not replace these board values.

## Current checkpoint

`PICASSO_WLAN smem` reports SMEM v12 partition geometry and `radio_started=0`.
`PICASSO_WLAN smp2p` reports lookup/parse status for the two items and the public
`wlan` state bits. Private partition selection and global fallback follow stock
`qcom_smem_get`: use global only if the private partition is absent.

The parser bounds partition and item counts, lengths, alignment and arithmetic,
checks wire identities, and rejects changing metadata instead of waiting on it.
This is an observation without the remote allocation lock, not a transport API.
No report implies firmware or radio readiness.

Legacy `sheng-wlan=1` now reports that WCSS MMIO is deferred until firmware mission
mode; it validates only static rail descriptions. Unpowered WCSS register reads
can reset the device, as documented by the reference. Stages 2–5 remain explicit
legacy diagnostic opt-ins and are not enabled by the normal boot arguments.

## Work still required

1. Qualify Picasso MPSS signed firmware, board data and private NV inputs; add
   controlled firmware access without distributing private blobs.
2. Implement board-specific PMIC/clock ownership, secure load, shared-memory
   permissions and ordered shutdown/recovery.
3. Implement SMP2P/GLINK/QRTR/QMI and required RMTFS/TFTP/PD/PDR/DMS services, then
   prove WLFW firmware readiness and the Picasso MSA/SMMU mapping.
4. Integrate ath10k SNOC, all 12 CE interrupts, DMA/cache ownership and worker
   progress; prove scanning before attempting authentication or data traffic.
5. Connect the selected iOS Apple80211 ABI and a real interface/MAC; only then
   enable the dependent Wi-Fi launch jobs and test DHCP, DNS, traffic and rejoin.

`scripts/check-picasso-wlan.py` executes production wire readers and existing
firmware/handshake fixtures under normal and sanitizer builds, and compiles the
complete probe body for ARM64 using explicit API stand-ins. It is not an XNU SDK
build or device qualification. About-page network waits must be diagnosed through
configd/InterfaceNamer independently; enabling `wifid` alone cannot repair them.

## gpu72-8 device checkpoint (2026-11-14)

The tested GL build records source baseline `1c9ebeb17577610ad620bb73941d27844dac0392`,
build commit `20dc3cfb2730baa5cf1cc3953eb05f7038778d29`, and kernel SHA-256
`d9f164fdaf7bb943ca561d0f398edaacf54854bf7aeb70c2f540fe5d07d4bdea`.
Its normal payload has `wifi=0`. The temporary `wifi-stage1` image keeps that
kernel, Data seed, GL addon and trust cache unchanged, removes the automatic
home gesture argument, and adds only `sheng-wlan=1`. It is a diagnostic image,
not a WLAN implementation or a build of the source fix below.

The captured `wifi-stage1-console.log` reports:

```text
PICASSO_WLAN smem error=0 partition=fb000 size=7c000 free=20..7c000 radio_started=0
PICASSO_WLAN smp2p item=435 found=-2 parsed=-2 wlan=0
PICASSO_WLAN smp2p item=428 found=-2 parsed=-2 wlan=0
PICASSO_WLAN stage1 WCSS MMIO deferred: MPSS/WLFW mission mode unavailable
PICASSO_GPU scm: is-call-avail init a0=00000000 a1=00000001 a2=00000000 a3=00000000
PICASSO_GPU scm: std is-call-avail a0=00000000 a1=00000001
PICASSO_GPU zap: pas init a0=00000000 a1=00000000 a2=00000000 a3=00000000
PICASSO_GPU zap: pas mem-setup a0=00000000 a1=00000000
PICASSO_GPU zap: pas auth-and-reset a0=00000000 a1=00000000
```

These observations disprove a blanket claim that SCM/PAS is absent or rejected
in this build. They establish the GPU secure-load path, not WLAN mission mode.
The empty private SMEM partition has no WLAN handshake items. PR13 still supplies
`native_wlan_firmware_size = 0`; `hsLoad`, the QMI request/wait callbacks and the
data/interface path remain unimplemented. A successful stage-1 static check does
not establish a working radio. The GPU probe's PAS IDs 13 and 1 do not qualify
the board-specific MPSS PAS ID; SMEM remote PID 1 is a different namespace.

Review also found a latent interrupted-SCM argument bug in `Adreno620Cp.cpp`:
the sixth C++ argument populated x5, so passing the resume token there left x6
zero. The fix supplies the unused x5 argument explicitly, then passes the full
token as the seventh argument. This follows the A6 quirk and x0=1 continuation
in [Linux qcom_scm-smc.c](https://github.com/torvalds/linux/blob/v6.12/drivers/firmware/qcom/qcom_scm-smc.c).
`a0=1` means an interrupted SCM call, not proof of a TrustZone rejection.

`scripts/check-picasso-scm-resume.py` exercises the actual source call with five
tokens including high 64-bit values and compiles the unmodified raw SMC shim for
ARM64. The original call fails; the fixed call passes. The WLAN wire-reader,
sanitizer and ARM64 probe-body checks pass. Full XNU compilation is unverified:
this Windows checkout lacks the recorded `IOPlatformExpert.cpo.json` build
recipe and assembled SDK build tree. The temporary device test uses the original
kernel; it does not validate the fixed interrupted-call branch on hardware.

## WiFi/SCM cloud build

The `codex/picasso-wifi-scm` build carries the x6 fix and corrects the modem
probe to use `WLAN_MPSS_PAS_ID=4`. Both checked-in Picasso DTBs identify
`/soc/qcom,mss@4080000` as `qcom,firmware-name="modem"`, `qcom,pas-id=<4>`;
`/soc/qcom,lpass@3000000` is ADSP, PAS 1. `check-picasso-wlan-pas.py` parses both
DTBs to guard that distinction, and the SCM fixture executes the actual query
expression against a recording transport. This query remains read-only and
does not start MPSS or WLAN.

Compose retains the two matching GL/GLH payloads and also produces
`gpu72-wifi-scm`: normal GL addon/trust plus `sheng-wlan=1`. All three use the
newly compiled kernel and the exact saved #481 Setup seed. The diagnostic
payload retains `wifi=0` because the firmware/transport/data path described
above is not implemented. A successful build is not a working WiFi interface.

The source guard permits only the reviewed WiFi/SCM/capture files over
`cd1066e703e7c023767dfa7be54d5e2109658087` and records their hashes. Device and
seed geometry, iOS ABI inputs, firmware pins and GL/GLH pair hashes remain
checked. Compose runs the SCM and WLAN host checks before building XNU; a
Windows job tests capture reconnect/partial-write behavior separately. Kernel,
payload and boot hashes and the build commit are recorded with the artifacts.
