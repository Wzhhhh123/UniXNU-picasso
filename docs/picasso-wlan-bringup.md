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
sanitizer and ARM64 probe-body checks pass. At this local checkpoint, full XNU
compilation was unverified: the Windows checkout lacked the recorded `IOPlatformExpert.cpo.json` build
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

### Cloud run 9 and hardware qualification (2026-11-14)

[Run 9](https://github.com/Wzhhhh123/UniXNU-picasso/actions/runs/43566300855)
successfully built commit `0a31138eeee778df9b79bc70782338ecca5dc181`.
Both the macOS build and Windows capture job passed. This resolves the local
SDK-build limitation above. The run used `fast=true`: SCM/PAS/WLAN and capture
checks ran, while the workflow's optional general artifact checks were skipped.

Verified artifacts:

- Kernel SHA-256: `0e3287e995c7cdd946ed93d600cb11a4ce8591875414f95136c837f26f36a2bf`.
- Kernel UUID: `23EB34B1-9600-368E-8A39-7AC1FD252B51`, matched in the hardware panic.
- `gpu72-wifi-scm.img` SHA-256: `442922ce6a728b58f5ada2de9581a22c48f5e875286bc6f8eb0a0327674aedec`.
- Artifact ZIP SHA-256: `28315a31acc615997d00a8c1237d165bcb2a0827b203e468430694e62c4cdedf`,
  matched the GitHub artifact digest before extraction.

The temporary boot returned `pas-supported(4) a0=00000000 a1=00000000`, with
`radio_started=0`. GPU PAS init, memory setup and auth/reset all returned zero.
The GPU's PAS-supported query also returned zero, so the raw supported-query
value must not be treated as proof that TrustZone rejects MPSS loading. No
interrupted SCM call occurred in this sample; the x6 continuation branch is
covered by the host fixture, not yet exercised on hardware.

A second temporary image changed only `picasso_usb_reset=pshold` to `psci` and
the Android image ID. It retained the same kernel, Data seed and GL addon/trust
pair. Its SHA-256 is `5fcb94d00cb78efd7966e71a5c3b7a3d8121fa5e028a495df87e2f9dbf97c787`.
The user confirmed entry into iOS; the log records EAGL, `glTexImage2D`, `glFlush`,
and DPU `flips=240 maps=3 mapfails=0 fallbacks=0 latch_timeouts=0`. This establishes
that the GL/display path ran after the SCM fix, not a performance improvement
or complete GPU qualification. GLH was built but not hardware-tested.

The PSHOLD panic test exported 149,051 stackshot bytes with CRC32 `4f3fbb52` and
completed the panic flush, but rebooted the installed older kernel instead of
Fastboot. Its subsequent PAS 1 log distinguishes that boot from the tested PAS 4
kernel. The user then manually returned to Fastboot. Thus an IMEM readback and
`PANIC FLUSH COMPLETE` alone do not prove automatic Fastboot entry. See the
[USB console notes](usb-console-bringup.md) for the reset test boundary.

After the user authorized a dedicated fallback in `boot`, a Linux Fastboot relay
was built and temporarily qualified, the existing boot partition was backed up
and hash-verified, and the relay was installed. A final run 9 diagnostic panic
then exported a valid stackshot and returned through that relay to Fastboot in
about 17 seconds. The WiFi diagnostic image itself was never flashed.

WiFi remains unavailable: the read-only SMEM survey still finds no WLAN SMP2P
items and does not start the radio. The firmware loader, QMI/GLINK transport,
SNOC data path and iOS network-interface work listed above remain required.

### Real modem firmware and read-only storage path (2026-11-14)

The phone's modem partition was read through temporary TWRP, mounted read-only.
It is on UFS LUN 4, with 4096-byte logical sectors; Linux's `start=492848`
is in 512-byte units, so the UFS start LBA is 61606. The partition is 81920
UFS blocks (320 MiB). The whole partition SHA-256 matched between device and
host: `1c31b4a755e503ab49b097263ac5088a6d7e2417d8886a3f76e9970fadbc3523`.
Firmware binaries remain local and are not included in Git or cloud artifacts.

The actual `modem.mdt` is 9420 bytes, SHA-256
`426dbc2ff0f626c2edd2faec162fbe38e8f2d730fa806534880584f371bbf3d1`.
It has 35 program headers: PH0 contains the ELF/program headers, PH1 is the
8248-byte Qualcomm signature/hash segment (`p_flags=0x02200000`), and 33
PT_LOAD ranges fit exactly within the DT's `[0x8c000000,0x9b800000)` reservation.
Thirty nonempty `.bXX` files total 85,409,366 bytes; three BSS-only ranges have
no segment file. The previous 24-header/64-MiB limits and synthetic SHA-1-table
assumption could not describe this firmware. Parsing now recognizes the
Qualcomm flag and packed MDT metadata; signature verification explicitly returns
ENOTSUP until PAS authentication is integrated. Parsing is not authentication.

`wlan-fw-read=1` adds a bounded read-only survey independent of legacy stage 2+.
It checks LUN 4 readiness/capacity, the primary GPT CRC and all partition bounds,
then reads only within the partition named `modem`. The FAT16 reader checks
4096-byte BPB geometry, cluster ranges/cycles, mirror agreement when following
chains, and ASCII LFN ordering/checksum (the real `image` directory uses an
unrelated short alias). The probe reads/hashes MDT and checks each required
segment's directory size and memory range. It does not read all payload bytes,
write remote RAM, authenticate/start MPSS, or expose a network interface.

Local host checks cover the production GPT/FAT/MDT code, the actual NativeUfs
adapter with mocked transport, corrupt metadata, missing/truncated parts, BSS,
partition/memory bounds, timeout quarantine propagation, and sanitizers. The
actual local modem image also passes a full read of every nonempty MPSS segment.
The cloud diagnostic payload now opts into this survey. Cloud and hardware
results are recorded below after the exact resulting build is tested.
