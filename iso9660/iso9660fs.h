/*
 * Copyright (c) 2022 Jonas 'Sortie' Termansen.
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
 * iso9660fs.h
 * Implementation of the ISO 9660 filesystem.
 */

#ifndef ISO9660FS_H
#define ISO9660FS_H

extern uid_t request_uid;
extern gid_t request_gid;

class Inode;

mode_t HostModeFromFsMode(uint32_t fsmode);
uint32_t FsModeFromHostMode(mode_t hostmode);
uint8_t HostDTFromFsDT(uint8_t fsdt);
void StatInode(Inode* inode, struct stat* st);

#endif
