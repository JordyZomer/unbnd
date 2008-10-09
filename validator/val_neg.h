/*
 * validator/val_neg.h - validator aggressive negative caching functions.
 *
 * Copyright (c) 2008, NLnet Labs. All rights reserved.
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
 * This file contains helper functions for the validator module.
 * The functions help with aggressive negative caching.
 * This creates new denials of existance, and proofs for absence of types
 * from cached NSEC records.
 */

#ifndef VALIDATOR_VAL_NEG_H
#define VALIDATOR_VAL_NEG_H
#include "util/locks.h"
#include "util/rbtree.h"
struct val_neg_data;
struct config_file;
struct reply_info;
struct rrset_cache;
struct regional;
struct query_info;
struct dns_msg;

/**
 * The negative cache.  It is shared between the threads, so locked. 
 * Kept as validator-environ-state.  It refers back to the rrset cache for
 * data elements.  It can be out of date and contain conflicting data 
 * from zone content changes.  
 * It contains a tree of zones, every zone has a tree of data elements.
 * The data elements are part of one big LRU list, with one memory counter.
 */
struct val_neg_cache {
	/** the big lock on the negative cache.  Because we use a rbtree 
	 * for the data (quick lookup), we need a big lock */
	lock_basic_t lock;
	/** The zone rbtree. contents sorted canonical, type val_neg_zone */
	rbtree_t tree;
	/** the first in linked list of LRU of val_neg_data */
	struct val_neg_data* first;
	/** last in lru (least recently used element) */
	struct val_neg_data* last;
	/** current memory in use (bytes) */
	size_t use;
	/** max memory to use (bytes) */
	size_t max;
	/** max nsec3 iterations allowed */
	size_t nsec3_max_iter;
};

/**
 * Per Zone aggressive negative caching data.
 */
struct val_neg_zone {
	/** rbtree node element, key is this struct: the name, class */
	rbnode_t node;
	/** name; the key */
	uint8_t* name;
	/** length of name */
	size_t len;
	/** labels in name */
	int labs;

	/** pointer to parent zone in the negative cache */
	struct val_neg_zone* parent;

	/** the number of elements, including this one and the ones whose
	 * parents (-parents) include this one, that are in_use 
	 * No elements have a count of zero, those are removed. */
	int count;

	/** if 0: NSEC zone, else NSEC3 hash algorithm in use */
	int nsec3_hash;
	/** nsec3 iteration count in use */
	size_t nsec3_iter;
	/** nsec3 salt in use */
	uint8_t* nsec3_salt;
	/** length of salt in bytes */
	size_t nsec3_saltlen;

	/** tree of NSEC data for this zone, sorted canonical 
	 * by NSEC owner name */
	rbtree_t tree;

	/** class of node; host order */
	uint16_t dclass;
	/** if this element is in use, boolean */
	uint8_t in_use;
};

/**
 * Data element for aggressive negative caching.
 * The tree of these elements acts as an index onto the rrset cache.
 * It shows the NSEC records that (may) exist and are (possibly) secure.
 * The rbtree allows for logN search for a covering NSEC record.
 * To make tree insertion and deletion logN too, all the parent (one label
 * less than the name) data elements are also in the rbtree, with a usage
 * count for every data element.
 * There is no actual data stored in this data element, if it is in_use,
 * then the data can (possibly) be found in the rrset cache.
 */
struct val_neg_data {
	/** rbtree node element, key is this struct: the name */
	rbnode_t node;
	/** name; the key */
	uint8_t* name;
	/** length of name */
	size_t len;
	/** labels in name */
	int labs;

	/** pointer to parent node in the negative cache */
	struct val_neg_data* parent;

	/** the number of elements, including this one and the ones whose
	 * parents (-parents) include this one, that are in use 
	 * No elements have a count of zero, those are removed. */
	int count;

	/** the zone that this denial is part of */
	struct val_neg_zone* zone;

	/** previous in LRU */
	struct val_neg_data* prev;
	/** next in LRU (next element was less recently used) */
	struct val_neg_data* next;

	/** if this element is in use, boolean */
	uint8_t in_use;
};

/**
 * Create negative cache
 * @param cfg: config options.
 * @param maxiter: max nsec3 iterations allowed.
 * @return neg cache, empty or NULL on failure.
 */
struct val_neg_cache* val_neg_create(struct config_file* cfg, size_t maxiter);

/**
 * see how much memory is in use by the negative cache.
 * @param neg: negative cache
 * @return number of bytes in use.
 */
size_t val_neg_get_mem(struct val_neg_cache* neg);

/**
 * Destroy negative cache. There must no longer be any other threads.
 * @param neg: negative cache.
 */
void neg_cache_delete(struct val_neg_cache* neg);

/** 
 * Comparison function for rbtree val neg data elements
 */
int val_neg_data_compare(const void* a, const void* b);

/** 
 * Comparison function for rbtree val neg zone elements
 */
int val_neg_zone_compare(const void* a, const void* b);

/**
 * Insert NSECs from this message into the negative cache for reference.
 * @param neg: negative cache
 * @param rep: reply with NSECs.
 * Errors are ignored, means that storage is omitted.
 */
void val_neg_addreply(struct val_neg_cache* neg, struct reply_info* rep);

/**
 * Insert NSECs from this referral into the negative cache for reference.
 * @param neg: negative cache
 * @param rep: referral reply with NS, NSECs.
 * @param zone: bailiwick for the referral.
 * Errors are ignored, means that storage is omitted.
 */
void val_neg_addreferral(struct val_neg_cache* neg, struct reply_info* rep,
	uint8_t* zone);

/**
 * Perform a DLV style lookup
 * During the lookup, we could find out that data has expired. In that
 * case the neg_cache entries are removed, and lookup fails.
 *
 * @param neg: negative cache.
 * @param qname: name to look for
 * @param len: length of qname.
 * @param qclass: class to look in.
 * @param rrset_cache: the rrset cache, for NSEC lookups.
 * @param now: current time for ttl checks.
 * @return 
 *	0 on error
 *	0 if no proof of negative
 *	1 if indeed negative was proven
 *	  thus, qname DLV qclass does not exist.
 */
int val_neg_dlvlookup(struct val_neg_cache* neg, uint8_t* qname, size_t len,
	uint16_t qclass, struct rrset_cache* rrset_cache, uint32_t now);

/**
 * For the given query, try to get a reply out of the negative cache.
 * The reply still needs to be validated.
 * @param neg: negative cache.
 * @param qinfo: query
 * @param region: where to allocate reply.
 * @param rrset_cache: rrset cache.
 * @param buf: temporary buffer.
 * @param now: to check TTLs against.
 * @return a reply message if something was found. 
 * 	This reply may still need validation.
 * 	NULL if nothing found (or out of memory).
 */
struct dns_msg* val_neg_getmsg(struct val_neg_cache* neg, 
	struct query_info* qinfo, struct regional* region, 
	struct rrset_cache* rrset_cache, ldns_buffer* buf, uint32_t now);

#endif /* VALIDATOR_VAL_NEG_H */
