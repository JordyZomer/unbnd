/*
 * testcode/unitvandergaast.c - unit test for vandergaast routines.
 *
 * Copyright (c) 2013, NLnet Labs. All rights reserved.
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
 *
 */
 
/**
 * \file
 * Calls vandergaast related unit tests. Exits with code 1 on a failure. 
 */

#include "config.h"

#ifdef CLIENT_SUBNET

#include "util/log.h"
#include "util/module.h"
#include "testcode/unitmain.h"
#include "edns-subnet/addrtree.h"

/*
	void printkey(addrkey_t *k, addrlen_t bits)
	{
		int byte;
		int bytes = bits/8 + ((bits%8)>0);
		for (byte = 0; byte < bytes; byte++) {
			printf("%02x ", k[byte]);
		}
	}

	void print_tree(struct addrnode* node, int indent)
	{
		struct addredge* edge;
		int i, s, byte;
		if (indent == 0) printf("-----Tree-----");
		printf("[node elem:%d]\n", node->elem != NULL);
		for (i = 0; i<2; i++) {
			if (node->edge[i]) {
				for (s = 0; s < indent; s++) printf(" ");
				printkey(node->edge[i]->str, node->edge[i]->len);
				printf("(len %d bits, %d bytes) ", node->edge[i]->len, 
					node->edge[i]->len/8 + ((node->edge[i]->len%8)>0));
				print_tree(node->edge[i]->node, indent+1);
			}
		}	
		if (indent == 0) printf("-----Tree-----");
	}
*/

/* what should we check?
 * X - is it balanced? (a node with 1 child shoudl not have  
 * a node with 1 child MUST have elem
 * child must be sub of parent
 * edge must be longer than parent edge
 * */
static int addrtree_inconsistent_subtree(struct addredge* parent_edge)
{
	struct addredge* edge;
	struct addrnode* node = parent_edge->node;
	int childcount, i, r;
	
	childcount = (node->edge[0] != NULL) + (node->edge[1] != NULL);
	/* Only nodes with 2 children should possibly have no element. */
	if (childcount < 2 && !node->elem) return 10;
	for (i = 0; i<2; i++) {
		edge = node->edge[i];
		if (!edge) continue;
		if (!edge->node) return 11;
		if (!edge->str) return 12;
		if (edge->len <= parent_edge->len) return 13;
		if (!unittest_wrapper_addrtree_issub(parent_edge->str,
				parent_edge->len, edge->str, edge->len, 0))
			return 14;
		if ((r = addrtree_inconsistent_subtree(edge)) != 0)
			return 15+r;
	}
	return 0;
}

static int addrtree_inconsistent(struct addrtree* tree)
{
	struct addredge* edge;
	int i, r;
	
	if (!tree) return 0;
	if (!tree->root) return 1;
	
	for (i = 0; i<2; i++) {
		edge = tree->root->edge[i];
		if (!edge) continue;
		if (!edge->node) return 3;
		if (!edge->str) return 4;
		if ((r = addrtree_inconsistent_subtree(edge)) != 0)
			return r;
	}
	return 0;
}

static int randomkey(addrkey_t **k, int maxlen)
{
	int byte;
	int bits = rand() % maxlen;
	int bytes = bits/8 + (bits%8>0); /*ceil*/
	*k = (addrkey_t *) malloc(bytes * sizeof(addrkey_t));
	for (byte = 0; byte < bytes; byte++) {
		(*k)[byte] = (addrkey_t)(rand() & 0xFF);
	}
	return bits;
}

static void elemfree(void *envptr, void *elemptr)
{
	struct reply_info *elem = (struct reply_info *)elemptr;
	struct module_env *env = (struct module_env *)envptr;
	free(elem);
}

static void consistency_test(void)
{
	int i, l, r;
	addrkey_t *k;
	struct addrtree* t;
	struct module_env env;
	struct reply_info *elem;
	time_t timenow = 0;
	unit_show_func("edns-subnet/addrtree.h", "Tree consistency check");
	srand(9195); /* just some value for reproducibility */

	t = addrtree_create(100, &elemfree, NULL, &env);

	for (i = 0; i < 1000; i++) {
		l = randomkey(&k, 128);
		elem = (struct reply_info *) calloc(1, sizeof(struct reply_info));
		addrtree_insert(t, k, l, 64, elem, timenow + 10);
		free(k);
		unit_assert( !addrtree_inconsistent(t) );
	}
}

static void issub_test(void)
{
	unit_show_func("edns-subnet/addrtree.h", "issub");
	addrkey_t k1[] = {0x55, 0x55, 0x5A};
	addrkey_t k2[] = {0x55, 0x5D, 0x5A};
	unit_assert( !unittest_wrapper_addrtree_issub(k1, 24, k2, 24,  0) );
	unit_assert(  unittest_wrapper_addrtree_issub(k1,  8, k2, 16,  0) );
	unit_assert(  unittest_wrapper_addrtree_issub(k2, 12, k1, 13,  0) );
	unit_assert( !unittest_wrapper_addrtree_issub(k1, 16, k2, 12,  0) );
	unit_assert(  unittest_wrapper_addrtree_issub(k1, 12, k2, 12,  0) );
	unit_assert( !unittest_wrapper_addrtree_issub(k1, 13, k2, 13,  0) );
	unit_assert(  unittest_wrapper_addrtree_issub(k1, 24, k2, 24, 13) );
	unit_assert( !unittest_wrapper_addrtree_issub(k1, 24, k2, 20, 13) );
	unit_assert(  unittest_wrapper_addrtree_issub(k1, 20, k2, 24, 13) );
}

static void getbit_test(void)
{
	unit_show_func("edns-subnet/addrtree.h", "getbit");
	addrkey_t k1[] = {0x55, 0x55, 0x5A};
	int i;
	for(i = 0; i<20; i++) {
		unit_assert( unittest_wrapper_addrtree_getbit(k1, 20, i) == (i&1) );
	}
}

static void bits_common_test(void)
{
	unit_show_func("edns-subnet/addrtree.h", "bits_common");
	addrkey_t k1[] = {0x12, 0x34, 0x56, 0x78, 0x9A, 0xBC, 0xDE, 0xF0};
	addrkey_t k2[] = {0,0,0,0,0,0,0,0};
	int i;
	
	for(i = 0; i<64; i++) {
		unit_assert( unittest_wrapper_addrtree_bits_common(k1, 64, k1, 64, i) == 64 );
	}
	for(i = 0; i<8; i++) {
		k2[i] = k1[i]^(1<<i);
	}
	unit_assert( unittest_wrapper_addrtree_bits_common(k1, 64, k2, 64,  0) == 0*8+7 );
	unit_assert( unittest_wrapper_addrtree_bits_common(k1, 64, k2, 64,  8) == 1*8+6 );
	unit_assert( unittest_wrapper_addrtree_bits_common(k1, 64, k2, 64, 16) == 2*8+5 );
	unit_assert( unittest_wrapper_addrtree_bits_common(k1, 64, k2, 64, 24) == 3*8+4 );
	unit_assert( unittest_wrapper_addrtree_bits_common(k1, 64, k2, 64, 32) == 4*8+3 );
	unit_assert( unittest_wrapper_addrtree_bits_common(k1, 64, k2, 64, 40) == 5*8+2 );
	unit_assert( unittest_wrapper_addrtree_bits_common(k1, 64, k2, 64, 48) == 6*8+1 );
	unit_assert( unittest_wrapper_addrtree_bits_common(k1, 64, k2, 64, 56) == 7*8+0 );
}

static void cmpbit_test(void)
{
	unit_show_func("edns-subnet/addrtree.h", "cmpbit");
	addrkey_t k1[] = {0xA5, 0x0F};
	addrkey_t k2[] = {0x5A, 0xF0};
	int i;
	
	for(i = 0; i<16; i++) {
		unit_assert( !unittest_wrapper_addrtree_cmpbit(k1,k1,i) );
		unit_assert(  unittest_wrapper_addrtree_cmpbit(k1,k2,i) );
	}
}

void vandergaast_test(void)
{
	unit_show_feature("vandergaast");
	cmpbit_test();
	bits_common_test();
	getbit_test();
	issub_test();
	consistency_test();
}
#endif /* CLIENT_SUBNET */

