/* NTP client
 *
 * Copyright (C) 1997, 1999, 2000, 2003, 2006, 2007, 2010  Larry Doolittle <larry@doolittle.boa.org>
 * Copyright (C) 2010-2016  Joachim Nilsson <troglobit@gmail.com>
 *
 *  This program is free software; you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License (Version 2,
 *  June 1991) as published by the Free Software Foundation.  At the
 *  time of writing, that license was published by the FSF with the URL
 *  http://www.gnu.org/copyleft/gpl.html, and is incorporated herein by
 *  reference.
 *
 *  This program is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *  GNU General Public License for more details.
 *
 * Possible future improvements:
 *    - Write more documentation  :-(
 *    - Support leap second processing
 *    - Support IPv6
 *    - Support multiple (interleaved) servers
 *
 * Compile with -DPRECISION_SIOCGSTAMP if your machine really has it.
 * There are patches floating around to add this to Linux, but
 * usually you only get an answer to the nearest jiffy.
 * Hint for Linux hacker wannabes: look at the usage of get_fast_time()
 * in net/core/dev.c, and its definition in kernel/time.c .
 *
 * If the compile gives you any flak, check below in the section
 * labelled "XXX fixme - non-automatic build configuration".
 */

#include <stdio.h>
#include <stdlib.h>
#include <signal.h>
#include <string.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <netdb.h>     /* gethostbyname */
#include <arpa/inet.h>
#include <time.h>
#include <unistd.h>
#include <errno.h>
#ifdef PRECISION_SIOCGSTAMP
#include <sys/ioctl.h>
#endif
#ifdef USE_OBSOLETE_GETTIMEOFDAY
#include <sys/time.h>
#endif

#include "ntpclient.h"

/* Default to the RFC-4330 specified value */
#ifndef MIN_INTERVAL
#define MIN_INTERVAL 15
#endif

#ifdef ENABLE_REPLAY
#define  REPLAY_OPTION   "r"
#else
#define  REPLAY_OPTION
#endif

int debug   = 0;
int verbose = 0;                /* Verbose flag, produce useful output to log */
int logging = 0;
const char *prognm = SYSLOG_IDENT;
static int sighup  = 0;
static int sigterm = 0;

extern char *optarg;		/* according to man 2 getopt */

#include <stdint.h>
typedef uint32_t u32;  /* universal for C99 */
/* typedef u_int32_t u32;   older Linux installs? */

/* XXX fixme - non-automatic build configuration */
#ifdef __linux__
#include <sys/utsname.h>
#include <sys/time.h>
#include <sys/timex.h>
#else
extern struct hostent *gethostbyname(const char *name);
extern int h_errno;
char __hstrerror_buf[10];
#define hstrerror(errnum) \
	snprintf(__hstrerror_buf, sizeof(__hstrerror_buf), "Error %d", errnum)
#endif
/* end configuration for host systems */

#define JAN_1970        0x83aa7e80      /* 2208988800 1970 - 1900 in seconds */
#define NTP_PORT (123)

/* How to multiply by 4294.967296 quickly (and not quite exactly)
 * without using floating point or greater than 32-bit integers.
 * If you want to fix the last 12 microseconds of error, add in
 * (2911*(x))>>28)
 */
#define NTPFRAC(x) ( 4294*(x) + ( (1981*(x))>>11 ) )

/* The reverse of the above, needed if we want to set our microsecond
 * clock (via clock_settime) based on the incoming time in NTP format.
 * Basically exact.
 */
#define USEC(x) ( ( (x) >> 12 ) - 759 * ( ( ( (x) >> 10 ) + 32768 ) >> 16 ) )

/* Converts NTP delay and dispersion, apparently in seconds scaled
 * by 65536, to microseconds.  RFC-1305 states this time is in seconds,
 * doesn't mention the scaling.
 * Should somehow be the same as 1000000 * x / 65536
 */
#define sec2u(x) ( (x) * 15.2587890625 )

struct ntptime {
	unsigned int coarse;
	unsigned int fine;
};

struct ntp_control {
	u32 time_of_send[2];
	int usermode;
	int live;
	int set_clock;		/* non-zero presumably needs root privs */
	int probe_count;
	int cycle_time;
	int goodness;
	int cross_check;

	short local_udp_port;
	char *server;		/* must be set */
	char serv_addr[4];
};

/* prototypes for some local routines */
static void send_packet(int usd, u32 time_sent[2]);
static int rfc1305print(u32 *data, struct ntptime *arrival, struct ntp_control *ntpc, int *error);
/* static void udp_handle(int usd, char *data, int data_len, struct sockaddr *sa_source, int sa_len); */

void logit(int severity, int syserr, const char *format, ...)
{
    va_list ap;
    static char buf[200];

    va_start(ap, format);
    vsnprintf(buf, sizeof(buf), format, ap);
    va_end(ap);

#ifdef ENABLE_SYSLOG
    if (logging) {
	    if (syserr) {
		    errno = syserr;
		    syslog(severity, "%s: %m", buf);
	    } else {
		    syslog(severity, "%s", buf);
	    }

	    return;
    }
#endif
    if (severity == LOG_WARNING)
	    fputs("Warning - ", stderr);
    else if (severity == LOG_ERR)
	    fputs("ERROR - ", stderr);

    if (syserr)
	    fprintf(stderr, "%s: %m\n", buf);
    else
	    fprintf(stderr, "%s\n", buf);
}

static int get_current_freq(void)
{
	/* OS dependent routine to get the current value of clock frequency.
	 */
#ifdef __linux__
	struct timex txc;
	txc.modes=0;
	if (adjtimex(&txc) < 0) {
		logit(LOG_ERR, errno, "Failed adjtimex(GET)");
		exit(1);
	}
	return txc.freq;
#else
	return 0;
#endif
}

static int set_freq(int new_freq)
{
	/* OS dependent routine to set a new value of clock frequency.
	 */
#ifdef __linux__
	struct timex txc;
	txc.modes = ADJ_FREQUENCY;
	txc.freq = new_freq;
	if (adjtimex(&txc) < 0) {
		logit(LOG_ERR, errno, "Failed adjtimex(SET)");
		exit(1);
	}
	return txc.freq;
#else
	return 0;
#endif
}

static void set_time(struct ntptime *new)
{
#ifndef USE_OBSOLETE_GETTIMEOFDAY
	/* POSIX 1003.1-2001 way to set the system clock
	 */
	struct timespec tv_set;
	/* it would be even better to subtract half the slop */
	tv_set.tv_sec  = new->coarse - JAN_1970;
	/* divide xmttime.fine by 4294.967296 */
	tv_set.tv_nsec = USEC(new->fine)*1000;
	if (clock_settime(CLOCK_REALTIME, &tv_set)<0) {
		logit(LOG_ERR, errno, "Failed clock_settime()");
		exit(1);
	}
	if (debug)
		logit(LOG_DEBUG, 0, "Set time to %lu.%.9lu", tv_set.tv_sec, tv_set.tv_nsec);
#else
	/* Traditional Linux way to set the system clock
	 */
	struct timeval tv_set;
	/* it would be even better to subtract half the slop */
	tv_set.tv_sec  = new->coarse - JAN_1970;
	/* divide xmttime.fine by 4294.967296 */
	tv_set.tv_usec = USEC(new->fine);
	if (settimeofday(&tv_set,NULL)<0) {
		logit(LOG_ERR, errno, "Failed settimeofday()");
		exit(1);
	}
	if (debug)
		logit(LOG_DEBUG, 0, "Set time to %lu.%.6lu", tv_set.tv_sec, tv_set.tv_usec);
#endif
}

static void ntpc_gettime(u32 *time_coarse, u32 *time_fine)
{
#ifndef USE_OBSOLETE_GETTIMEOFDAY
	/* POSIX 1003.1-2001 way to get the system time
	 */
	struct timespec now;
	clock_gettime(CLOCK_REALTIME, &now);
	*time_coarse = now.tv_sec + JAN_1970;
	*time_fine   = NTPFRAC(now.tv_nsec/1000);
#else
	/* Traditional Linux way to get the system time
	 */
	struct timeval now;
	gettimeofday(&now, NULL);
	*time_coarse = now.tv_sec + JAN_1970;
	*time_fine   = NTPFRAC(now.tv_usec);
#endif
}

static void send_packet(int usd, u32 time_sent[2])
{
	u32 data[12];
#define LI 0
#define VN 3
#define MODE 3
#define STRATUM 0
#define POLL 4
#define PREC -6

#ifdef ENABLE_DEBUG
	if (debug) logit(LOG_DEBUG, 0, "Sending packet...");
#endif
	if (sizeof(data) != 48) {
		logit(LOG_ERR, 0, "Packet size error");
		return;
	}
	memset(data, 0, sizeof data);
	data[0] = htonl (
		( LI << 30 ) | ( VN << 27 ) | ( MODE << 24 ) |
		( STRATUM << 16) | ( POLL << 8 ) | ( PREC & 0xff ) );
	data[1] = htonl(1<<16);  /* Root Delay (seconds) */
	data[2] = htonl(1<<16);  /* Root Dispersion (seconds) */
	ntpc_gettime(time_sent, time_sent+1);
	data[10] = htonl(time_sent[0]); /* Transmit Timestamp coarse */
	data[11] = htonl(time_sent[1]); /* Transmit Timestamp fine   */
	send(usd,data,48,0);
}

static void get_packet_timestamp(int usd, struct ntptime *udp_arrival_ntp)
{
#ifdef PRECISION_SIOCGSTAMP
	/* XXX broken */
	struct timeval udp_arrival;

	if ( ioctl(usd, SIOCGSTAMP, &udp_arrival) < 0 ) {
		logit(LOG_ERR, errno, "Failed ioctl(SIOCGSTAMP)");
		gettimeofday(&udp_arrival, NULL);
	}
	udp_arrival_ntp->coarse = udp_arrival.tv_sec + JAN_1970;
	udp_arrival_ntp->fine   = NTPFRAC(udp_arrival.tv_usec);
#else
	(void) usd;  /* not used */
	ntpc_gettime(&udp_arrival_ntp->coarse, &udp_arrival_ntp->fine);
#endif
}

static int check_source(int data_len, struct sockaddr *sa, unsigned int sa_len, struct ntp_control *ntpc)
{
	struct sockaddr_in *sin = (struct sockaddr_in *)sa;

	(void) data_len;	/* not used */
	(void) sa_len;		/* not used */

#ifdef ENABLE_DEBUG
	if (debug) {
		logit(LOG_DEBUG, 0, "Packet of length %d received", data_len);
		if (sa->sa_family == AF_INET) {
			logit(LOG_DEBUG, 0, "Source: INET Port %u host %s",
			      ntohs(sin->sin_port), inet_ntoa(sin->sin_addr));
		} else {
			logit(LOG_DEBUG, 0, "Source: Address family %d", sa->sa_family);
		}
	}
#endif

	/* we could check that the source is the server we expect, but
	 * Denys Vlasenko recommends against it: multihomed hosts get it
	 * wrong too often. */
#if 0
	if (memcmp(ntpc->serv_addr, &(sin->sin_addr), 4)!=0) {
		return 1;  /* fault */
	}
#else
	(void) ntpc; /* not used */
#endif
	if (NTP_PORT != ntohs(sin->sin_port)) {
		return 1;  /* fault */
	}

	return 0;
}

static double ntpdiff( struct ntptime *start, struct ntptime *stop)
{
	int a;
	unsigned int b;
	a = stop->coarse - start->coarse;
	if (stop->fine >= start->fine) {
		b = stop->fine - start->fine;
	} else {
		b = start->fine - stop->fine;
		b = ~b;
		a -= 1;
	}

	return a*1.e6 + b * (1.e6/4294967296.0);
}

/* Does more than print, so this name is bogus.
 * It also makes time adjustments, both sudden (-s)
 * and phase-locking (-l).
 * sets *error to the number of microseconds uncertainty in answer
 * returns 0 normally, 1 if the message fails sanity checks
 */
static int rfc1305print(u32 *data, struct ntptime *arrival, struct ntp_control *ntpc, int *error)
{
	/* straight out of RFC-1305 Appendix A */
	int li, vn, mode, stratum, prec;
	int delay, disp;
#ifdef ENABLE_DEBUG
	int poll, refid;
	struct ntptime reftime;
#endif
	struct ntptime orgtime, rectime, xmttime;
	double el_time,st_time,skew1,skew2;
	int freq;
	const char *drop_reason=NULL;

#define Data(i) ntohl(((u32 *)data)[i])
	li      = Data(0) >> 30 & 0x03;
	vn      = Data(0) >> 27 & 0x07;
	mode    = Data(0) >> 24 & 0x07;
	stratum = Data(0) >> 16 & 0xff;
#ifdef ENABLE_DEBUG
	poll    = Data(0) >>  8 & 0xff;
#endif
	prec    = Data(0)       & 0xff;
	if (prec & 0x80) prec|=0xffffff00;
	delay   = Data(1);
	disp    = Data(2);
#ifdef ENABLE_DEBUG
	refid   = Data(3);
	reftime.coarse = Data(4);
	reftime.fine   = Data(5);
#endif
	orgtime.coarse = Data(6);
	orgtime.fine   = Data(7);
	rectime.coarse = Data(8);
	rectime.fine   = Data(9);
	xmttime.coarse = Data(10);
	xmttime.fine   = Data(11);
#undef Data

#ifdef ENABLE_DEBUG
	if (debug) {
                logit(LOG_DEBUG, 0, "LI=%d  VN=%d  Mode=%d  Stratum=%d  Poll=%d  Precision=%d",
		      li, vn, mode, stratum, poll, prec);
		logit(LOG_DEBUG, 0, "Delay=%.1f  Dispersion=%.1f  Refid=%u.%u.%u.%u", sec2u(delay),sec2u(disp),
		      refid>>24&0xff, refid>>16&0xff, refid>>8&0xff, refid&0xff);
		logit(LOG_DEBUG, 0, "Reference %u.%.6u", reftime.coarse, USEC(reftime.fine));
		logit(LOG_DEBUG, 0, "(sent)    %u.%.6u", ntpc->time_of_send[0], USEC(ntpc->time_of_send[1]));
		logit(LOG_DEBUG, 0, "Originate %u.%.6u", orgtime.coarse, USEC(orgtime.fine));
		logit(LOG_DEBUG, 0, "Receive   %u.%.6u", rectime.coarse, USEC(rectime.fine));
		logit(LOG_DEBUG, 0, "Transmit  %u.%.6u", xmttime.coarse, USEC(xmttime.fine));
		logit(LOG_DEBUG, 0, "Our recv  %u.%.6u", arrival->coarse, USEC(arrival->fine));
	}
#endif

	el_time = ntpdiff(&orgtime,arrival);   /* elapsed */
	st_time = ntpdiff(&rectime,&xmttime);  /* stall */
	skew1   = ntpdiff(&orgtime,&rectime);
	skew2   = ntpdiff(&xmttime,arrival);
	freq    = get_current_freq();

#ifdef ENABLE_DEBUG
	if (debug) {
		logit(LOG_DEBUG, 0, "Total elapsed: %9.2f", el_time);
		logit(LOG_DEBUG, 0, "Server stall:  %9.2f", st_time);
		logit(LOG_DEBUG, 0, "Slop:          %9.2f", el_time - st_time);
		logit(LOG_DEBUG, 0, "Skew:          %9.2f", (skew1 - skew2) / 2);
		logit(LOG_DEBUG, 0, "Frequency:     %9d", freq);
		logit(LOG_DEBUG, 0, " Day   Second     Elapsed    Stall     Skew  Dispersion  Freq");
	}
#endif

	/* error checking, see RFC-4330 section 5 */
#define FAIL(x) do { drop_reason=(x); goto fail;} while (0)
	if (ntpc->cross_check) {
		if (li == 3)   FAIL("LI==3");  /* unsynchronized */
		if (vn < 3)    FAIL("VN<3");   /* RFC-4330 documents SNTP v4, but we interoperate with NTP v3 */
		if (mode != 4) FAIL("MODE!=3");
		if (orgtime.coarse != ntpc->time_of_send[0] ||
		    orgtime.fine   != ntpc->time_of_send[1] ) FAIL("ORG!=sent");
		if (xmttime.coarse == 0 && xmttime.fine == 0) FAIL("XMT==0");
		if (delay > 65536 || delay < -65536) FAIL("abs(DELAY)>65536");
		if (disp  > 65536 || disp  < -65536) FAIL("abs(DISP)>65536");
		if (stratum == 0) FAIL("STRATUM==0");  /* kiss o' death */
#undef FAIL
	}

	/* XXX should I do this if debug flag is set? */
	if (ntpc->set_clock) { /* you'd better be root, or ntpclient will exit here! */
		set_time(&xmttime);
		if (verbose) {
			logit(LOG_NOTICE, 0, "Time synchronized to server %s, stratum %d", ntpc->server, stratum);
		}
	}

	/* Not the ideal order for printing, but we want to be sure
	 * to do all the time-sensitive thinking (and time setting)
	 * before we start the output, especially fflush() (which
	 * could be slow).  Of course, if debug is turned on, speed
	 * has gone down the drain anyway. */
	if (ntpc->live) {
		int new_freq;

		new_freq = contemplate_data(arrival->coarse, (skew1 - skew2) / 2, el_time + sec2u(disp), freq);
		if (!debug && new_freq != freq)
			set_freq(new_freq);
	}

	/* Display by default for regular users, root users need to supply -d. */
	if (debug || ntpc->usermode) {
                logit(LOG_NOTICE, 0, "%d %.5d.%.3d  %8.1f %8.1f  %8.1f %8.1f %9d",
		      arrival->coarse/86400, arrival->coarse%86400,
		      arrival->fine/4294967, el_time, st_time,
		      (skew1-skew2)/2, sec2u(disp), freq);
        }
	*error = el_time-st_time;

	return 0;
fail:
	if (debug || verbose) {
		logit(LOG_ERR, 0, "%d %.5d.%.3d rejected packet: %s",
		      arrival->coarse/86400, arrival->coarse%86400,
		      arrival->fine/4294967, drop_reason);
        }

	return 1;
}

static void stuff_net_addr(struct in_addr *p, char *hostname)
{
	struct hostent *ntpserver;

	ntpserver = gethostbyname(hostname);
	if (ntpserver == NULL) {
                if (verbose) {
                        logit(LOG_ERR, 0, "Unable lookup %s: %s", hostname, hstrerror(h_errno));
		}
		exit(1);
	}

	if (ntpserver->h_length != 4) {
		/* IPv4 only, until I get a chance to test IPv6 */
		logit(LOG_NOTICE, 0, "Oops h_length=%d, only IPv4 supported currently.", ntpserver->h_length);
		exit(1);
	}
	memcpy(&(p->s_addr),ntpserver->h_addr_list[0],4);
}

static void setup_receive(int usd, unsigned int interface, short port)
{
	struct sockaddr_in sa_rcvr;
	memset(&sa_rcvr,0,sizeof sa_rcvr);
	sa_rcvr.sin_family=AF_INET;
	sa_rcvr.sin_addr.s_addr=htonl(interface);
	sa_rcvr.sin_port=htons(port);
	if (bind(usd,(struct sockaddr *) &sa_rcvr,sizeof sa_rcvr) == -1) {
		logit(LOG_ERR, errno, "Failed binding to UDP port %d", port);
		exit(1);
	}
	/* listen(usd,3); this isn't TCP; thanks Alexander! */
}

static void setup_transmit(int usd, char *host, short port, struct ntp_control *ntpc)
{
	struct sockaddr_in sa_dest;

	memset(&sa_dest,0,sizeof sa_dest);
	sa_dest.sin_family=AF_INET;
	stuff_net_addr(&(sa_dest.sin_addr),host);
	memcpy(ntpc->serv_addr,&(sa_dest.sin_addr),4); /* XXX asumes IPv4 */
	sa_dest.sin_port=htons(port);
	while (connect(usd,(struct sockaddr *)&sa_dest,sizeof sa_dest)==-1) {
                if (ntpc->live) {
                        /* Wait here a while, networking is probably not up yet. */
                        sleep(1);
                        continue;
                }

		logit(LOG_ERR, errno, "Failed connecting to NTP server");
                exit(1);
        }
}

static int setup_socket(struct ntp_control *ntpc)
{
	int sd;

	if ((sd = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP)) == -1)
		return -1;

	setup_receive(sd, INADDR_ANY, ntpc->local_udp_port);
	setup_transmit(sd, ntpc->server, NTP_PORT, ntpc);

	return sd;
}

/*
 * Signal handler.  Take note of the fact that the signal arrived
 * so that the main loop can take care of it.
 */
static void handler(int sig)
{
	switch (sig) {
		case SIGHUP:
			/* Trigger NTP sync */
			sighup = 1;
			break;

		case SIGINT:
		case SIGQUIT:
		case SIGTERM:
		case SIGUSR1:
		case SIGUSR2:
			sigterm = 1;
			break;
	}
}

static void setup_signals(void)
{
	struct sigaction sa;

	sa.sa_handler = handler;
	sa.sa_flags = 0;	/* Interrupt system calls */
	sigemptyset(&sa.sa_mask);
	sigaction(SIGHUP, &sa, NULL);
	sigaction(SIGINT, &sa, NULL);
	sigaction(SIGQUIT, &sa, NULL);
	sigaction(SIGTERM, &sa, NULL);
	sigaction(SIGUSR1, &sa, NULL);
	sigaction(SIGUSR2, &sa, NULL);
}

static void primary_loop(int usd, struct ntp_control *ntpc)
{
	fd_set fds;
	struct sockaddr sa_xmit;
	int i, pack_len, probes_sent, error;
	socklen_t sa_xmit_len;
	struct timeval to;
	struct ntptime udp_arrival_ntp;
	static u32 incoming_word[325];
#define incoming ((char *) incoming_word)
#define sizeof_incoming (sizeof incoming_word)

#ifdef ENABLE_DEBUG
	if (debug) logit(LOG_DEBUG, 0, "Listening...");
#endif
	probes_sent = 0;
	sa_xmit_len = sizeof(sa_xmit);
	to.tv_sec   = 0;
	to.tv_usec  = 0;

	while (1) {
		if (sigterm) {
			ntpc->live = 0;
			break;
		}
		if (sighup) {
			sighup     = 0;
			to.tv_sec  = 0;
			to.tv_usec = 0;
			close (usd);
			usd = setup_socket(ntpc);
			if (-1 == usd) {
				logit(LOG_ERR, errno, "Failed reopening NTP socket");
				return;
			}
			logit(LOG_DEBUG, 0, "Got SIGHUP, triggering resync with NTP server.");
		}

		FD_ZERO(&fds);
		FD_SET(usd, &fds);
		i = select(usd + 1, &fds, NULL, NULL, &to);  /* Wait on read or error */
		if ((i != 1) || (!FD_ISSET(usd, &fds))) {
			if (i < 0) {
				if (errno != EINTR)
					logit(LOG_ERR, errno, "Failed select()");
				continue;
			}
			if (to.tv_sec == 0) {
				if (probes_sent >= ntpc->probe_count && ntpc->probe_count != 0)
					break;

				send_packet(usd, ntpc->time_of_send);
				++probes_sent;
				to.tv_sec = ntpc->cycle_time;
				to.tv_usec = 0;
			}
			continue;
		}

		error = ntpc->goodness;
		pack_len = recvfrom(usd, incoming, sizeof_incoming, 0, &sa_xmit, &sa_xmit_len);
		if (pack_len < 0) {
			logit(LOG_ERR, errno, "Failed recvfrom()");
		} else if (pack_len > 0 && (unsigned)pack_len < sizeof_incoming) {
			get_packet_timestamp(usd, &udp_arrival_ntp);
			if (check_source(pack_len, &sa_xmit, sa_xmit_len, ntpc) != 0)
				continue;
			if (rfc1305print(incoming_word, &udp_arrival_ntp, ntpc, &error) != 0)
				continue;
			/* udp_handle(usd,incoming,pack_len,&sa_xmit,sa_xmit_len); */
		} else {
			logit(LOG_ERR, 0, "Ooops.  pack_len=%d", pack_len);
		}

		/* best rollover option: specify -g, -s, and -l.
		 * simpler rollover option: specify -s and -l, which
		 * triggers a magic -c 1 */
		if ((error < ntpc->goodness && ntpc->goodness != 0) ||
		    (probes_sent >= ntpc->probe_count && ntpc->probe_count != 0)) {
			ntpc->set_clock = 0;
			if (!ntpc->live)
				break;
		}
	}
#undef incoming
#undef sizeof_incoming
}

#ifdef ENABLE_REPLAY
static int do_replay(void)
{
	char line[100];
	int n, day, freq, absolute;
	float sec, el_time, st_time, disp;
	double skew, errorbar;
	int simulated_freq = 0;
	unsigned int last_fake_time = 0;
	double fake_delta_time = 0.0;

	while (fgets(line,sizeof line,stdin)) {
		n = sscanf(line,"%d %f %f %f %lf %f %d",
			   &day, &sec, &el_time, &st_time, &skew, &disp, &freq);
		if (n == 7) {
			logit(LOG_DEBUG, 0, "%s", line);
			absolute = day*86400 + (int)sec;
			errorbar = el_time + disp;
#ifdef ENABLE_DEBUG
			if (debug)
				logit(LOG_DEBUG, 0, "Contemplate %u %.1f %.1f %d", absolute, skew, errorbar, freq);
#endif
			if (last_fake_time == 0) simulated_freq = freq;
			fake_delta_time += (absolute - last_fake_time) * ((double)(freq - simulated_freq)) / 65536;
#ifdef ENABLE_DEBUG
			if (debug)
				logit(LOG_DEBUG, 0, "Fake %f %d", fake_delta_time, simulated_freq);
#endif
			skew += fake_delta_time;
			freq  = simulated_freq;
			last_fake_time = absolute;
			simulated_freq = contemplate_data(absolute, skew, errorbar, freq);
		} else {
			logit(LOG_ERR, 0, "Replay input error");
			return 2;
		}
	}

	return 0;
}
#endif

static int usage(void)
{
	fprintf(stderr,
		"Usage: %s [-dlnstv] [-c count] [-f frequency] [-g goodness] [-h hostname]\n"
		"                 [-i interval] [-p port] [-q min_delay]"
#ifdef ENABLE_REPLAY
		" [-r]"
#endif
#ifdef ENABLE_SYSLOG
		" [-L]"
#endif
		" hostname\n", prognm);

	fprintf(stderr,	"Options:\n"
		" -c count     Stop after count time measurements. Default: 0 (forever)\n"
		" -d           Debug, or diagnostics mode  Possible to enable more at compile\n"
		" -g goodness  Stop after getting a result more accurate than goodness msec,\n"
		"              microseconds. Default: 0 (forever)\n"
		" -h hostname  NTP server, mandatory(!), against which to sync system time\n"
		" -i interval  Check time every interval seconds.  Default: 600\n"
		" -l           Attempt to lock local clock to server using adjtimex(2)\n");
#ifdef ENABLE_SYSLOG
	fprintf(stderr, " -L           Use syslog instead of stdout for log messages, enabled\n"
		"              by default when started as root\n");
#endif
	fprintf(stderr, " -n           Don't fork.  Prevents %s from daemonizing by default\n"
		"              Only when running as root, does nothing for regular users\n"
		" -p port      NTP client UDP port.  Default: 0 (\"any available\")\n"
		" -q min_delay Minimum packet delay for transaction (default 800 microseconds)\n",
		prognm);
#ifdef ENABLE_REPLAY
	fprintf(stderr, " -r           Replay analysis code based on stdin\n");
#endif
	fprintf(stderr, " -s           Simple clock set, implies -c 1 unless -l is also set\n"
		" -t           Trust network and server, no RFC-4330 recommended validation\n"
		" -v           Be verbose.  This option will cause time sync events, hostname\n"
		"              lookup errors and program version to be displayed\n"
		" -V           Display version and copyright information\n");
	fprintf(stderr, "\nReport %s bugs to troglobit@vmlinux.org\n", prognm);
	fprintf(stderr, "Home page: http://troglobit.com/ntpclient.shtml\n");

	return 1;
}

static int version(void)
{
	fprintf(stderr, "Larry Doolittle's ntpclient v" VERSION_STRING "\n\n");
	fprintf(stderr, "Copyright (C) 1997, 1999, 2000, 2003, 2006, 2007  Larry Doolittle <larry@doolittle.boa.org>\n"
		"Copyright (C) 2010  Joachim Nilsson <troglobit@vmlinux.org>\n\n");
	fprintf(stderr, "License GPLv2: GNU GPL version 2 <http://gnu.org/licenses/gpl2.html>\n"
		"This is free software: you are free to change and redistribute it.\n"
		"There is NO WARRANTY, to the extent permitted by law.\n");

	return 1;
}

static const char *progname(const char *arg0)
{
	const char *nm;

	nm = strrchr(arg0, '/');
	if (nm)
		nm++;
	else
		nm = arg0;

	return nm;
}

int main(int argc, char *argv[])
{
	int c, usd;  /* socket */
	/* These parameters are settable from the command line
	   the initializations here provide default behavior */
	int daemonize = 0;
	int initial_freq = 0;	/* initial freq value to use */
	struct ntp_control ntpc;

	/* Setup application defaults depending on root/user mode */
	memset(&ntpc, 0, sizeof(ntpc));
	ntpc.probe_count = 0;	/* default of 0 means loop forever */
	ntpc.cycle_time  = 600;	/* seconds */
	ntpc.goodness    = 0;
	ntpc.set_clock   = 0;
	if (geteuid() == 0) {
		daemonize        = 1;
		ntpc.usermode    = 0;
		ntpc.live        = 1;
		ntpc.cross_check = 0;
	} else {
		ntpc.usermode    = 1;
		ntpc.live        = 0;
		ntpc.cross_check = 1;
	}

	prognm = progname(argv[0]);
	while (1) {
		char opts[] = "c:df:g:h:i:lnp:q:" REPLAY_OPTION "st" LOG_OPTION "vV?";

		c = getopt(argc, argv, opts);
		if (c == EOF)
			break;

		switch (c) {
		case 'c':
			ntpc.probe_count = atoi(optarg);
			break;

		case 'd':
			debug++;
			break;

		case 'f':
			initial_freq = atoi(optarg);
			break;

		case 'g':
			ntpc.goodness = atoi(optarg);
			break;

		case 'h':
			ntpc.server = optarg;
			break;

		case 'i':
			ntpc.cycle_time = atoi(optarg);
			break;

		case 'l':
			ntpc.live++;
			break;

		case 'n':
			daemonize = 0;
			break;

		case 'p':
			ntpc.local_udp_port = atoi(optarg);
			break;

		case 'q':
			min_delay = atof(optarg);
			break;

#ifdef ENABLE_REPLAY
		case 'r':
			return do_replay();
#endif

		case 's':
			ntpc.set_clock++;
			break;

		case 't':
			ntpc.cross_check = 0;
			break;

#ifdef ENABLE_SYSLOG
		case 'L':
			logging++;
			break ;
#endif

		case 'v':
			++verbose;
			break;

		case 'V':
			return version();

		case '?':
			default:
				return usage();
		}
	}

#ifdef ENABLE_SYSLOG
	openlog(prognm, SYSLOG_OPTIONS, SYSLOG_FACILITY);
#endif

	if (initial_freq) {
#ifdef ENABLE_DEBUG
		logit(LOG_DEBUG, 0, "Initial frequency %d", initial_freq);
#endif
		set_freq(initial_freq);
	}

	if (optind < argc && ntpc.server == NULL)
		ntpc.server = argv[optind];

	if (ntpc.server == NULL) {
		fprintf(stderr, "Missing NTP server argument.\n");
		return usage();
	}

	if (ntpc.set_clock && !ntpc.live && !ntpc.goodness && !ntpc.probe_count) {
		ntpc.probe_count = 1;
	}

	/* If suer gives a probe count, then assume non-live run and verbose reporting. */
	if (ntpc.probe_count > 0) {
		ntpc.live = 0;
		verbose = 1;
	}

	/* respect only applicable MUST of RFC-4330 */
	if (ntpc.probe_count != 1 && ntpc.cycle_time < MIN_INTERVAL) {
		ntpc.cycle_time = MIN_INTERVAL;
	}

#ifdef ENABLE_DEBUG
	if (debug) {
		logit(LOG_DEBUG, 0, "Configuration:");
		logit(LOG_DEBUG, 0, "  -c probe_count %d", ntpc.probe_count);
		logit(LOG_DEBUG, 0, "  -d (debug)     %d", debug);
		logit(LOG_DEBUG, 0, "  -g goodness    %d", ntpc.goodness);
		logit(LOG_DEBUG, 0, "  -h hostname    %s", server);
		logit(LOG_DEBUG, 0, "  -i interval    %d", ntpc.cycle_time);
		logit(LOG_DEBUG, 0, "  -l live        %d", ntpc.live);
		logit(LOG_DEBUG, 0, "  -p local_port  %d", ntpc.local_udp_port);
		logit(LOG_DEBUG, 0, "  -q min_delay   %f", min_delay);
		logit(LOG_DEBUG, 0, "  -s set_clock   %d", ntpc.set_clock);
		logit(LOG_DEBUG, 0, "  -t cross_check %d", ntpc.cross_check);
#ifdef ENABLE_SYSLOG
		logit(LOG_DEBUG, 0, "  -L logging     %d", logging);
#endif
	}
#endif /* ENABLE_DEBUG */

	/* Startup sequence */
	if (daemonize) {
		if (-1 == daemon(0, 0)) {
			logit(LOG_ERR, errno, "Failed daemonizing, aborting");
			exit(1);
		}

		/* Force output to syslog, since we no longer have any way
		 * of communicating with the user after being daemonized. */
		logging = 1;
		if (verbose) {
			logit(LOG_NOTICE, 0, "Starting ntpclient v" VERSION_STRING);
		}
	}

	usd = setup_socket(&ntpc);
	if (usd == -1) {
		logit(LOG_ERR, errno, "Failed creating UDP socket() to SNTP server");
		exit(1);
	}

	setup_signals();

	if (daemonize && verbose) {
		logit(LOG_NOTICE, 0, "Using time sync server: %s", ntpc.server);
	}
	primary_loop(usd, &ntpc);

	if (daemonize && verbose) {
		logit(LOG_NOTICE, 0, "Stopping ntpclient v" VERSION_STRING);
	}
	close(usd);
#ifdef ENABLE_SYSLOG
	closelog();
#endif
	return 0;
}

/**
 * Local Variables:
 *  compile-command: "make ntpclient"
 *  indent-tabs-mode: t
 *  c-file-style: "linux"
 * End:
 */
