// user.S
// To compile on macOS (Apple Silicon):
// as -arch arm64 -o user.o user.S
// objcopy -O binary user.o user.bin

.text
.align 4
.globl _start
_start:
    nop
    nop
    mov x0, #10
    svc #2

    ldr x0, =0x80200001  // unaligned address

    mov x10, #42
    str x10, [x0]  // unaligned STR

    ldr x2, [x0]  // unaligned LDR

    ldp x2, x3, [x0]  // unaligned LDP

    mov x11, #43
    stp x11, x3, [x0]  // unaligned STP

    fmov d0, #1.0  // SIMD
    fmov d1, #2.0
    fadd d0, d0, d1

    mov x0, #42
    svc #1  // SVC, shim adds 1 to x0, so x0=43 after

    // HVC from EL0 is UNDEFINED!
    // hvc #10  // HVC to host, with x0=43

    b .