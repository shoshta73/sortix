/*
 * Copyright (c) 2013, 2014 Jonas 'Sortie' Termansen.
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
 * sortix/ucontext.h
 * Declares ucontext_t and associated values.
 */

#ifndef _INCLUDE_SORTIX_UCONTEXT_H
#define _INCLUDE_SORTIX_UCONTEXT_H

#include <sys/cdefs.h>

#include <sortix/sigset.h>
#include <sortix/stack.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Register declarations for i386 */
#if defined(__i386__)
typedef int __greg_t;
#define __NGREG 23
typedef __greg_t __gregset_t[__NGREG];
#if __USE_SORTIX
enum
{
	REG_GS = 0,
#define REG_GS REG_GS
	REG_FS,
#define REG_FS REG_FS
	REG_ES,
#define REG_ES REG_ES
	REG_DS,
#define REG_DS REG_DS
	REG_EDI,
#define REG_EDI REG_EDI
	REG_ESI,
#define REG_ESI REG_ESI
	REG_EBP,
#define REG_EBP REG_EBP
	REG_ESP,
#define REG_ESP REG_ESP
	REG_EBX,
#define REG_EBX REG_EBX
	REG_EDX,
#define REG_EDX REG_EDX
	REG_ECX,
#define REG_ECX REG_ECX
	REG_EAX,
#define REG_EAX REG_EAX
	REG_EIP,
#define REG_EIP REG_EIP
	REG_CS,
#define REG_CS REG_CS
	REG_EFL,
#define REG_EFL REG_EFL
	REG_SS,
#define REG_SS REG_SS
	REG_CR2,
#define REG_CR2 REG_CR2
	REG_FSBASE,
#define REG_FSBASE REG_FSBASE
	REG_GSBASE
#define REG_GSBASE REG_GSBASE
};
#endif
#endif

/* Register declarations for x86-64 */
#if defined(__x86_64__)
typedef long int __greg_t;
#define __NGREG 23
typedef __greg_t __gregset_t[__NGREG];
#if __USE_SORTIX
enum
{
	REG_R8 = 0,
#define REG_R8 REG_R8
	REG_R9,
#define REG_R9 REG_R9
	REG_R10,
#define REG_R10 REG_R10
	REG_R11,
#define REG_R11 REG_R11
	REG_R12,
#define REG_R12 REG_R12
	REG_R13,
#define REG_R13 REG_R13
	REG_R14,
#define REG_R14 REG_R14
	REG_R15,
#define REG_R15 REG_R15
	REG_RDI,
#define REG_RDI REG_RDI
	REG_RSI,
#define REG_RSI REG_RSI
	REG_RBP,
#define REG_RBP REG_RBP
	REG_RBX,
#define REG_RBX REG_RBX
	REG_RDX,
#define REG_RDX REG_RDX
	REG_RAX,
#define REG_RAX REG_RAX
	REG_RCX,
#define REG_RCX REG_RCX
	REG_RSP,
#define REG_RSP REG_RSP
	REG_RIP,
#define REG_RIP REG_RIP
	REG_EFL,
#define REG_EFL REG_EFL
	REG_CSGSFS, /* Actually short cs, gs, fs, __pad0.  */
#define REG_CSGSFS REG_CSGSFS
	REG_CR2,
#define REG_CR2 REG_CR2
	REG_FSBASE,
#define REG_FSBASE REG_FSBASE
	REG_GSBASE
#define REG_GSBASE REG_GSBASE
};
#endif
#endif

#if __USE_SORTIX
typedef __greg_t greg_t;
#define NGREG __NGREG
typedef __gregset_t gregset_t;
#endif

typedef struct
{
#if __USE_SORTIX
	gregset_t gregs;
#if defined(__i386__) || defined(__x86_64__)
	unsigned char fpuenv[512];
#endif
#else
	__gregset_t __gregs;
#if defined(__i386__) || defined(__x86_64__)
	unsigned char __fpuenv[512];
#endif
#endif
} mcontext_t;

typedef struct __ucontext ucontext_t;
typedef struct __ucontext
{
	ucontext_t* uc_link;
	sigset_t uc_sigmask;
	stack_t uc_stack;
	mcontext_t uc_mcontext;
} ucontext_t;

#ifdef __cplusplus
} /* extern "C" */
#endif

#endif
