/*
 * Copyright (c) 2016, 2017 Jonas 'Sortie' Termansen.
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
 * sortix/ioctl.h
 * Miscellaneous device control interface.
 */

#ifndef INCLUDE_SORTIX_IOCTL_H
#define INCLUDE_SORTIX_IOCTL_H

#define __IOCTL_TYPE_EXP 3 /* 2^3 kinds of argument types supported.*/
#define __IOCTL_TYPE_MASK ((1 << __IOCTL_TYPE_EXP) - 1)
#define __IOCTL_TYPE_VOID 0
#define __IOCTL_TYPE_INT 1
#define __IOCTL_TYPE_LONG 2
#define __IOCTL_TYPE_PTR 3
/* 4-7 is unused in case of future expansion. */
#define __IOCTL(index, type) ((index) << __IOCTL_TYPE_EXP | (type))
#define __IOCTL_INDEX(value) ((value) >> __IOCTL_TYPE_EXP)
#define __IOCTL_TYPE(value) ((value) & __IOCTL_TYPE_MASK)

#define TIOCGWINSZ __IOCTL(1, __IOCTL_TYPE_PTR)
#define TIOCSWINSZ __IOCTL(2, __IOCTL_TYPE_PTR)
#define TIOCSCTTY __IOCTL(3, __IOCTL_TYPE_INT)
#define TIOCSPTLCK __IOCTL(4, __IOCTL_TYPE_PTR)
#define TIOCGPTLCK __IOCTL(5, __IOCTL_TYPE_PTR)
#define TIOCGNAME __IOCTL(6, __IOCTL_TYPE_PTR)
#define TIOCGPTN __IOCTL(7, __IOCTL_TYPE_PTR)
#define TIOCGDISPLAYS __IOCTL(8, __IOCTL_TYPE_PTR)

#define IOC_TYPE(x) ((x) >> 0 & 0xFF)
#define IOC_TYPE_BLOCK_DEVICE 1
#define IOC_TYPE_NETWORK_INTERFACE 2
#define IOC_SUBTYPE(x) ((x) >> 8 & 0xFF)
#define IOC_SUBTYPE_BLOCK_DEVICE_HARDDISK 1
#define IOC_SUBTYPE_BLOCK_DEVICE_PARTITION 2
#define IOC_MAKE_TYPE(type, subtype) ((type) << 0 | (subtype) << 8)
#define IOCGETTYPE __IOCTL(9, __IOCTL_TYPE_VOID)

#define NIOC_GETINFO __IOCTL(10, __IOCTL_TYPE_PTR)
#define NIOC_GETSTATUS __IOCTL(11, __IOCTL_TYPE_PTR)
#define NIOC_GETCONFIG __IOCTL(12, __IOCTL_TYPE_PTR)
#define NIOC_SETCONFIG __IOCTL(13, __IOCTL_TYPE_PTR)
#define NIOC_GETCONFIG_ETHER __IOCTL(14, __IOCTL_TYPE_PTR)
#define NIOC_SETCONFIG_ETHER __IOCTL(15, __IOCTL_TYPE_PTR)
#define NIOC_GETCONFIG_INET __IOCTL(16, __IOCTL_TYPE_PTR)
#define NIOC_SETCONFIG_INET __IOCTL(17, __IOCTL_TYPE_PTR)
#define NIOC_WAITLINKSTATUS __IOCTL(18, __IOCTL_TYPE_INT)

#endif
