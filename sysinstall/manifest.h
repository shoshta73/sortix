/*
 * Copyright (c) 2015, 2016, 2020 Jonas 'Sortie' Termansen.
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
 * manifest.h
 * Manifest handling functions.
 */

#ifndef MANIFEST_H
#define MANIFEST_H

bool has_manifest(const char* manifest);
char** read_manifest(const char* path, size_t* out_count);
void install_manifest(const char* manifest,
                      const char* from_prefix,
                      const char* to_prefix,
                      const char* const* preserved,
                      size_t preserved_count,
                      bool may_hardlink);
void install_manifests(const char* const* manifests,
                       size_t manifests_count,
                       const char* from_prefix,
                       const char* to_prefix,
                       bool may_hardlink);
char** read_installed_list(const char* prefix, size_t* out_count);
void install_manifests_detect(const char* from_prefix,
                              const char* to_prefix,
                              bool system,
                              bool detect_from,
                              bool detect_to,
                              bool may_hardlink);

#endif
