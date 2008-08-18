/*
 * util/log.c - implementation of the log code
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
 * Implementation of log.h.
 */

#include "config.h"
#include "util/log.h"
#include "util/locks.h"
#ifdef HAVE_TIME_H
#include <time.h>
#endif
#ifdef HAVE_SYSLOG_H
#  include <syslog.h>
#else
/**define LOG_ constants */
#  define LOG_CRIT 2
#  define LOG_ERR 3
#  define LOG_WARNING 4
#  define LOG_NOTICE 5
#  define LOG_INFO 6
#  define LOG_DEBUG 7
#endif

/* default verbosity */
enum verbosity_value verbosity = 0;
/** the file logged to. */
static FILE* logfile = 0;
/** if key has been created */
static int key_created = 0;
/** pthread key for thread ids in logfile */
static ub_thread_key_t logkey;
/** the identity of this executable/process */
static const char* ident="unbound";
#ifdef HAVE_SYSLOG_H
/** are we using syslog(3) to log to */
static int logging_to_syslog = 0;
#endif /* HAVE_SYSLOG_H */
/** time to print in log, if NULL, use time(2) */
static uint32_t* log_now = NULL;

void
log_init(const char* filename, int use_syslog, const char* chrootdir)
{
	FILE *f;
	if(!key_created) {
		key_created = 1;
		ub_thread_key_create(&logkey, NULL);
	}
	if(logfile 
#ifdef HAVE_SYSLOG_H
	|| logging_to_syslog
#endif
	)
	verbose(VERB_QUERY, "switching log to %s", 
		use_syslog?"syslog":(filename&&filename[0]?filename:"stderr"));
	if(logfile && logfile != stderr)
		fclose(logfile);
#ifdef HAVE_SYSLOG_H
	if(logging_to_syslog) {
		closelog();
		logging_to_syslog = 0;
	}
	if(use_syslog) {
		openlog(ident, 0, LOG_DAEMON);
		logging_to_syslog = 1;
		return;
	}
#endif /* HAVE_SYSLOG_H */
	if(!filename || !filename[0]) {
		logfile = stderr;
		return;
	}
	/* open the file for logging */
	if(chrootdir && chrootdir[0] && strncmp(filename, chrootdir,
		strlen(chrootdir)) == 0) 
		filename += strlen(chrootdir);
	f = fopen(filename, "a");
	if(!f) {
		log_err("Could not open logfile %s: %s", filename, 
			strerror(errno));
		return;
	}
	logfile = f;
}

void log_file(FILE *f)
{
	logfile = f;
}

void log_thread_set(int* num)
{
	ub_thread_key_set(logkey, num);
}

void log_ident_set(const char* id)
{
	ident = id;
}

void log_set_time(uint32_t* t)
{
	log_now = t;
}

void
log_vmsg(int pri, const char* type,
	const char *format, va_list args)
{
	char message[MAXSYSLOGMSGLEN];
	unsigned int* tid = (unsigned int*)ub_thread_key_get(logkey);
	uint32_t now;
	(void)pri;
	vsnprintf(message, sizeof(message), format, args);
#ifdef HAVE_SYSLOG_H
	if(logging_to_syslog) {
		syslog(pri, "[%d:%x] %s: %s", 
			(int)getpid(), tid?*tid:0, type, message);
		return;
	}
#endif /* HAVE_SYSLOG_H */
	if(!logfile) return;
	if(log_now)
		now = *log_now;
	else	now = (uint32_t)time(NULL);
	fprintf(logfile, "[%u] %s[%d:%x] %s: %s\n", (unsigned)now, 
		ident, (int)getpid(), tid?*tid:0, type, message);
	fflush(logfile);
}

/**
 * implementation of log_info
 * @param format: format string printf-style.
 */
void
log_info(const char *format, ...)
{
        va_list args;
	va_start(args, format);
	log_vmsg(LOG_INFO, "info", format, args);
	va_end(args);
}

/**
 * implementation of log_err
 * @param format: format string printf-style.
 */
void
log_err(const char *format, ...)
{
        va_list args;
	va_start(args, format);
	log_vmsg(LOG_ERR, "error", format, args);
	va_end(args);
}

/**
 * implementation of log_warn
 * @param format: format string printf-style.
 */
void
log_warn(const char *format, ...)
{
        va_list args;
	va_start(args, format);
	log_vmsg(LOG_WARNING, "warning", format, args);
	va_end(args);
}

/**
 * implementation of fatal_exit
 * @param format: format string printf-style.
 */
void
fatal_exit(const char *format, ...)
{
        va_list args;
	va_start(args, format);
	log_vmsg(LOG_CRIT, "fatal error", format, args);
	va_end(args);
	exit(1);
}

/**
 * implementation of verbose
 * @param level: verbose level for the message.
 * @param format: format string printf-style.
 */
void
verbose(enum verbosity_value level, const char* format, ...)
{
        va_list args;
	va_start(args, format);
	if(verbosity >= level) {
		if(level == VERB_OPS)
			log_vmsg(LOG_NOTICE, "notice", format, args);
		else if(level == VERB_DETAIL)
			log_vmsg(LOG_INFO, "info", format, args);
		else	log_vmsg(LOG_DEBUG, "debug", format, args);
	}
	va_end(args);
}

static void 
log_hex_f(enum verbosity_value v, const char* msg, void* data, size_t length)
{
	size_t i, j;
	uint8_t* data8 = (uint8_t*)data;
	const char* hexchar = "0123456789ABCDEF";
	char buf[1024+1]; /* alloc blocksize hex chars + \0 */
	const size_t blocksize = 512;
	size_t len;

	if(length == 0) {
		verbose(v, "%s[%u]", msg, (unsigned)length);
		return;
	}

	for(i=0; i<length; i+=blocksize/2) {
		len = blocksize/2;
		if(length - i < blocksize/2)
			len = length - i;
		for(j=0; j<len; j++) {
			buf[j*2] = hexchar[ data8[i+j] >> 4 ];
			buf[j*2 + 1] = hexchar[ data8[i+j] & 0xF ];
		}
		buf[len*2] = 0;
		verbose(v, "%s[%u:%u] %.*s", msg, (unsigned)length, 
			(unsigned)i, (int)len*2, buf);
	}
}

void 
log_hex(const char* msg, void* data, size_t length)
{
	log_hex_f(verbosity, msg, data, length);
}

void log_buf(enum verbosity_value level, const char* msg, ldns_buffer* buf)
{
	if(verbosity < level)
		return;
	log_hex_f(level, msg, ldns_buffer_begin(buf), ldns_buffer_limit(buf));
}

#ifdef USE_WINSOCK
char* wsa_strerror(DWORD err)
{
	static char unknown[32];

	switch(err) {
	case WSA_INVALID_HANDLE: return "Specified event object handle is invalid.";
	case WSA_NOT_ENOUGH_MEMORY: return "Insufficient memory available.";
	case WSA_INVALID_PARAMETER: return "One or more parameters are invalid.";
	case WSA_OPERATION_ABORTED: return "Overlapped operation aborted.";
	case WSA_IO_INCOMPLETE: return "Overlapped I/O event object not in signaled state.";
	case WSA_IO_PENDING: return "Overlapped operations will complete later.";
	case WSAEINTR: return "Interrupted function call.";
	case WSAEBADF: return "File handle is not valid.";
 	case WSAEACCES: return "Permission denied.";
	case WSAEFAULT: return "Bad address.";
	case WSAEINVAL: return "Invalid argument.";
	case WSAEMFILE: return "Too many open files.";
	case WSAEWOULDBLOCK: return "Resource temporarily unavailable.";
	case WSAEINPROGRESS: return "Operation now in progress.";
	case WSAEALREADY: return "Operation already in progress.";
	case WSAENOTSOCK: return "Socket operation on nonsocket.";
	case WSAEDESTADDRREQ: return "Destination address required.";
	case WSAEMSGSIZE: return "Message too long.";
	case WSAEPROTOTYPE: return "Protocol wrong type for socket.";
	case WSAENOPROTOOPT: return "Bad protocol option.";
	case WSAEPROTONOSUPPORT: return "Protocol not supported.";
	case WSAESOCKTNOSUPPORT: return "Socket type not supported.";
	case WSAEOPNOTSUPP: return "Operation not supported.";
	case WSAEPFNOSUPPORT: return "Protocol family not supported.";
	case WSAEAFNOSUPPORT: return "Address family not supported by protocol family.";
	case WSAEADDRINUSE: return "Address already in use.";
	case WSAEADDRNOTAVAIL: return "Cannot assign requested address.";
	case WSAENETDOWN: return "Network is down.";
	case WSAENETUNREACH: return "Network is unreachable.";
	case WSAENETRESET: return "Network dropped connection on reset.";
	case WSAECONNABORTED: return "Software caused connection abort.";
	case WSAECONNRESET: return "Connection reset by peer.";
	case WSAENOBUFS: return "No buffer space available.";
	case WSAEISCONN: return "Socket is already connected.";
	case WSAENOTCONN: return "Socket is not connected.";
	case WSAESHUTDOWN: return "Cannot send after socket shutdown.";
	case WSAETOOMANYREFS: return "Too many references.";
	case WSAETIMEDOUT: return "Connection timed out.";
	case WSAECONNREFUSED: return "Connection refused.";
	case WSAELOOP: return "Cannot translate name.";
	case WSAENAMETOOLONG: return "Name too long.";
	case WSAEHOSTDOWN: return "Host is down.";
	case WSAEHOSTUNREACH: return "No route to host.";
	case WSAENOTEMPTY: return "Directory not empty.";
	case WSAEPROCLIM: return "Too many processes.";
	case WSAEUSERS: return "User quota exceeded.";
	case WSAEDQUOT: return "Disk quota exceeded.";
	case WSAESTALE: return "Stale file handle reference.";
	case WSAEREMOTE: return "Item is remote.";
	case WSASYSNOTREADY: return "Network subsystem is unavailable.";
	case WSAVERNOTSUPPORTED: return "Winsock.dll version out of range.";
	case WSANOTINITIALISED: return "Successful WSAStartup not yet performed.";
	case WSAEDISCON: return "Graceful shutdown in progress.";
	case WSAENOMORE: return "No more results.";
	case WSAECANCELLED: return "Call has been canceled.";
	case WSAEINVALIDPROCTABLE: return "Procedure call table is invalid.";
	case WSAEINVALIDPROVIDER: return "Service provider is invalid.";
	case WSAEPROVIDERFAILEDINIT: return "Service provider failed to initialize.";
	case WSASYSCALLFAILURE: return "System call failure.";
	case WSASERVICE_NOT_FOUND: return "Service not found.";
	case WSATYPE_NOT_FOUND: return "Class type not found.";
	case WSA_E_NO_MORE: return "No more results.";
	case WSA_E_CANCELLED: return "Call was canceled.";
	case WSAEREFUSED: return "Database query was refused.";
	case WSAHOST_NOT_FOUND: return "Host not found.";
	case WSATRY_AGAIN: return "Nonauthoritative host not found.";
	case WSANO_RECOVERY: return "This is a nonrecoverable error.";
	case WSANO_DATA: return "Valid name, no data record of requested type.";
	case WSA_QOS_RECEIVERS: return "QOS receivers.";
	case WSA_QOS_SENDERS: return "QOS senders.";
	case WSA_QOS_NO_SENDERS: return "No QOS senders.";
	case WSA_QOS_NO_RECEIVERS: return "QOS no receivers.";
	case WSA_QOS_REQUEST_CONFIRMED: return "QOS request confirmed.";
	case WSA_QOS_ADMISSION_FAILURE: return "QOS admission error.";
	case WSA_QOS_POLICY_FAILURE: return "QOS policy failure.";
	case WSA_QOS_BAD_STYLE: return "QOS bad style.";
	case WSA_QOS_BAD_OBJECT: return "QOS bad object.";
	case WSA_QOS_TRAFFIC_CTRL_ERROR: return "QOS traffic control error.";
	case WSA_QOS_GENERIC_ERROR: return "QOS generic error.";
	case WSA_QOS_ESERVICETYPE: return "QOS service type error.";
	case WSA_QOS_EFLOWSPEC: return "QOS flowspec error.";
	case WSA_QOS_EPROVSPECBUF: return "Invalid QOS provider buffer.";
	case WSA_QOS_EFILTERSTYLE: return "Invalid QOS filter style.";
	case WSA_QOS_EFILTERTYPE: return "Invalid QOS filter type.";
	case WSA_QOS_EFILTERCOUNT: return "Incorrect QOS filter count.";
	case WSA_QOS_EOBJLENGTH: return "Invalid QOS object length.";
	case WSA_QOS_EFLOWCOUNT: return "Incorrect QOS flow count.";
	/*case WSA_QOS_EUNKOWNPSOBJ: return "Unrecognized QOS object.";*/
	case WSA_QOS_EPOLICYOBJ: return "Invalid QOS policy object.";
	case WSA_QOS_EFLOWDESC: return "Invalid QOS flow descriptor.";
	case WSA_QOS_EPSFLOWSPEC: return "Invalid QOS provider-specific flowspec.";
	case WSA_QOS_EPSFILTERSPEC: return "Invalid QOS provider-specific filterspec.";
	case WSA_QOS_ESDMODEOBJ: return "Invalid QOS shape discard mode object.";
	case WSA_QOS_ESHAPERATEOBJ: return "Invalid QOS shaping rate object.";
	case WSA_QOS_RESERVED_PETYPE: return "Reserved policy QOS element type.";
	default:
		snprintf(unknown, sizeof(unknown),
			"unknown WSA error code %d", (int)err);
		return unknown;
	}
}
#endif /* USE_WINSOCK */
