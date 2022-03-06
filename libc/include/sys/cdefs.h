/*
 * Copyright (c) 2011, 2012, 2013, 2015 Jonas 'Sortie' Termansen.
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
 * sys/cdefs.h
 * Declares internal macros for the C programming language.
 */

#ifndef _INCLUDE_SYS_CDEFS_H
#define _INCLUDE_SYS_CDEFS_H

#include <features.h>

/* Preprocessor trick to turn anything into a string. */
#define __STRINGIFY(x) #x

/* Issue warning when this is used, except in defines, where the warning is
   inserted whenever the macro is expanded. This can be used to deprecated
   macros - and it happens on preprocessor level - so it shouldn't change any
   semantics of any code that uses such a macro. The argument msg should be a
   string that contains the warning. */
#define __PRAGMA_WARNING(msg) _Pragma(__STRINGIFY(GCC warning msg))

/* Use the real restrict keyword if it is available. Not that this really
   matters as gcc uses __restrict and __restrict__ as aliases for restrict, but
   it will look nicer after preprocessing. */
#if __HAS_RESTRICT
#undef __restrict
#define __restrict restrict
#endif

/* Provide the restrict keyword if requested and unavailable. */
#if !__HAS_RESTRICT && __want_restrict
#define restrict __restrict
#undef __HAS_RESTRICT
#define __HAS_RESTRICT 2
#endif

/* Macro to declare a weak alias. */
#if defined(__is_sortix_libc)
#define weak_alias(old, new) \
	extern __typeof(old) new __attribute__((weak, alias(#old)))
#endif

#define __pure2 __attribute__((__const__))

/*
 * Compatibility macros to advertise to ports when features become available.
 * These macros are only for things that are not trivial to detect from a C
 * source file (such as by a standard macro).
 *
 * The feature foo should initially have an undef preprocessor statement of the
 * macro __SORTIX_HAS_FOO__ (or something like that). It should carry a TODO
 * comment stating:
 *
 * - What the feature is and when the macro should become defined.
 * - Which ports (if any) rely on this macro and will use the feature whenever
 *   the macro is defined.
 * - That the macro should be removed when the specific ports have been updated
 *   to no longer rely on the macro. If no ports used the feature in the first
 *   place, it should say to remove the macro when the feature is implemented.
 *
 * The undef statement should become a define statement when the feature is
 * implemented. As the each port is updated after that, the feature should
 * become unconditionally used in the port, and the port is removed from the
 * list here. Finally, when the list is empty, the macro is removed entirely.
 *
 * For instance, the TODO statement for the feature foo with the undef'd macro
 * __SORTIX_HAS_FOO__ might say:
 *
 *   "Define when foo is implemented. Remove when libbar, libqux, and libbaz are
 *    updated to not rely on this macro."
 *
 * Should a port become broken if a macro is defined here, the port should be
 * fixed (by either fixing it or disabling the feature in the port regardless
 * of the macro) before the macro is officially enabled here.
 */

/* TODO: Define when floating point formating is implemented. Remove when gawk
         is updated to not rely on this macro. */
#undef __SORTIX_HAS_FORMAT_FLOAT__

/* TODO: Define when floating point parsing is implemented. No ports are using
         this macro at this time. Remove when the feature is implemented. */
#undef __SORTIX_HAS_FLOAT_PARSE__

/* TODO: Define when restartable system calls are implemented. Remove when git
         is updated to not rely on this macro. */
#undef __SORTIX_HAS_RESTARTABLE_SYSCALLS__

/* TODO: Define when general user security is implemented. Remove when ssh is
         updated to not rely on this macro. */
#undef __SORTIX_HAS_UID_SECURITY__

/* TODO: Define when mmap MAP_SHARED works properly. Remove when ssh is updated
         to not rely on this macro. */
#undef __SORTIX_HAS_WORKING_MAP_SHARED__

/* TODO: Define when shared memory, file descriptor passing, and general user
         security are implemented. Remove when ssh is updated to not rely on
         this macro. */
#undef __SORTIX_HAS_WORKING_PRIVSEP__

/* TODO: Define when the main stack will automatically grow. Remove when httpd
         is updated to not rely on this macro. */
#undef __SORTIX_HAS_GROWING_STACK__

/* TODO: Define when initgroups(2) is implemented. Remove when libdbus and ssh
         are updated not rely on this macro. */
#undef __SORTIX_HAS_INITGROUPS__

/* TODO: Define when setgroups(2) is implemented. Remove when libdbus and ssh
         are updated not rely on this macro. */
#undef __SORTIX_HAS_SETGROUPS__

/* TODO: Define when getgroups(2) is implemented. Remove when ssh is updated to
         not rely on this macro. */
#undef __SORTIX_HAS_GETGROUPS__

/* TODO: Define when scanf is standards compliant. Remove when ssh is updated to
         not rely on this macro. */
#undef __SORTIX_HAS_WORKING_SCANF__

/* TODO: Define when getservbyname(3) is implemented and works. Remove when ssh
         is updated to not rely on this macro. */
#undef __SORTIX_HAS_GETSERVBYNAME__

#endif
