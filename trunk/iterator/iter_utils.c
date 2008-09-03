/*
 * iterator/iter_utils.c - iterative resolver module utility functions.
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
 * This file contains functions to assist the iterator module.
 * Configuration options. Forward zones. 
 */
#include "config.h"
#include "iterator/iter_utils.h"
#include "iterator/iterator.h"
#include "iterator/iter_hints.h"
#include "iterator/iter_fwd.h"
#include "iterator/iter_donotq.h"
#include "iterator/iter_delegpt.h"
#include "iterator/iter_priv.h"
#include "services/cache/infra.h"
#include "services/cache/dns.h"
#include "services/cache/rrset.h"
#include "util/net_help.h"
#include "util/module.h"
#include "util/log.h"
#include "util/config_file.h"
#include "util/regional.h"
#include "util/data/msgparse.h"
#include "util/data/dname.h"
#include "util/random.h"
#include "util/fptr_wlist.h"
#include "validator/val_anchor.h"

/** fillup fetch policy array */
static void
fetch_fill(struct iter_env* ie, const char* str)
{
	char* s = (char*)str, *e;
	int i;
	for(i=0; i<ie->max_dependency_depth+1; i++) {
		ie->target_fetch_policy[i] = strtol(s, &e, 10);
		if(s == e)
			fatal_exit("cannot parse fetch policy number %s", s);
		s = e;
	}
}

/** Read config string that represents the target fetch policy */
static int
read_fetch_policy(struct iter_env* ie, const char* str)
{
	int count = cfg_count_numbers(str);
	if(count < 1) {
		log_err("Cannot parse target fetch policy: \"%s\"", str);
		return 0;
	}
	ie->max_dependency_depth = count - 1;
	ie->target_fetch_policy = (int*)calloc(
		(size_t)ie->max_dependency_depth+1, sizeof(int));
	if(!ie->target_fetch_policy) {
		log_err("alloc fetch policy: out of memory");
		return 0;
	}
	fetch_fill(ie, str);
	return 1;
}

int 
iter_apply_cfg(struct iter_env* iter_env, struct config_file* cfg)
{
	int i;
	/* target fetch policy */
	if(!read_fetch_policy(iter_env, cfg->target_fetch_policy))
		return 0;
	for(i=0; i<iter_env->max_dependency_depth+1; i++)
		verbose(VERB_QUERY, "target fetch policy for level %d is %d",
			i, iter_env->target_fetch_policy[i]);
	
	if(!iter_env->hints)
		iter_env->hints = hints_create();
	if(!iter_env->hints || !hints_apply_cfg(iter_env->hints, cfg)) {
		log_err("Could not set root or stub hints");
		return 0;
	}
	if(!iter_env->fwds)
		iter_env->fwds = forwards_create();
	if(!iter_env->fwds || !forwards_apply_cfg(iter_env->fwds, cfg)) {
		log_err("Could not set forward zones");
		return 0;
	}
	if(!iter_env->donotq)
		iter_env->donotq = donotq_create();
	if(!iter_env->donotq || !donotq_apply_cfg(iter_env->donotq, cfg)) {
		log_err("Could not set donotqueryaddresses");
		return 0;
	}
	if(!iter_env->priv)
		iter_env->priv = priv_create();
	if(!iter_env->priv || !priv_apply_cfg(iter_env->priv, cfg)) {
		log_err("Could not set private addresses");
		return 0;
	}
	iter_env->supports_ipv6 = cfg->do_ip6;
	return 1;
}

/** filter out unsuitable targets, return rtt or -1 */
static int
iter_filter_unsuitable(struct iter_env* iter_env, struct module_env* env,
	uint8_t* name, size_t namelen, uint16_t qtype, uint32_t now, 
	struct delegpt_addr* a)
{
	int rtt;
	int lame;
	int dnsseclame;
	if(donotq_lookup(iter_env->donotq, &a->addr, a->addrlen)) {
		return -1; /* server is on the donotquery list */
	}
	if(!iter_env->supports_ipv6 && addr_is_ip6(&a->addr, a->addrlen)) {
		return -1; /* there is no ip6 available */
	}
	/* check lameness - need zone , class info */
	if(infra_get_lame_rtt(env->infra_cache, &a->addr, a->addrlen, 
		name, namelen, qtype, &lame, &dnsseclame, &rtt, now)) {
		if(lame)
			return -1; /* server is lame */
		else if(rtt >= USEFUL_SERVER_TOP_TIMEOUT)
			return -1; /* server is unresponsive */
		else if(dnsseclame)
			return rtt+USEFUL_SERVER_TOP_TIMEOUT; /* nonpref */
		else	return rtt;
	}
	/* no server information present */
	return UNKNOWN_SERVER_NICENESS;
}

/** lookup RTT information, and also store fastest rtt (if any) */
static int
iter_fill_rtt(struct iter_env* iter_env, struct module_env* env,
	uint8_t* name, size_t namelen, uint16_t qtype, uint32_t now, 
	struct delegpt* dp, int* best_rtt)
{
	int got_it = 0;
	struct delegpt_addr* a;
	for(a=dp->result_list; a; a = a->next_result) {
		a->sel_rtt = iter_filter_unsuitable(iter_env, env, 
			name, namelen, qtype, now, a);
		if(a->sel_rtt != -1) {
			if(!got_it) {
				*best_rtt = a->sel_rtt;
				got_it = 1;
			} else if(a->sel_rtt < *best_rtt) {
				*best_rtt = a->sel_rtt;
			}
		}
	}
	return got_it;
}

/** filter the addres list, putting best targets at front,
 * returns number of best targets (or 0, no suitable targets) */
static int
iter_filter_order(struct iter_env* iter_env, struct module_env* env,
	uint8_t* name, size_t namelen, uint16_t qtype, uint32_t now, 
	struct delegpt* dp, int* selected_rtt)
{
	int got_num = 0, low_rtt = 0, swap_to_front;
	struct delegpt_addr* a, *n, *prev=NULL;

	/* fillup sel_rtt and find best rtt in the bunch */
	got_num = iter_fill_rtt(iter_env, env, name, namelen, qtype, now, dp, 
		&low_rtt);
	if(got_num == 0) 
		return 0;

	got_num = 0;
	a = dp->result_list;
	while(a) {
		/* skip unsuitable targets */
		if(a->sel_rtt == -1) {
			prev = a;
			a = a->next_result;
			continue;
		}
		/* classify the server address and determine what to do */
		swap_to_front = 0;
		if(a->sel_rtt >= low_rtt && a->sel_rtt - low_rtt <= RTT_BAND) {
			got_num++;
			swap_to_front = 1;
		} else if(a->sel_rtt<low_rtt && low_rtt-a->sel_rtt<=RTT_BAND) {
			got_num++;
			swap_to_front = 1;
		}
		/* swap to front if necessary, or move to next result */
		if(swap_to_front && prev) {
			n = a->next_result;
			prev->next_result = n;
			a->next_result = dp->result_list;
			dp->result_list = a;
			a = n;
		} else {
			prev = a;
			a = a->next_result;
		}
	}
	*selected_rtt = low_rtt;
	return got_num;
}

struct delegpt_addr* 
iter_server_selection(struct iter_env* iter_env, 
	struct module_env* env, struct delegpt* dp, 
	uint8_t* name, size_t namelen, uint16_t qtype, int* dnssec_expected)
{
	int sel;
	int selrtt;
	struct delegpt_addr* a, *prev;
	int num = iter_filter_order(iter_env, env, name, namelen, qtype,
		*env->now, dp, &selrtt);

	if(num == 0)
		return NULL;
	if(selrtt >= USEFUL_SERVER_TOP_TIMEOUT)
		*dnssec_expected = 0;
	if(num == 1) {
		a = dp->result_list;
		if(++a->attempts < OUTBOUND_MSG_RETRY)
			return a;
		dp->result_list = a->next_result;
		return a;
	}
	/* randomly select a target from the list */
	log_assert(num > 1);
	/* we do not need secure random numbers here, but
	 * we do need it to be threadsafe, so we use this */
	sel = ub_random(env->rnd) % num; 
	a = dp->result_list;
	prev = NULL;
	while(sel > 0 && a) {
		prev = a;
		a = a->next_result;
		sel--;
	}
	if(!a)  /* robustness */
		return NULL;
	if(++a->attempts < OUTBOUND_MSG_RETRY)
		return a;
	/* remove it from the delegation point result list */
	if(prev)
		prev->next_result = a->next_result;
	else	dp->result_list = a->next_result;
	return a;
}

struct dns_msg* 
dns_alloc_msg(ldns_buffer* pkt, struct msg_parse* msg, 
	struct regional* region)
{
	struct dns_msg* m = (struct dns_msg*)regional_alloc(region,
		sizeof(struct dns_msg));
	if(!m)
		return NULL;
	memset(m, 0, sizeof(*m));
	if(!parse_create_msg(pkt, msg, NULL, &m->qinfo, &m->rep, region)) {
		log_err("malloc failure: allocating incoming dns_msg");
		return NULL;
	}
	return m;
}

struct dns_msg* 
dns_copy_msg(struct dns_msg* from, struct regional* region)
{
	struct dns_msg* m = (struct dns_msg*)regional_alloc(region,
		sizeof(struct dns_msg));
	if(!m)
		return NULL;
	m->qinfo = from->qinfo;
	if(!(m->qinfo.qname = regional_alloc_init(region, from->qinfo.qname,
		from->qinfo.qname_len)))
		return NULL;
	if(!(m->rep = reply_info_copy(from->rep, NULL, region)))
		return NULL;
	return m;
}

int 
iter_dns_store(struct module_env* env, struct query_info* msgqinf,
	struct reply_info* msgrep, int is_referral)
{
	return dns_cache_store(env, msgqinf, msgrep, is_referral);
}

int 
iter_ns_probability(struct ub_randstate* rnd, int n, int m)
{
	int sel;
	if(n == m) /* 100% chance */
		return 1;
	/* we do not need secure random numbers here, but
	 * we do need it to be threadsafe, so we use this */
	sel = ub_random(rnd) % m; 
	return (sel < n);
}

/** detect dependency cycle for query and target */
static int
causes_cycle(struct module_qstate* qstate, uint8_t* name, size_t namelen,
	uint16_t t, uint16_t c)
{
	struct query_info qinf;
	qinf.qname = name;
	qinf.qname_len = namelen;
	qinf.qtype = t;
	qinf.qclass = c;
	fptr_ok(fptr_whitelist_modenv_detect_cycle(
		qstate->env->detect_cycle));
	return (*qstate->env->detect_cycle)(qstate, &qinf, 
		(uint16_t)(BIT_RD|BIT_CD), qstate->is_priming);
}

void 
iter_mark_cycle_targets(struct module_qstate* qstate, struct delegpt* dp)
{
	struct delegpt_ns* ns;
	for(ns = dp->nslist; ns; ns = ns->next) {
		if(ns->resolved)
			continue;
		/* see if this ns as target causes dependency cycle */
		if(causes_cycle(qstate, ns->name, ns->namelen, 
			LDNS_RR_TYPE_AAAA, qstate->qinfo.qclass) ||
		   causes_cycle(qstate, ns->name, ns->namelen, 
			LDNS_RR_TYPE_A, qstate->qinfo.qclass)) {
			log_nametypeclass(VERB_QUERY, "skipping target due "
			 	"to dependency cycle (harden-glue: no may "
				"fix some of the cycles)", 
				ns->name, LDNS_RR_TYPE_A, 
				qstate->qinfo.qclass);
			ns->resolved = 1;
		}
	}
}

int 
iter_dp_is_useless(struct module_qstate* qstate, struct delegpt* dp)
{
	struct delegpt_ns* ns;
	/* check:
	 *      o all NS items are required glue.
	 *      o no addresses are provided.
	 *      o RD qflag is on.
	 * OR
	 *      o no addresses are provided.
	 *      o RD qflag is on.
	 *      o the query is for one of the nameservers in dp,
	 *        and that nameserver is a glue-name for this dp.
	 */
	if(!(qstate->query_flags&BIT_RD))
		return 0;
	/* either available or unused targets */
	if(dp->usable_list || dp->result_list) 
		return 0;
	
	/* see if query is for one of the nameservers, which is glue */
	if( (qstate->qinfo.qtype == LDNS_RR_TYPE_A ||
		qstate->qinfo.qtype == LDNS_RR_TYPE_AAAA) &&
		dname_subdomain_c(qstate->qinfo.qname, dp->name) &&
		delegpt_find_ns(dp, qstate->qinfo.qname, 
			qstate->qinfo.qname_len))
		return 1;
	
	for(ns = dp->nslist; ns; ns = ns->next) {
		if(ns->resolved) /* skip failed targets */
			continue;
		if(!dname_subdomain_c(ns->name, dp->name))
			return 0; /* one address is not required glue */
	}
	return 1;
}

int 
iter_indicates_dnssec(struct module_env* env, struct delegpt* dp,
        struct dns_msg* msg, uint16_t dclass)
{
	/* information not available, !env->anchors can be common */
	if(!env || !env->anchors || !dp || !dp->name)
		return 0;
	/* a trust anchor exists with this name, RRSIGs expected */
	if(anchor_find(env->anchors, dp->name, dp->namelabs, dp->namelen,
		dclass))
		return 1;
	/* see if DS rrset was given, in AUTH section */
	if(msg && msg->rep &&
		reply_find_rrset_section_ns(msg->rep, dp->name, dp->namelen,
		LDNS_RR_TYPE_DS, dclass))
		return 1;
	return 0;
}

int 
iter_msg_has_dnssec(struct dns_msg* msg)
{
	size_t i;
	if(!msg || !msg->rep)
		return 0;
	for(i=0; i<msg->rep->an_numrrsets + msg->rep->ns_numrrsets; i++) {
		if(((struct packed_rrset_data*)msg->rep->rrsets[i]->
			entry.data)->rrsig_count > 0)
			return 1;
	}
	/* empty message has no DNSSEC info, with DNSSEC the reply is
	 * not empty (NSEC) */
	return 0;
}

int iter_msg_from_zone(struct dns_msg* msg, struct delegpt* dp,
        enum response_type type, uint16_t dclass)
{
	if(!msg || !dp || !msg->rep || !dp->name)
		return 0;
	/* SOA RRset - always from reply zone */
	if(reply_find_rrset_section_an(msg->rep, dp->name, dp->namelen,
		LDNS_RR_TYPE_SOA, dclass) ||
	   reply_find_rrset_section_ns(msg->rep, dp->name, dp->namelen,
		LDNS_RR_TYPE_SOA, dclass))
		return 1;
	if(type == RESPONSE_TYPE_REFERRAL) {
		size_t i;
		/* if it adds a single label, i.e. we expect .com,
		 * and referral to example.com. NS ... , then origin zone
		 * is .com. For a referral to sub.example.com. NS ... then
		 * we do not know, since example.com. may be in between. */
		for(i=0; i<msg->rep->an_numrrsets+msg->rep->ns_numrrsets; 
			i++) {
			struct ub_packed_rrset_key* s = msg->rep->rrsets[i];
			if(ntohs(s->rk.type) == LDNS_RR_TYPE_NS &&
				ntohs(s->rk.rrset_class) == dclass) {
				int l = dname_count_labels(s->rk.dname);
				if(l == dp->namelabs + 1 &&
					dname_strict_subdomain(s->rk.dname,
					l, dp->name, dp->namelabs))
					return 1;
			}
		}
		return 0;
	}
	log_assert(type==RESPONSE_TYPE_ANSWER || type==RESPONSE_TYPE_CNAME);
	/* not a referral, and not lame delegation (upwards), so, 
	 * any NS rrset must be from the zone itself */
	if(reply_find_rrset_section_an(msg->rep, dp->name, dp->namelen,
		LDNS_RR_TYPE_NS, dclass) ||
	   reply_find_rrset_section_ns(msg->rep, dp->name, dp->namelen,
		LDNS_RR_TYPE_NS, dclass))
		return 1;
	return 0;
}
