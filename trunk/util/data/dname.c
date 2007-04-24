/*
 * util/data/dname.h - domain name handling
 *
 * Copyright (c) 2007, NLnet Labs. All rights reserved.
 *
 * This software is open source.
 * 
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 
 * Redistributions of source code must retain the above copyright notice,
 * this list of conditions and the following disclaimer.
 * 
 * Redistributions in binary form must reproduce the above copyright notice,
 * this list of conditions and the following disclaimer in the documentation
 * and/or other materials provided with the distribution.
 * 
 * Neither the name of the NLNET LABS nor the names of its contributors may
 * be used to endorse or promote products derived from this software without
 * specific prior written permission.
 * 
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
 * "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED
 * TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR
 * PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE REGENTS OR CONTRIBUTORS BE
 * LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
 * CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
 * SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
 * INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN
 * CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
 * ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
 * POSSIBILITY OF SUCH DAMAGE.
 */

/**
 * \file
 *
 * This file contains domain name handling functions.
 */

#include "config.h"
#include "util/data/dname.h"
#include "util/data/msgparse.h"
#include "util/log.h"
#include "util/storage/lookup3.h"

/** determine length of a dname in buffer, no compression pointers allowed. */
size_t
query_dname_len(ldns_buffer* query)
{
	size_t len = 0;
	size_t labellen;
	while(1) {
		if(ldns_buffer_remaining(query) < 1)
			return 0; /* parse error, need label len */
		labellen = ldns_buffer_read_u8(query);
		if(labellen&0xc0)
			return 0; /* no compression allowed in queries */
		len += labellen + 1;
		if(len > LDNS_MAX_DOMAINLEN)
			return 0; /* too long */
		if(labellen == 0)
			return len;
		if(ldns_buffer_remaining(query) < labellen)
			return 0; /* parse error, need content */
		ldns_buffer_skip(query, (ssize_t)labellen);
	}
}

int 
query_dname_compare(uint8_t* d1, uint8_t* d2)
{
	uint8_t lab1, lab2;
	log_assert(d1 && d2);
	lab1 = *d1++;
	lab2 = *d2++;
	while( lab1 != 0 || lab2 != 0 ) {
		/* compare label length */
		/* if one dname ends, it has labellength 0 */
		if(lab1 != lab2) {
			if(lab1 < lab2)
				return -1;
			return 1;
		}
		log_assert(lab1 == lab2 && lab1 != 0);
		/* compare lowercased labels. */
		while(lab1--) {
			if(tolower((int)*d1) != tolower((int)*d2)) {
				if(tolower((int)*d1) < tolower((int)*d2))
					return -1;
				return 1;
			}
			d1++;
			d2++;
		}
		/* next pair of labels. */
		lab1 = *d1++;
		lab2 = *d2++;
	}
	return 0;
}

void 
query_dname_tolower(uint8_t* dname, size_t len)
{
	/* the dname is stored uncompressed */
	uint8_t labellen;
	log_assert(len > 0);
	labellen = *dname;
	while(labellen) {
		dname++;
		while(labellen--) {
			*dname = (uint8_t)tolower((int)*dname);
			dname++;
		}
		labellen = *dname;
	}
}

/** maximum compression pointer position pointed to */
#define MAX_COMPRESS_POS 16384
/** size of bitmap for loop detection */
#define LOOP_BITMAP_SIZE (MAX_COMPRESS_POS/8)

/** check bit in bitmap for loop detection, then set it for next check */
static uint8_t 
loopcheck(uint8_t loop[], size_t pos)
{
	const uint8_t bits[8] = {0x1, 0x2, 0x4, 0x8, 0x10, 0x20, 0x40, 0x80};
	uint8_t ret;
	log_assert(pos < MAX_COMPRESS_POS);
	ret = loop[ pos / 8 ] & bits[ pos % 8 ];
	loop[ pos / 8 ] |= bits[ pos % 8 ];	
	return ret;
}


size_t
pkt_dname_len(ldns_buffer* pkt)
{
	size_t len = 0;
	uint8_t loop[LOOP_BITMAP_SIZE]; /* loopcheck array. */
	uint8_t labellen;
	size_t endpos = 0;

	/* read dname and determine length */
	/* check compression pointers, loops, out of bounds */
	memset(loop, 0, sizeof(loop));

	while(1) {
		/* read next label */
		if(ldns_buffer_remaining(pkt) < 1)
			return 0;
		labellen = ldns_buffer_read_u8(pkt);
		if(LABEL_IS_PTR(labellen)) {
			/* compression ptr */
			uint16_t ptr;
			if(ldns_buffer_remaining(pkt) < 1)
				return 0;
			ptr = PTR_OFFSET(labellen, ldns_buffer_read_u8(pkt));
			if(loopcheck(loop, ptr))
				return 0; /* loop! */
			if(ldns_buffer_limit(pkt) <= ptr)
				return 0; /* out of bounds! */
			if(!endpos)
				endpos = ldns_buffer_position(pkt);
			ldns_buffer_set_position(pkt, ptr);
		} else {
			/* label contents */
			if(labellen > 0x3f)
				return 0; /* label too long */
			len += 1 + labellen;
			if(len > LDNS_MAX_DOMAINLEN)
				return 0;
			if(labellen == 0) {
				/* end of dname */
				break;
			}
			if(ldns_buffer_remaining(pkt) < labellen)
				return 0;
			ldns_buffer_skip(pkt, (ssize_t)labellen);
		}
	}
	if(endpos)
		ldns_buffer_set_position(pkt, endpos);

	return len;
}

int 
dname_pkt_compare(ldns_buffer* pkt, uint8_t* d1, uint8_t* d2)
{
	uint8_t len1, len2;
	log_assert(pkt && d1 && d2);
	len1 = *d1++;
	len2 = *d2++;
	while( len1 != 0 || len2 != 0 ) {
		/* resolve ptrs */
		if(LABEL_IS_PTR(len1)) {
			d1 = ldns_buffer_at(pkt, PTR_OFFSET(len1, *d1));
			len1 = *d1++;
			continue;
		}
		if(LABEL_IS_PTR(len2)) {
			d2 = ldns_buffer_at(pkt, PTR_OFFSET(len2, *d2));
			len2 = *d2++;
			continue;
		}
		/* check label length */
		log_assert(len1 <= LDNS_MAX_LABELLEN);
		log_assert(len2 <= LDNS_MAX_LABELLEN);
		if(len1 != len2) {
			if(len1 < len2) return -1;
			return 1;
		}
		log_assert(len1 == len2 && len1 != 0);
		/* compare labels */
		while(len1--) {
			if(tolower((int)*d1++) != tolower((int)*d2++)) {
				if(tolower((int)d1[-1]) < tolower((int)d2[-1]))
					return -1;
				return 1;
			}
		}
		len1 = *d1++;
		len2 = *d2++;
	}
	return 0;
}

hashvalue_t 
dname_query_hash(uint8_t* dname, hashvalue_t h)
{
	uint8_t labuf[LDNS_MAX_LABELLEN+1];
	uint8_t lablen;
	int i;

	/* preserve case of query, make hash label by label */
	lablen = *dname;
	while(lablen) {
		log_assert(lablen <= LDNS_MAX_LABELLEN);
		labuf[0] = lablen;
		dname++;
		i=0;
		while(lablen--)
			labuf[++i] = (uint8_t)tolower((int)*dname++);
		h = hashlittle(labuf, labuf[0] + 1, h);
		lablen = *dname;
	}

	return h;
}

hashvalue_t 
dname_pkt_hash(ldns_buffer* pkt, uint8_t* dname, hashvalue_t h)
{
	uint8_t labuf[LDNS_MAX_LABELLEN+1];
	uint8_t lablen;
	int i;

	/* preserve case of query, make hash label by label */
	lablen = *dname++;
	while(lablen) {
		if(LABEL_IS_PTR(lablen)) {
			/* follow pointer */
			dname = ldns_buffer_at(pkt, PTR_OFFSET(lablen, *dname));
			lablen = *dname++;
			continue;
		}
		log_assert(lablen <= LDNS_MAX_LABELLEN);
		labuf[0] = lablen;
		i=0;
		while(lablen--)
			labuf[++i] = (uint8_t)tolower((int)*dname++);
		h = hashlittle(labuf, labuf[0] + 1, h);
		lablen = *dname++;
	}

	return h;
}

void dname_pkt_copy(ldns_buffer* pkt, uint8_t* to, uint8_t* dname)
{
	/* copy over the dname and decompress it at the same time */
	uint8_t lablen;
	lablen = *dname++;
	while(lablen) {
		if(LABEL_IS_PTR(lablen)) {
			/* follow pointer */
			dname = ldns_buffer_at(pkt, PTR_OFFSET(lablen, *dname));
			lablen = *dname++;
			continue;
		}
		log_assert(lablen <= LDNS_MAX_LABELLEN);
		*to++ = lablen;
		memmove(to, dname, lablen);
		dname += lablen;
		to += lablen;
		lablen = *dname++;
	}
	/* copy last \0 */
	*to = 0;
}

void dname_print(FILE* out, ldns_buffer* pkt, uint8_t* dname)
{
	uint8_t lablen;
	if(!out) out = stdout;
	if(!dname) return;

	lablen = *dname++;
	if(!lablen) 
		fputc('.', out);
	while(lablen) {
		if(LABEL_IS_PTR(lablen)) {
			/* follow pointer */
			if(!pkt) {
				fputs("??compressionptr??", out);
				return;
			}
			dname = ldns_buffer_at(pkt, PTR_OFFSET(lablen, *dname));
			lablen = *dname++;
			continue;
		}
		if(lablen > LDNS_MAX_LABELLEN) {
			fputs("??extendedlabel??", out);
			return;
		}
		while(lablen--)
			fputc((int)*dname++, out);
		fputc('.', out);
		lablen = *dname++;
	}
}
