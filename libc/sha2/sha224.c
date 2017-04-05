/*	$OpenBSD: sha2.c,v 1.25 2016/09/03 16:25:03 tedu Exp $	*/

/*
 * FILE:	sha2.c
 * AUTHOR:	Aaron D. Gifford <me@aarongifford.com>
 *
 * Copyright (c) 2000-2001, Aaron D. Gifford
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 * 3. Neither the name of the copyright holder nor the names of contributors
 *    may be used to endorse or promote products derived from this software
 *    without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE AUTHOR AND CONTRIBUTOR(S) ``AS IS'' AND
 * ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED.  IN NO EVENT SHALL THE AUTHOR OR CONTRIBUTOR(S) BE LIABLE
 * FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
 * DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS
 * OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION)
 * HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT
 * LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY
 * OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF
 * SUCH DAMAGE.
 *
 * $From: sha2.c,v 1.1 2001/11/08 00:01:51 adg Exp adg $
 */

#include <sys/types.h>

#include <endian.h>
#include <stdint.h>
#include <string.h>

#include "sha2.h"

#define BE_32_TO_8(cp, src) do {					\
	(cp)[0] = (src) >> 24;						\
	(cp)[1] = (src) >> 16;						\
	(cp)[2] = (src) >> 8;						\
	(cp)[3] = (src);						\
} while (0)

/*** SHA-XYZ INITIAL HASH VALUES AND CONSTANTS ************************/
/* Initial hash value H for SHA-224: */
static const uint32_t sha224_initial_hash_value[8] = {
	0xc1059ed8UL,
	0x367cd507UL,
	0x3070dd17UL,
	0xf70e5939UL,
	0xffc00b31UL,
	0x68581511UL,
	0x64f98fa7UL,
	0xbefa4fa4UL
};

/*** SHA-224: *********************************************************/
void
SHA224Init(SHA2_CTX *context)
{
	memcpy(context->state.st32, sha224_initial_hash_value,
	    sizeof(sha224_initial_hash_value));
	memset(context->buffer, 0, sizeof(context->buffer));
	context->bitcount[0] = 0;
}

void
SHA224Transform(uint32_t state[8], const uint8_t data[SHA224_BLOCK_LENGTH])
{
	SHA256Transform(state, data);
}

void
SHA224Update(SHA2_CTX *context, const uint8_t *data, size_t len)
{
	SHA256Update(context, data, len);
}

void
SHA224Pad(SHA2_CTX *context)
{
	SHA256Pad(context);
}

void
SHA224Final(uint8_t digest[SHA224_DIGEST_LENGTH], SHA2_CTX *context)
{
	SHA224Pad(context);

#if BYTE_ORDER == LITTLE_ENDIAN
	int	i;

	/* Convert TO host byte order */
	for (i = 0; i < 7; i++)
		BE_32_TO_8(digest + i * 4, context->state.st32[i]);
#else
	memcpy(digest, context->state.st32, SHA224_DIGEST_LENGTH);
#endif
	explicit_bzero(context, sizeof(*context));
}
