// Copyright 2020 The Fuchsia Authors
//
// Use of this source code is governed by a MIT-style
// license that can be found in the LICENSE file or at
// https://opensource.org/licenses/MIT

#include <asm.h>
#include <arch/riscv64.h>

#define REGOFF(x) ((x) * 8)

// top level exception handler for riscv64 in non vectored mode
.balign 4
FUNCTION(riscv64_exception_entry)
    // swap the scratch system register with the current stack pointer
    csrrw  sp, sscratch, sp
    // if we took the exception from kernelspace, sscratch was set to 0. if we
    // took if from userspace, sscratch was set to the thread's kernel stack
    bnez   sp, taken_from_userspace

    // the following two blocks:
    // 1- ensure they have a kernel stack based on where the exception was taken
    // 2- create an iframe on that kernel stack
    // 3- save the previous sscratch on that iframe
    // 4- save the previous sp on that iframe
    // 5- save t0 on that iframe (necessary to retrieve sp in the user case)
taken_from_kernelspace:
    // 1- we were already running in kernelspace, we just keep the previous sp
    csrrw  sp, sscratch, sp
    // 4- save the existing sp in an iframe on the stack
    sd     sp, -REGOFF(19)(sp)
    // 2- reserve an iframe_t on the stack
    addi   sp, sp, -REGOFF(20)
    // 3- sscratch was 0 so we store x0 in the iframe's field for sscratch
    sd     x0, REGOFF(0)(sp)
    // 5- save the t0 register
    sd     t0, REGOFF(13)(sp)

    j      save_rest_of_iframe
taken_from_userspace:
    // 1- we just got the current thread's kernel stack out of sscratch
    // 3- sscratch was our current stack, we store it in an iframe on the stack
    sd     sp, -REGOFF(20)(sp)
    // 2- reserve an iframe_t on the stack
    addi   sp, sp, -REGOFF(20)
    // 5- save the t0 register before we need to overwrite it with sscratch
    sd     t0, REGOFF(13)(sp)
    // 4a- retrieve the previous sp in t0. use this opportunity to also set
    // sscratch to 0, in on our convention this means that we are in kernelspace
    csrrw  t0, sscratch, 0
    // 4b- save the existing sp in an iframe on the stack
    sd     t0, REGOFF(1)(sp)

    // this is shared by both cases, we store all the callee trashed regs
save_rest_of_iframe:
    // sscratch is already saved
    // sp is already saved
    csrr   t0, sepc
    sd     t0, REGOFF(2)(sp)
    csrr   t0, sstatus
    sd     t0, REGOFF(3)(sp)
    sd     ra, REGOFF(4)(sp)
    sd     a0, REGOFF(5)(sp)
    sd     a1, REGOFF(6)(sp)
    sd     a2, REGOFF(7)(sp)
    sd     a3, REGOFF(8)(sp)
    sd     a4, REGOFF(9)(sp)
    sd     a5, REGOFF(10)(sp)
    sd     a6, REGOFF(11)(sp)
    sd     a7, REGOFF(12)(sp)
    // t0 is already saved
    sd     t1, REGOFF(14)(sp)
    sd     t2, REGOFF(15)(sp)
    sd     t3, REGOFF(16)(sp)
    sd     t4, REGOFF(17)(sp)
    sd     t5, REGOFF(18)(sp)
    sd     t6, REGOFF(19)(sp)

call_c_handler:
    csrr   a0, scause
    mv     a1, sp
    jal    riscv64_exception_handler

restore_iframe:
    ld     t0, REGOFF(0)(sp)
    csrw   sscratch, t0
    ld     t0, REGOFF(2)(sp)
    csrw   sepc, t0
    ld     t0, REGOFF(3)(sp)
    csrw   sstatus, t0
    ld     ra, REGOFF(4)(sp)
    ld     a0, REGOFF(5)(sp)
    ld     a1, REGOFF(6)(sp)
    ld     a2, REGOFF(7)(sp)
    ld     a3, REGOFF(8)(sp)
    ld     a4, REGOFF(9)(sp)
    ld     a5, REGOFF(10)(sp)
    ld     a6, REGOFF(11)(sp)
    ld     a7, REGOFF(12)(sp)
    ld     t0, REGOFF(13)(sp)
    ld     t1, REGOFF(14)(sp)
    ld     t2, REGOFF(15)(sp)
    ld     t3, REGOFF(16)(sp)
    ld     t4, REGOFF(17)(sp)
    ld     t5, REGOFF(18)(sp)
    ld     t6, REGOFF(19)(sp)
    // finish by restoring the stack (because it overwrites our iframe pointer)
    ld     sp, REGOFF(1)(sp)

    sret
END_FUNCTION(riscv64_exception_entry)
