/*
 * Copyright (c) 2011, 2012, 2013, 2014, 2015, 2026 Jonas 'Sortie' Termansen.
 *
 * Permission to use, copy, modify, and distribute this software for any
 * purpose with or without fee is hereby granted, provided that the above
 * copyright notice and this permission notice appear in all copies.
 *
 * THE SOFTWARE IS PROVIDED "AS IS" AND THE AUTHOR DISCLAIMS ALL WARRANTIES
 * WITH REGARD TO THIS SOFTWARE INCLUDING ALL IMPLIED WARRANTIES OF
 * MERCHANTABILITY AND FITNESS. IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR
 * ANY SPECIAL, DIRECT, INDIRECT, OR CONSEQUENTIAL DAMAGES OR ANY DAMAGES
 * WHATSOEVER RESULTING FROM LOSS OF USE, DATA OR PROFITS, WHETHER IN AN
 * ACTION OF CONTRACT, NEGLIGENCE OR OTHER TORTIOUS ACTION, ARISING OUT OF
 * OR IN CONNECTION WITH THE USE OR PERFORMANCE OF THIS SOFTWARE.
 *
 * x64/syscall.S
 * An assembly stub that acts as glue for system calls.
 */

#include <sortix/syscall.h>

.section .text

.global syscall_handler
.type syscall_handler, @function
syscall_handler:
	cld
	movl $0, errno

	pushq %rbp
	movq %rsp, %rbp

	# Make sure the requested system call is valid, if not, then fix it.
	cmp $SYSCALL_MAX_NUM, %rax
	jae 3f

1:
	# Read a system call function pointer.
	movq syscall_ptr, %r11
	movq (%r11,%rax,8), %r10

	# Oh how nice, user-space put the parameters in: rdi, rsi, rdx, rcx, r8, r9

	# Call the system call.
	callq *%r10

	# Return to user-space, system call result in %rax:%rdx, errno in %ecx.
	popq %rbp
	movl errno, %ecx

	# Zero registers to avoid information leaks.
	# rax is return value.
	# rdi is set in a moment.
	xor %rsi, %rsi
	# rdx is return value (MIGHT NOT BE INITIALIZED, CAN LEAK!).
	# rcx is errno.
	xor %r8, %r8
	xor %r9, %r9
	xor %r10, %r10
	xor %r11, %r11
	# The rest of the registers are preserved by the ABI and syscall ABI.

	# If any signals are pending, fire them now.
	movq asm_signal_is_pending, %rdi
	testq %rdi, %rdi
	jnz 4f
	# rdi is zero in this branch.

2:
	iretq

3:
	# Call the null system call instead.
	xorq %rax, %rax
	jmp 1b

4:
	# We can't return to this location after the signal, since if any system
	# call is made this stack will get reused and all our nice temporaries wil
	# be garbage. We therefore pass the kernel the state to return to and it'll
	# handle it for us when the signal is over.
	movq 0(%rsp), %rdi # userspace rip
	movq 16(%rsp), %rsi # userspace rflags
	movq 24(%rsp), %r8 # userspace rsp, note %rcx is used for errno
	int $130 # Deliver pending signals.
	# If we end up here, it means that the signal didn't override anything and
	# that we should just go ahead and return to userspace ourselves. Zero the
	# scratch registers to avoid information leaks.
	xor %rdi, %rdi
	xor %rsi, %rsi
	xor %r8, %r8
	jmp 2b
.size syscall_handler, .-syscall_handler

.global strace
.type strace, @function
strace:
	pushq %rbp
	movq %rsp, %rbp
	# Make a copy of the parameters, and tell syscall_start what they are.
	push %rax # rax is a special non-calling-convention parameter here
	push %rdi
	push %rsi
	push %rdx
	push %rcx
	push %r8
	push %r9
	subq $8, %rsp
	push %rax # Parameter
	call syscall_start
	addq $16, %rsp
	pop %r9
	pop %r8
	pop %rcx
	pop %rdx
	pop %rsi
	pop %rdi
	pop %rax
	# Invoke the actual syscall with the real parameters.
	xorq %r11, %r11
	movq syscall_list(%r11,%rax,8), %r10
	callq *%r10
	# Inform syscall_end what the syscall result was.
	push %rax
	mov %rax, %rdi
	subq $8, %rsp
	call syscall_end
	addq $8, %rsp
	pop %rax
	pop %rbp
	ret
.size strace, .-strace
