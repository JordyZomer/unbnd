/*
 * services/outside_network.h - listen to answers from the network
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
 * This file has functions to send queries to authoritative servers,
 * and wait for the pending answer, with timeouts.
 */

#ifndef OUTSIDE_NETWORK_H
#define OUTSIDE_NETWORK_H

#include "config.h"
#include "util/rbtree.h"
#include "util/netevent.h"
struct pending;
struct pending_timeout;
struct ub_randstate;
struct pending_tcp;
struct waiting_tcp;
struct infra_cache;

/**
 * Send queries to outside servers and wait for answers from servers.
 * Contains answer-listen sockets.
 */
struct outside_network {
	/** Base for select calls */
	struct comm_base* base;

	/** buffer shared by UDP connections, since there is only one
	    datagram at any time. */
	ldns_buffer* udp_buff;

	/** buffer for storage. (buffer for incoming connections, since
	 * either an event to outside or incoming happens, but not both 
	 * This buffer is used during callbacks, so that the datagram
	 * that just arrived does not collide with new datagrams sent out. */
	ldns_buffer* udp_second;

	/** 
	 * Array of udp comm point* that are used to listen to pending events.
	 * Each is on a different port. This is for ip4 ports.
	 */
	struct comm_point** udp4_ports;
	/** number of udp4 ports */
	size_t num_udp4;

	/**
	 * The opened ip6 ports.
	 */
	struct comm_point** udp6_ports;
	/** number of udp6 ports */
	size_t num_udp6;

	/** pending udp answers. sorted by id, addr */
	rbtree_t* pending;
	/** serviced queries, sorted by qbuf, addr, dnssec */
	rbtree_t* serviced;
	/** host cache, pointer but not owned by outnet. */
	struct infra_cache* infra;
	/** where to get random numbers */
	struct ub_randstate* rnd;

	/**
	 * Array of tcp pending used for outgoing TCP connections.
	 * Each can be used to establish a TCP connection with a server.
	 * The file descriptors are -1 if they are free, and need to be 
	 * opened for the tcp connection. Can be used for ip4 and ip6.
	 */
	struct pending_tcp **tcp_conns;
	/** number of tcp communication points. */
	size_t num_tcp;
	/** list of tcp comm points that are free for use */
	struct pending_tcp* tcp_free;
	/** list of tcp queries waiting for a buffer */
	struct waiting_tcp* tcp_wait_first;
	/** last of waiting query list */
	struct waiting_tcp* tcp_wait_last;
};

/**
 * A query that has an answer pending for it.
 */
struct pending {
	/** redblacktree entry, key is the pending struct(id, addr). */
	rbnode_t node;
	/** the ID for the query */
	uint16_t id;
	/** remote address. */
	struct sockaddr_storage addr;
	/** length of addr field in use. */
	socklen_t addrlen;
	/** comm point it was sent on (and reply must come back on). */
	struct comm_point* c;
	/** timeout event */
	struct comm_timer* timer;
	/** callback for the timeout, error or reply to the message */
	comm_point_callback_t* cb;
	/** callback user argument */
	void* cb_arg;
	/** the outside network it is part of */
	struct outside_network* outnet;
};

/**
 * Pending TCP query to server.
 */
struct pending_tcp {
	/** next in list of free tcp comm points, or NULL. */
	struct pending_tcp* next_free;
	/** the ID for the query; checked in reply */
	uint16_t id;
	/** tcp comm point it was sent on (and reply must come back on). */
	struct comm_point* c;
	/** the query being serviced, NULL if the pending_tcp is unused. */
	struct waiting_tcp* query;
};

/**
 * Query waiting for TCP buffer.
 */
struct waiting_tcp {
	/** 
	 * next in waiting list.
	 * if pkt==0, this points to the pending_tcp structure.
	 */
	struct waiting_tcp* next_waiting;
	/** timeout event; timer keeps running whether the query is
	 * waiting for a buffer or the tcp reply is pending */
	struct comm_timer* timer;
	/** the outside network it is part of */
	struct outside_network* outnet;
	/** remote address. */
	struct sockaddr_storage addr;
	/** length of addr field in use. */
	socklen_t addrlen;
	/** 
	 * The query itself, the query packet to send.
	 * allocated after the waiting_tcp structure.
	 * set to NULL when the query is serviced and it part of pending_tcp.
	 * if this is NULL, the next_waiting points to the pending_tcp.
	 */
	uint8_t* pkt;
	/** length of query packet. */
	size_t pkt_len;
	/** callback for the timeout, error or reply to the message */
	comm_point_callback_t* cb;
	/** callback user argument */
	void* cb_arg;
};

/**
 * Callback to party interested in serviced query results.
 */
struct service_callback {
	/** next in callback list */
	struct service_callback* next;
	/** callback function */
	comm_point_callback_t* cb;
	/** user argument for callback function */
	void* cb_arg;
};

/**
 * Query service record.
 * Contains query and destination. UDP, TCP, EDNS are all tried.
 * complete with retries and timeouts. A number of interested parties can
 * receive a callback.
 */
struct serviced_query {
	/** The rbtree node, key is this record */
	rbnode_t node;
	/** The query that needs to be answered. Starts with flags u16,
	 * then qdcount, ..., including qname, qtype, qclass. Does not include
	 * EDNS record. */
	uint8_t* qbuf;
	/** length of qbuf. */
	size_t qbuflen;
	/** If an EDNS section is included, the DO bit will be turned on. */
	int dnssec;
	/** where to send it */
	struct sockaddr_storage addr;
	/** length of addr field in use. */
	socklen_t addrlen;
	/** current status */
	enum serviced_query_status {
		/** initial status */
		serviced_initial,
		/** UDP with EDNS sent */
		serviced_query_UDP_EDNS,
		/** UDP without EDNS sent */
		serviced_query_UDP,
		/** TCP with EDNS sent */
		serviced_query_TCP_EDNS,
		/** TCP without EDNS sent */
		serviced_query_TCP
	} status;
	/** true if serviced_query is scheduled for deletion already */
	int to_be_deleted;
	/** number of UDP retries */
	int retry;
	/** time last UDP was sent */
	struct timeval last_sent_time;
	/** outside network this is part of */
	struct outside_network* outnet;
	/** list of interested parties that need callback on results. */
	struct service_callback* cblist;
	/** the UDP or TCP query that is pending, see status which */
	void* pending;
};

/**
 * Create outside_network structure with N udp ports.
 * @param base: the communication base to use for event handling.
 * @param bufsize: size for network buffers.
 * @param num_ports: number of udp ports to open per interface.
 * @param ifs: interface names (or NULL for default interface).
 *    These interfaces must be able to access all authoritative servers.
 * @param num_ifs: number of names in array ifs.
 * @param do_ip4: service IP4.
 * @param do_ip6: service IP6.
 * @param port_base: if -1 system assigns ports, otherwise try to get
 *    the ports numbered from this starting number.
 * @param num_tcp: number of outgoing tcp buffers to preallocate.
 * @param infra: pointer to infra cached used for serviced queries.
 * @param rnd: stored to create random numbers for serviced queries.
 * @return: the new structure (with no pending answers) or NULL on error.
 */
struct outside_network* outside_network_create(struct comm_base* base,
	size_t bufsize, size_t num_ports, char** ifs, int num_ifs,
	int do_ip4, int do_ip6, int port_base, size_t num_tcp, 
	struct infra_cache* infra, struct ub_randstate* rnd);

/**
 * Delete outside_network structure.
 * @param outnet: object to delete.
 */
void outside_network_delete(struct outside_network* outnet);

/**
 * Set secondary UDP buffer. Make sure it is not used during outside network
 * callbacks. Such as the incoming network UDP buffer. Caller responsible
 * for deletion.
 * @param outnet: outside network.
 * @param buf: buffer to use as secondary buffer.
 */
void outside_network_set_secondary_buffer(struct outside_network* outnet,
	ldns_buffer* buf);

/**
 * Send UDP query, create pending answer.
 * Changes the ID for the query to be random and unique for that destination.
 * @param outnet: provides the event handling
 * @param packet: wireformat query to send to destination.
 * @param addr: address to send to.
 * @param addrlen: length of addr.
 * @param timeout: in milliseconds from now.
 * @param callback: function to call on error, timeout or reply.
 * @param callback_arg: user argument for callback function.
 * @param rnd: random state for generating ID and port.
 * @return: NULL on error for malloc or socket. Else the pending query object.
 */
struct pending* pending_udp_query(struct outside_network* outnet, 
	ldns_buffer* packet, struct sockaddr_storage* addr, 
	socklen_t addrlen, int timeout, comm_point_callback_t* callback, 
	void* callback_arg, struct ub_randstate* rnd);

/**
 * Send TCP query. May wait for TCP buffer. Selects ID to be random, and 
 * checks id.
 * @param outnet: provides the event handling.
 * @param packet: wireformat query to send to destination. copied from.
 * @param addr: address to send to.
 * @param addrlen: length of addr.
 * @param timeout: in seconds from now.
 *    Timer starts running now. Timer may expire if all buffers are used,
 *    without any query been sent to the server yet.
 * @param callback: function to call on error, timeout or reply.
 * @param callback_arg: user argument for callback function.
 * @param rnd: random state for generating ID.
 * @return: false on error for malloc or socket. Else the pending TCP object.
 */
struct waiting_tcp* pending_tcp_query(struct outside_network* outnet, 
	ldns_buffer* packet, struct sockaddr_storage* addr, 
	socklen_t addrlen, int timeout, comm_point_callback_t* callback, 
	void* callback_arg, struct ub_randstate* rnd);

/**
 * Delete pending answer.
 * @param outnet: outside network the pending query is part of.
 *    Internal feature: if outnet is NULL, p is not unlinked from rbtree.
 * @param p: deleted
 */
void pending_delete(struct outside_network* outnet, struct pending* p);

/**
 * Perform a serviced query to the authoritative servers.
 * Duplicate efforts are detected, and EDNS, TCP and UDP retry is performed.
 * @param outnet: outside network, with rbtree of serviced queries.
 * @param qname: what qname to query.
 * @param qnamelen: length of qname in octets including 0 root label.
 * @param qtype: rrset type to query (host format)
 * @param qclass: query class. (host format)
 * @param flags: flags u16 (host format), includes opcode, CD bit.
 * @param dnssec: if set, DO bit is set in EDNS queries.
 * @param callback: callback function.
 * @param callback_arg: user argument to callback function.
 * @param addr: to which server to send the query.
 * @param addrlen: length of addr.
 * @param buff: scratch buffer to create query contents in. Empty on exit.
 * @param arg_compare: function to compare callback args, return true if 
 * 	identical. It is given the callback_arg and args that are listed.
 * @return 0 on error, or pointer to serviced query that is used to answer
 *	this serviced query may be shared with other callbacks as well.
 */
struct serviced_query* outnet_serviced_query(struct outside_network* outnet,
	uint8_t* qname, size_t qnamelen, uint16_t qtype, uint16_t qclass,
	uint16_t flags, int dnssec, struct sockaddr_storage* addr, 
	socklen_t addrlen, comm_point_callback_t* callback, 
	void* callback_arg, ldns_buffer* buff, 
	int (*arg_compare)(void*,void*));

/**
 * Remove service query callback.
 * If that leads to zero callbacks, the query is completely cancelled.
 * @param sq: serviced query to adjust.
 * @param cb_arg: callback argument of callback that needs removal.
 *	same as the callback_arg to outnet_serviced_query().
 */
void outnet_serviced_query_stop(struct serviced_query* sq, void* cb_arg);

/**
 * Get memory size in use by outside network.
 * Counts buffers and outstanding query (serviced queries) malloced data.
 * @param outnet: outside network structure.
 * @return size in bytes.
 */
size_t outnet_get_mem(struct outside_network* outnet);

#endif /* OUTSIDE_NETWORK_H */
