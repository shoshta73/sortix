/*
 * Copyright (c) 2015, 2016, 2017 Jonas 'Sortie' Termansen.
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
 * conf.h
 * Utility functions to handle upgrade.conf(5).
 */

#ifndef CONF_H
#define CONF_H

struct conf
{
	char* channel;
	bool force_mirror;
	bool grub;
	char* mirror;
	bool newsrc;
	bool ports;
	char* release_key;
	char* release_sig_url;
	bool src;
	bool system;
};

void conf_init(struct conf* conf);
void conf_free(struct conf* conf);
bool conf_load(struct conf* conf, const char* path);

#endif
