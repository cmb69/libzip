/*
  zip_source_pkware.c -- Traditional PKWARE de/encryption routines
  Copyright (C) 2009 Dieter Baron and Thomas Klausner

  This file is part of libzip, a library to manipulate ZIP archives.
  The authors can be contacted at <libzip@nih.at>

  Redistribution and use in source and binary forms, with or without
  modification, are permitted provided that the following conditions
  are met:
  1. Redistributions of source code must retain the above copyright
     notice, this list of conditions and the following disclaimer.
  2. Redistributions in binary form must reproduce the above copyright
     notice, this list of conditions and the following disclaimer in
     the documentation and/or other materials provided with the
     distribution.
  3. The names of the authors may not be used to endorse or promote
     products derived from this software without specific prior
     written permission.
 
  THIS SOFTWARE IS PROVIDED BY THE AUTHORS ``AS IS'' AND ANY EXPRESS
  OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED
  WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
  ARE DISCLAIMED.  IN NO EVENT SHALL THE AUTHORS BE LIABLE FOR ANY
  DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
  DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE
  GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
  INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER
  IN CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR
  OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN
  IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
*/



#include <stdlib.h>
#include <string.h>

#include "zipint.h"

struct trad_pkware {
    struct zip_source *src;
    int e[2];

    zip_uint32_t key[3];
};

#define HEADERLEN	12
#define KEY0		305419896
#define KEY1		591751049
#define KEY2		878082192

static const uLongf *crc = NULL;

#define CRC32(c, b) (crc[((c) ^ (b)) & 0xff] ^ ((c) >> 8))



static void decrypt(struct trad_pkware *, zip_uint8_t *,
		    const zip_uint8_t *, zip_uint64_t, int);
static int decrypt_header(struct trad_pkware *);
static zip_int64_t pkware_decrypt(void *, void *, zip_uint64_t, enum zip_source_cmd);



ZIP_EXTERN struct zip_source *
zip_source_pkware(struct zip *za, struct zip_source *src,
		  zip_uint16_t em, int flags, const char *password)
{
    struct trad_pkware *ctx;

    if (za == NULL || password == NULL || src == NULL
	|| em != ZIP_EM_TRAD_PKWARE) {
	_zip_error_set(&za->error, ZIP_ER_INVAL, 0);
	return NULL;
    }
    if (flags & ZIP_CODEC_ENCODE) {
	_zip_error_set(&za->error, ZIP_ER_ENCRNOTSUPP, 0);
	return NULL;
    }

    if (crc == NULL)
	crc = get_crc_table();

    if ((ctx=(struct trad_pkware *)malloc(sizeof(*ctx))) == NULL) {
	_zip_error_set(&za->error, ZIP_ER_MEMORY, 0);
	return NULL;
    }

    ctx->src = src;
    ctx->e[0] = ctx->e[1] = 0;

    ctx->key[0] = KEY0;
    ctx->key[1] = KEY1;
    ctx->key[2] = KEY2;
    decrypt(ctx, NULL, (const zip_uint8_t *)password, strlen(password), 1);

    return zip_source_function(za, pkware_decrypt, ctx);
}



static void
decrypt(struct trad_pkware *ctx, zip_uint8_t *out, const zip_uint8_t *in,
	zip_uint64_t len, int update_only)
{
    zip_uint16_t tmp;
    zip_uint64_t i;
    Bytef b;

    for (i=0; i<len; i++) {
	b = in[i];

	if (!update_only) {
	    /* decrypt next byte */
	    tmp = ctx->key[2] | 2;
	    tmp = (tmp * (tmp ^ 1)) >> 8;
	    b ^= tmp;
	}

	/* store cleartext */
	if (out)
	    out[i] = b;

	/* update keys */
	ctx->key[0] = CRC32(ctx->key[0], b);
	ctx->key[1] = (ctx->key[1] + (ctx->key[0] & 0xff)) * 134775813 + 1;
	b = ctx->key[1] >> 24;
	ctx->key[2] = CRC32(ctx->key[2], b);
    }
}



static int
decrypt_header(struct trad_pkware *ctx)
{
    zip_uint8_t header[HEADERLEN];
    struct zip_stat st;
    zip_int64_t n;
    unsigned short dostime, dosdate;

    if ((n=zip_source_call(ctx->src, header, HEADERLEN, ZIP_SOURCE_READ)) < 0) {
	zip_source_call(ctx->src, ctx->e, sizeof(ctx->e), ZIP_SOURCE_ERROR);
	return -1;
    }
    
    if (n != HEADERLEN) {
	ctx->e[0] = ZIP_ER_EOF;
	ctx->e[1] = 0;
	return -1;
    }

    decrypt(ctx, header, header, HEADERLEN, 0);

    if (zip_source_call(ctx->src, &st, sizeof(st), ZIP_SOURCE_STAT) < 0) {
	/* stat failed, skip password validation */

	return 0;
    }

    _zip_u2d_time(st.mtime, &dostime, &dosdate);

    if (header[HEADERLEN-1] != st.crc>>24
	&& header[HEADERLEN-1] != dostime>>8) {
	ctx->e[0] = ZIP_ER_WRONGPASSWD;
	ctx->e[1] = 0;
	return -1;
    }

    return 0;
}



static zip_int64_t
pkware_decrypt(void *ud, void *data, zip_uint64_t len, enum zip_source_cmd cmd)
{
    struct trad_pkware *ctx;
    zip_int64_t n;

    ctx = (struct trad_pkware *)ud;

    switch (cmd) {
    case ZIP_SOURCE_OPEN:
	if (zip_source_call(ctx->src, data, len, cmd) < 0) {
	    zip_source_call(ctx->src, ctx->e, sizeof(ctx->e), ZIP_SOURCE_ERROR);
	    return -1;
	}
	if (decrypt_header(ctx) < 0)
	    return -1;
	return 0;

    case ZIP_SOURCE_READ:
	if ((n=zip_source_call(ctx->src, data, len, cmd)) < 0) {
	    zip_source_call(ctx->src, ctx->e, sizeof(ctx->e), ZIP_SOURCE_ERROR);
	    return -1;
	}
	decrypt(ud, (zip_uint8_t *)data, (zip_uint8_t *)data, (zip_uint64_t)n,
		0);
	return n;

    case ZIP_SOURCE_CLOSE:
	if (zip_source_call(ctx->src, data, len, cmd) < 0) {
	    zip_source_call(ctx->src, ctx->e, sizeof(ctx->e), ZIP_SOURCE_ERROR);
	    return -1;
	}
	return 0;

    case ZIP_SOURCE_STAT:
	if (zip_source_call(ctx->src, data, len, cmd) < 0) {
	    zip_source_call(ctx->src, ctx->e, sizeof(ctx->e), ZIP_SOURCE_ERROR);
	    return -1;
	}
	else {
	    struct zip_stat *st;

	    st = (struct zip_stat *)data;

	    st->encryption_method = ZIP_EM_NONE;
	    st->valid |= ZIP_STAT_ENCRYPTION_METHOD;
	    if (st->valid & ZIP_STAT_COMP_SIZE)
		st->comp_size -= HEADERLEN;
	}
	return 0;

    case ZIP_SOURCE_ERROR:
	if (len < sizeof(int)*2)
	    return -1;

	memcpy(data, ctx->e, sizeof(int)*2);
	return sizeof(int)*2;

    case ZIP_SOURCE_FREE:
	zip_source_call(ctx->src, data, len, cmd);
	free(ud);
	return 0;

    default:
	ctx->e[0] = ZIP_ER_INVAL;
	ctx->e[1] = 0;
	return -1;
    }
}
