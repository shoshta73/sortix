/*
 * Copyright (c) 2015, 2016, 2017, 2024, 2025 Jonas 'Sortie' Termansen.
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
 * release.h
 * Utility functions to handle release information.
 */

#ifndef RELEASE_H
#define RELEASE_H

struct release
{
	char* architecture;
	char* pretty_name;
	unsigned long version_major;
	unsigned long version_minor;
	bool version_dev;
	unsigned long abi_major;
	unsigned long abi_minor;
};

int abi_compare(unsigned long a_major, unsigned long a_minor,
                unsigned long b_major, unsigned long b_minor);
bool abi_compatible(unsigned long a_major, unsigned long a_minor,
                    unsigned long b_major, unsigned long b_minor);
int version_compare(unsigned long a_major, unsigned long a_minor, bool a_dev,
                    unsigned long b_major, unsigned long b_minor, bool b_dev);
void release_free(struct release* release);
int release_compare_abi(const struct release* a, const struct release* b);
int release_compare_version(const struct release* a, const struct release* b);
bool os_release_load(struct release* release,
                     const char* path,
                     const char* errpath);
char* read_platform(const char* prefix);

#endif
