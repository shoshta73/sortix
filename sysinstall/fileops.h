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
 * fileops.h
 * File operation utility functions.
 */

#ifndef FILEOPS_H
#define FILEOPS_H

char* join_paths(const char* a, const char* b);
int mkdir_p(const char* path, mode_t mode);
int access_or_die(const char* path, int mode);
int access_join_or_die(const char* a, const char* b, int mode);
void mkdir_or_chmod_or_die(const char* path, mode_t mode);
void write_random_seed(const char* path);
char* read_string_file(const char* path);
char** read_lines_file(const char* path, size_t* out_count);
char* akernelinfo(const char* request);

#endif
