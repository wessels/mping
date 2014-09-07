/*
 * $Id: mping.c,v 1.13 2010/10/08 21:57:00 wessels Exp $
 * 
 * Copyright Duane Wessels
 * 
 * To compile:
 * 
 * % cc mping.c -o mping -lcurses
 */

#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <errno.h>
extern int errno;
#include <signal.h>
#include <string.h>
char *strchr();
char *strrchr();
#include <math.h>
#include <curses.h>
#include <memory.h>
#if defined(__sgi__)
#include <bstring.h>
#endif
#if !defined(__sgi__) && !defined(__sun__) && !defined(__linux__)
#include <machine/endian.h>
#endif
#include <assert.h>
#include <time.h>

#include <sys/types.h>
#include <sys/time.h>
#include <sys/socket.h>
#include <sys/ioctl.h>
#include <netinet/in_systm.h>
#include <netinet/in.h>
#include <netinet/ip.h>
#include <netinet/ip_icmp.h>
#include <netdb.h>
#include <arpa/inet.h>

#define MAX_PKT_SZ	4096
#define DEF_PKT_SZ	64
#define HIST_SIZE	200

#define SIZE_ICMP_HDR	8
#define SIZE_TIME_DATA	8
#define DEF_DATALEN	56
#define DEF_NPKTS	10

#ifndef INADDR_NONE
#define INADDR_NONE	0xffffffff
#endif

static unsigned char send_pkt[MAX_PKT_SZ];
static unsigned char recv_pkt[MAX_PKT_SZ];


static struct sockaddr_in from_addr;
static int icmp_sock;
static int datalen;
static int ident;
static int icmp_pktsize;
static int delay;		/* milliseconds */
static struct protoent *proto;
static char *progname;
static struct timeval now_tv;

typedef struct _site {
    char to_host[256];
    struct sockaddr_in to_addr;
    int rtt_hist[HIST_SIZE];
    int ttl_hist[HIST_SIZE];
    int high_seq;
    int npkts_sent;
    int nrecv;			/* total # pings sent */
    int ttl;
    double rtt;
    double avrtt;
    struct timeval last_sent;
}     site_t;

#define MAX_N_SITES 100

static site_t Sites[MAX_N_SITES];
static int NSites;

int main(int argc, char **argv);
void create_windows(void);
int in_cksum(unsigned short *ptr, int size);
void main_loop(void);
int recv_ping(void);
void send_next_ping(void);
time_t send_ping(site_t *);
int tvsub(struct timeval *t1, struct timeval *t2);

static WINDOW *w;

static void
do_stats(void)
{
    int i;
    int j;
    int nloss = 0;
    int nsent = 0;
    int pct_loss = 0;
    char buf[128];
    int rtt;
    int min = 5000, max = 0, avg = 0, sum = 0, count = 0;
    time_t now = time(NULL);
    int xtracols;
    site_t *S = NULL;
    int ttl_min;
    int ttl_max;
    int ttl;

    move(0, 0);

    for (i = 0; i < NSites; i++) {
	S = &Sites[i];
	nloss = 0;
	pct_loss = 0;
	nsent = 0;
	min = 5000;
	max = 0;
	avg = 0;
	sum = 0;
	count = 0;
	ttl_min = 255;
	ttl_max = 0;
	for (j = 0; j < HIST_SIZE; j++) {
	    rtt = S->rtt_hist[j];
	    if (rtt < 0)
		continue;
	    nsent++;
	    if (rtt == 0) {
		nloss++;
		continue;
	    }
	    if (rtt < min)
		min = rtt;
	    if (rtt > max)
		max = rtt;
	    sum += rtt;
	    ttl = S->ttl_hist[j];
	    if (0 < ttl && ttl < ttl_min)
		ttl_min = ttl;
	    if (ttl > ttl_max)
		ttl_max = ttl;
	    count++;
	}
	if (nsent)
	    pct_loss = 100 * nloss / nsent;
	if (count)
	    avg = sum / count;
	sprintf(buf, "%-23.23s %d/%d/%d, %dms, %d%% loss",
	    S->to_host,
	    min, avg, max,
	    (int)(S->avrtt + 0.5),
	/* ttl_min, S->ttl, ttl_max, */
	    pct_loss);
	printw("%-57.57s", buf);
	xtracols = w->_maxx - 58;
	assert(xtracols < HIST_SIZE);
	for (j = (HIST_SIZE - xtracols); j < HIST_SIZE; j++) {
	    rtt = S->rtt_hist[j];
	    if (rtt < 0)
		addch(' ');
	    else if (rtt == 0)
		addch('.');
	    else
		addch('!');
	}
	addch('\n');
    }

    move(w->_maxy - 1, 0);
    addstr(ctime(&now));
    refresh();
}

site_t *
find_a_site_to_ping(void)
{
    site_t *S = NULL;
    int i;
    site_t *best = NULL;
    time_t max_t = 0;
    time_t dt = 0;
    for (i = 0; i < NSites; i++) {
	S = &Sites[i];
	dt = tvsub(&S->last_sent, &now_tv);
	if (dt < delay * 1000)
	    continue;
	if (max_t > dt)
	    continue;
	max_t = dt;
	best = S;
    }
    return best;
}

void
main_loop(void)
{
    fd_set R;
    struct timeval to;
    int maxfd;
    int x;
    site_t *S = NULL;
    time_t wait = 0;

    while (1) {
	FD_ZERO(&R);
	FD_SET(icmp_sock, &R);
	to.tv_sec = 0;
	to.tv_usec = wait > 200000 ? 200000 : wait < 1 ? 30000 : wait;
	wait = 0;
	maxfd = icmp_sock + 1;
	x = select(maxfd, &R, 0, 0, &to);
	if (x > 0) {
	    if (FD_ISSET(icmp_sock, &R)) {
		if (recv_ping())
		    do_stats();
	    }
	} else if (x == 0) {
	    gettimeofday(&now_tv, NULL);
	    if ((S = find_a_site_to_ping()))
		wait = send_ping(S) * 2000;
	    do_stats();
	}
    }
}

void
create_windows(void)
{
    w = initscr();
    do_stats();
}

int
in_cksum(unsigned short *ptr, int size)
{

    register long sum;
    unsigned short oddbyte;
    register unsigned short answer;

    sum = 0;
    while (size > 1) {
	sum += *ptr++;
	size -= 2;
    }

    if (size == 1) {
	oddbyte = 0;
	*((unsigned char *)&oddbyte) = *(unsigned char *)ptr;
	sum += oddbyte;
    }
    sum = (sum >> 16) + (sum & 0xffff);
    sum += (sum >> 16);
    answer = ~sum;
    return (answer);
}


int
main(int argc, char *argv[])
{
    char myhostname[256];
    struct hostent *h;
    int n;
    extern char *optarg;
    extern int optind;
    int c;

    progname = argv[0];
    myhostname[0] = '\0';

    delay = 3000;

    /* process options */

    while ((c = getopt(argc, argv, "s:d:")) != -1) {
	switch (c) {
	case 's':
	    strncpy(myhostname, optarg, 63);
	    break;
	case 'd':
	    delay = atoi(optarg);
	    break;
	}
    }
    argc -= optind;
    argv += optind;

    if (argc < 1) {
	fprintf(stderr, "usage: %s [-s source hostname] [-d delay] hosts...\n",
	    progname);
	exit(1);
    }
    NSites = 0;
    while (argc) {
	if (NSites == 100)
	    break;
	memset(&Sites[NSites], '\0', sizeof(site_t));
	for (n = 0; n < HIST_SIZE; n++)
	    Sites[NSites].rtt_hist[n] = -1;
	strcpy(Sites[NSites].to_host, argv[0]);

	Sites[NSites].to_addr.sin_family = AF_INET;
	if ((h = gethostbyname(Sites[NSites].to_host))) {
	    memcpy(&(Sites[NSites].to_addr.sin_addr.s_addr),
		*(h->h_addr_list),
		4);
	} else {
	    Sites[NSites].to_addr.sin_addr.s_addr = inet_addr(Sites[NSites].to_host);
	}
	gettimeofday(&(Sites[NSites].last_sent), 0);
	Sites[NSites].last_sent.tv_sec -= (delay / 1000);
	NSites++;
	argv++;
	argc--;
    }

    if (myhostname[0] == '\0')
	if (gethostname(myhostname, 63) < 0) {
	    perror("gethostname");
	    exit(1);
	}
    memset((char *)&from_addr, '\0', sizeof(from_addr));
    from_addr.sin_family = AF_INET;
    from_addr.sin_addr.s_addr = INADDR_ANY;
    datalen = DEF_DATALEN;
    icmp_pktsize = datalen + SIZE_ICMP_HDR;

    ident = getpid() & 0xffff;

    if ((proto = getprotobyname("icmp")) == 0) {
	fprintf(stderr, "unknown protocol: icmp\n");
	exit(1);
    }
    if ((icmp_sock = socket(AF_INET, SOCK_RAW, proto->p_proto)) < 0) {
	perror("icmp socket");
	exit(1);
    }
    create_windows();
    main_loop();

    exit(0);
}

#define MAX_N_AVG	12

#ifndef ICMP_MINLEN
#define ICMP_MINLEN  8
#endif

site_t *
find_site(from)
    struct sockaddr_in from;
{
    int i;
    for (i = 0; i < NSites; i++) {
	if (from.sin_addr.s_addr == Sites[i].to_addr.sin_addr.s_addr)
	    return &Sites[i];
    }
    return NULL;
}

int
recv_ping(void)
{
    static int n;
    static int fromlen;
    static struct sockaddr_in from;

    int iphdrlen;
    int j;
    int offset;
    int k;
    struct ip *ip = NULL;
    register struct icmp *icp = NULL;
    char *source = NULL;
    struct timeval tv;
    struct timeval *tv1 = NULL;
    site_t *S = NULL;
    int N;

    fromlen = sizeof(from);
    n = recvfrom(icmp_sock, recv_pkt, sizeof(recv_pkt), 0,
	(struct sockaddr *)&from, &fromlen);
    gettimeofday(&tv, 0);
    source = inet_ntoa(from.sin_addr);

    ip = (struct ip *)recv_pkt;
#if defined(__osf__) && __STDC__==1
#if BYTE_ORDER == BIG_ENDIAN
    iphdrlen = (ip->ip_vhl >> 4) << 2;
#endif
#if BYTE_ORDER == LITTLE_ENDIAN
    iphdrlen = (ip->ip_vhl & 0xF) << 2;
#endif
#else
    iphdrlen = ip->ip_hl << 2;
#endif

    if (n < iphdrlen + ICMP_MINLEN) {
	fprintf(stderr, "packet too short (%d bytes) from %s\n",
	    n, source);
	return 0;
    }
    n -= iphdrlen;

    icp = (struct icmp *)(recv_pkt + iphdrlen);

    if (icp->icmp_type != ICMP_ECHOREPLY)
	return 0;

    if (icp->icmp_id != ident) {
	return 0;
    }
    if ((S = find_site(from)) == NULL)
	return 0;

#ifndef icmp_data
    tv1 = (struct timeval *)(icp + 1);
#else
    tv1 = (struct timeval *)&icp->icmp_data[0];
#endif
    S->rtt = (double)tvsub(tv1, &tv) / 1000;
    if (S->rtt < 1.0)
	S->rtt = 1.0;
    S->ttl = ip->ip_ttl;

    N = ++S->nrecv;
    if (N >= MAX_N_AVG)
	N = MAX_N_AVG;
    S->avrtt = ((S->avrtt * (N - 1)) + S->rtt) / N;

    offset = ntohs(icp->icmp_seq) > S->high_seq;
    assert(offset >= 0);
    if (offset > 0) {
	for (j = 0; j < HIST_SIZE; j++) {
	    k = j + offset;
	    S->rtt_hist[j] = k < HIST_SIZE ? S->rtt_hist[k] : 0;
	}
	S->high_seq = ntohs(icp->icmp_seq);
    }
    S->rtt_hist[HIST_SIZE - 1] = (int)(S->rtt + 0.5);

    if (offset > 0) {
	for (j = 0; j < HIST_SIZE; j++) {
	    k = j + offset;
	    S->ttl_hist[j] = k < HIST_SIZE ? S->ttl_hist[k] : 0;
	}
	S->high_seq = ntohs(icp->icmp_seq);
    }
    S->ttl_hist[HIST_SIZE - 1] = S->ttl;

    return 1;
}


time_t
send_ping(site_t * S)
{
    register int i;
    register struct icmp *icp;
    struct timeval *tv;
    int offset, j, k;

    memset(send_pkt, '\0', sizeof(send_pkt));

    icp = (struct icmp *)send_pkt;
    icp->icmp_type = ICMP_ECHO;
    icp->icmp_code = 0;
    icp->icmp_cksum = 0;
    icp->icmp_id = ident;
    icp->icmp_seq = htons(S->npkts_sent++);
    tv = (struct timeval *)(send_pkt + SIZE_ICMP_HDR);
    gettimeofday(tv, 0);
    S->last_sent = *tv;
    icp->icmp_cksum = in_cksum((unsigned short *)icp, icmp_pktsize);

    i = sendto(icmp_sock, send_pkt, icmp_pktsize, 0,
	(struct sockaddr *)&(S->to_addr), sizeof(struct sockaddr_in));

    offset = (S->npkts_sent - 1) - S->high_seq;
    assert(offset >= 0);
    if (offset > 0) {
	for (j = 0; j < HIST_SIZE; j++) {
	    k = j + offset;
	    S->rtt_hist[j] = k < HIST_SIZE ? S->rtt_hist[k] : 0;
	}
	S->high_seq = S->npkts_sent - 1;
    }
    return (time_t) (S->avrtt + 0.5);
}


int
tvsub(struct timeval *t1, struct timeval *t2)
{
    return (t2->tv_sec - t1->tv_sec) * 1000000 + (t2->tv_usec - t1->tv_usec);
}
