/* SPDX-License-Identifier: BSD-2-Clause-Patent
 * Linux AArch64 PID 1 for the Picasso fallback boot image.
 * No filesystem or block-device writes: wait for driver probing, then request
 * the vendor kernel's RESTART2("bootloader") PMIC/IMEM reset sequence.
 */
.global _start
.section .text
.balign 4
_start:
    adr x0, delay
    mov x1, xzr
    mov x8, #101                 /* __NR_nanosleep */
    svc #0

    movz x0, #0xdead
    movk x0, #0xfee1, lsl #16    /* LINUX_REBOOT_MAGIC1 */
    movz x1, #0x1969
    movk x1, #0x2812, lsl #16    /* LINUX_REBOOT_MAGIC2 */
    movz x2, #0xc3d4
    movk x2, #0xa1b2, lsl #16    /* LINUX_REBOOT_CMD_RESTART2 */
    adr x3, target
    mov x8, #142                 /* __NR_reboot */
    svc #0

    /* A failed reboot must not exit PID 1 or create a reboot loop. */
    mov x0, #2
    adr x1, failed
    mov x2, #(failed_end - failed)
    mov x8, #64                  /* __NR_write, console stderr only */
    svc #0
1:
    adr x0, delay
    mov x1, xzr
    mov x8, #101
    svc #0
    b 1b

.section .rodata
.balign 8
delay: .quad 3, 0
target: .asciz "bootloader"
failed: .ascii "Picasso Fastboot relay: reboot failed; use the hardware keys.\n"
failed_end:
.section .note.GNU-stack,"",%progbits
