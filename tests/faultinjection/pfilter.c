/* -*- mode: c; c-basic-offset: 8; indent-tabs-mode: nil; -*-
 *
 * Copyright (c) 2007, XenSource Inc.
 * All rights reserved.
 */
#include <linux/netfilter.h>
#include <libipq/libipq.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>
#include <signal.h>
#include <unistd.h>
#include <sys/time.h>
#include <sys/stat.h>
#include <time.h>
#include <sys/resource.h>

#define LOGFILE "/var/log/pfilter.log"
#define DEBUG_ON 0
#define BUFSIZE 65535
#define INITIAL_PERCENT_DROP 0
#define INITIAL_PACKET_DROP  0

#define PRIO_SPECIAL_IO -9999

#define RUNDIR "/var/run/pfilter"
#define PIDFILE RUNDIR "/pid"

/*
 * modes:
 *     drop percentage mode
 *     drop individual packet mode
 * actions:
 *     increment droppage
 *     decrement droppage
 *     allow all
 *     disable all
 */

typedef enum {
    DROP_NONE,
    DROP_PERCENT,
    DROP_PACKET,
} mode_t;

typedef enum {
    ACTION_NA,
    ACTION_INCREMENT,
    ACTION_DECREMENT,
    ACTION_ALLOW_ALL,
    ACTION_DISALLOW_ALL,
    ACTION_RESET_PACKET_COUNT,
} action_t;

char *mode_string[4] = {
    "NONE", 
    "DROP_PERCENT", 
    "DROP_PACKET", 
    "huh?"
    };

char *action_string[7] = {
    "ACTION_NA",
    "ACTION_INCREMENT",
    "ACTION_DECREMENT",
    "ACTION_ALLOW_ALL",
    "ACTION_DISALLOW_ALL",
    "ACTION_RESET_PACKET_COUNT",
    "huh?"
    };

mode_t mode = DROP_NONE;
action_t last_action = ACTION_ALLOW_ALL;

unsigned long long packet_count = 0;
unsigned int curr_packet_count = 0;

int drop_percent = INITIAL_PERCENT_DROP;
int drop_packet  = INITIAL_PACKET_DROP;

#define TRACE(format, args...)                                       \
    do {                                                             \
        struct timeval tim;                                          \
        gettimeofday(&tim, 0);                                       \
        fprintf(stderr,"%ld.%06ld: ", tim.tv_sec, tim.tv_usec);      \
        fprintf(stderr, format, ## args);                            \
        fflush(stderr);                                              \
    } while (0)
#if DEBUG_ON
#define DEBUG(format, args...)                                       \
        if (logfile == 0) {                                          \
            printf(format, ## args);                                 \
        } else {                                                     \
            fprintf(logfile, format, ## args);                       \
            fflush(logfile);                                         \
        }
#else
#define DEBUG(format, args...)
#endif

static void trace_data(int verdict)
{
        if (last_action == ACTION_ALLOW_ALL ||
            last_action == ACTION_DISALLOW_ALL) {
                /* ignore tracing if allowing or disallowing all */
                return;
        }

        if (mode == DROP_PERCENT) {
                TRACE("data packet %llu %s\n", packet_count, 
                      verdict == NF_DROP ?  "DROPPED" : "ACCEPTED");
        } else if (mode == DROP_PACKET) {
                TRACE("data packet: %llu curr run: %d %s\n", packet_count, 
                      curr_packet_count,
                      verdict == NF_DROP ? "DROPPED" : "ACCEPTED");
        }
}

static void die(struct ipq_handle *h)
{
        ipq_perror("passer");
        ipq_destroy_handle(h);
        exit(1);
}

static void trace_adjustment(void)
{
        if (last_action == ACTION_ALLOW_ALL) {
                TRACE("change: Allow All packets\n");
        } else if (last_action == ACTION_DISALLOW_ALL) {
                TRACE("change: Disallow All packets\n");
        } else if (mode == DROP_PERCENT) {
                TRACE("change DROP_PERCENT: rate=%d\n", drop_percent);
        } else if (mode == DROP_PACKET) {
                TRACE("change DROP_PACKET: count=%d curr=%d\n", 
                      drop_packet, curr_packet_count);
        } else {
                TRACE("huh?\n");
        }
}

static void check_bounds(void)
{
    if (drop_percent < 0) drop_percent = 0;
    if (drop_percent > 100) drop_percent = 100;
    if (drop_packet < 0) drop_packet = 0;
    DEBUG("mode=%d, drop_percent=%d, drop_packet=%d\n", 
           mode, drop_percent, drop_packet);
}

static void adjust_action(action_t new_action)
{
    int skip = 0;

    DEBUG("adjust action=%s, mode=%s\n", 
           action_string[new_action], mode_string[mode]);
    switch (new_action) {
        case ACTION_INCREMENT:
            if (mode == DROP_PERCENT)     drop_percent++;
            else if (mode == DROP_PACKET) drop_packet++;
            else skip = 1;
            break;
        case ACTION_DECREMENT:
            if (mode == DROP_PERCENT)     drop_percent--;
            else if (mode == DROP_PACKET) drop_packet--;
            else skip = 1;
            break;
        case ACTION_ALLOW_ALL:
        case ACTION_DISALLOW_ALL:
            break;
            break;
        case ACTION_RESET_PACKET_COUNT:
            if (mode == DROP_PACKET) curr_packet_count = 0;
            else skip = 1;
            break;
        default:
            assert(0);
    }

    if (!skip) {
            check_bounds();
            last_action = new_action;
            trace_adjustment();
    }
}

static void adjust_mode(mode_t new_mode)
{
    DEBUG("change to mode=%s\n", mode_string[mode]);
    mode = new_mode;
    switch (mode) {
        case DROP_PERCENT:
            drop_percent = INITIAL_PERCENT_DROP;
            break;
        case DROP_PACKET:
            drop_packet  = INITIAL_PACKET_DROP;
            break;
        default:
            assert(0);
    }
    last_action = ACTION_NA;
    trace_adjustment();
}

static void increment_droppage(int sig)
{
        /* reassert signal */
        signal (SIGUSR1, increment_droppage);
        DEBUG("SIGUSR1\n")
        adjust_action(ACTION_INCREMENT);
}

static void decrement_droppage(int sig)
{
        /* reassert signal */
        signal (SIGUSR2, decrement_droppage);
        DEBUG("SIGUSR2\n")
        adjust_action(ACTION_DECREMENT);
}

static void allow_all_packets(int sig)
{
        /* reassert signal */
        signal (SIGCHLD, allow_all_packets);
        DEBUG("SIGCHLD\n")
        adjust_action(ACTION_ALLOW_ALL);
}

static void disable_all_packets(int sig)
{
        /* reassert signal */
        signal (SIGALRM, disable_all_packets);
        DEBUG("SIGALRM\n")
        adjust_action(ACTION_DISALLOW_ALL);
}

static void reset_packet_count(int sig)
{
        /* reassert signal */
        signal (SIGILL, reset_packet_count);
        DEBUG("SIGILL\n")
        adjust_action(ACTION_RESET_PACKET_COUNT);
}

static void percent_droppage_mode(int sig)
{
        /* reassert signal */
        signal (SIGFPE, percent_droppage_mode);
        DEBUG("SIGFPE\n")
        adjust_mode(DROP_PERCENT);
}

static void packet_droppage_mode(int sig)
{
        /* reassert signal */
        signal (SIGURG, packet_droppage_mode);
        DEBUG("SIGURG\n")
        adjust_mode(DROP_PACKET);
}

static void dump_settings(int sig)
{
        /* reassert signal */
        signal (SIGPROF, dump_settings);
        DEBUG("SIGPROF\n")

        if (last_action == ACTION_ALLOW_ALL) {
                TRACE("Allowing All packets\n");
        } else if (last_action == ACTION_DISALLOW_ALL) {
                TRACE("Disallowing All packets\n");
        } else if (mode == DROP_PERCENT) {
                TRACE("mode: percentage drop rate=%d\n", drop_percent);
        } else if (mode == DROP_PACKET) {
                TRACE("mode: packet drop count=%d curr=%d\n", 
                      drop_packet, curr_packet_count);
        } else {
                TRACE("unsure?\n");
        }
}

static pid_t writepid(void)
{
        FILE *pidfile;
        pid_t pid;

        pidfile = fopen(PIDFILE, "w");
        if (!pidfile) {
                perror(PIDFILE);
                return -1;
        }

        pid = getpid();
        fprintf(pidfile, "%d\n", pid);
        fclose(pidfile);

        return pid;
}

static pid_t running(void)
{
        FILE *pidfile;
        pid_t pid;
        int count, err;

        pidfile = fopen(PIDFILE, "r");
        if (!pidfile)
                return -1;

        count = fscanf(pidfile, "%d", &pid);
        if (!count)
                return -1;

        err = kill(pid, 0);
        if (err < 0 && errno != EEXIST)
                return -1;

        return pid;
}

static int make_ipc(void)
{
        int err;
        
        err = mkdir(RUNDIR, S_IRWXU|S_IRWXG);
        if (err < 0 && errno != EEXIST) {
                perror(RUNDIR);
                return -1;
        }

        /*
         * check and write pidfile
         */
        if (err < 0 && errno == EEXIST ) {
                pid_t pid = running();
                if (pid >= 0) {
                        fprintf(stderr, "Already running: PID %u\n", pid);
                        return -1;
                }
        }

        err = writepid();
        if (err < 0)
                return -1;

        /*
         * signal handlers
         */
        signal(SIGUSR1, increment_droppage);
        signal(SIGUSR2, decrement_droppage);
        signal(SIGCHLD, allow_all_packets);
        signal(SIGALRM, disable_all_packets);
        signal(SIGILL,  reset_packet_count);
        signal(SIGFPE,  percent_droppage_mode);
        signal(SIGURG,  packet_droppage_mode);
        signal(SIGPROF, dump_settings);

        return 0;
}

static void printhelp(void)
{
        FILE *stream = stdout;
        fprintf(stream, "SIGUSR1=increment droppage\n");
        fprintf(stream, "SIGUSR2=decrement droppage\n");
        fprintf(stream, "SIGCHLD=allow all packets\n");
        fprintf(stream, "SIGALRM=disallow all packets\n");
        fprintf(stream, "SIGILL =reset packet count\n");
        fprintf(stream, "SIGFPE =set drop to percent mode\n");
        fprintf(stream, "SIGURG =set drop to packet mode\n");
        fprintf(stream, "SIGPROF=dump settings\n");
}

static struct ipq_handle * make_ipq(void)
{
        int status;
        struct ipq_handle *h;

        h = ipq_create_handle(0, PF_INET);
        if (!h)
                die(h);

        status = ipq_set_mode(h, IPQ_COPY_PACKET, BUFSIZE);
        if (status < 0)
                die(h);

        return h;
}

static void fail_retry(struct ipq_handle **h)
{
        ipq_perror("passer");
        ipq_destroy_handle(*h);

        *h = make_ipq();
}

static int filter(void)
{
        int verdict;

        /* filter to return either NF_ACCEPT or NF_DROP */

        /* 
         * this filter could get arbitrarily complex to walk through
         * some attribute of the packet (protocol, source, etc...) but
         * right now its a simple all/nothing DROP or ACCEPT based on 
         * counts, etc.
         */

        switch (last_action) {
        case ACTION_ALLOW_ALL:
                verdict = NF_ACCEPT;
                break;

        case ACTION_DISALLOW_ALL:
                verdict = NF_DROP;
                break;

        default:
                switch (mode) {
                case DROP_NONE:
                        verdict = NF_ACCEPT;
                        break;

                case DROP_PERCENT:
                        if ((rand() % 100) < drop_percent)
                                verdict = NF_DROP;
                        else
                                verdict = NF_ACCEPT;
                        break;
                        
                case DROP_PACKET:
                        if (curr_packet_count < drop_packet)
                                verdict = NF_ACCEPT;
                        else
                                verdict = NF_DROP;
                        break;

                default:
                        abort();
                }
        }

        /* monotonically increasing packet count */
        packet_count++;

        /* reset-able packet count */
        curr_packet_count++;

        /* catch all */
        return verdict;
}

int main(int argc, char **argv)
{
        int verdict;
        unsigned char buf[BUFSIZE];
        struct ipq_handle *h;
        int cfd;
        int daemonize = 1;

        if (argc > 1) {
            if (!strcmp(argv[1], "-nd")) {
                daemonize = 0;
            }
        }

        srand(0);

        cfd = make_ipc();
        if ( cfd < 0 )
                exit(1);

        printhelp();

        h = make_ipq();

        if (daemonize) {
                FILE *logfile = 0;
                int pid;

                logfile = fopen(LOGFILE, "a");
                if (!logfile) {
                        perror("Failed to open " LOGFILE);
                        exit(1);
                }

                if ( daemon(0, 0) < 0 ) {
                        perror("Failed to daemonize");
                        exit(1);
                }

                if ( dup2(fileno(logfile), STDERR_FILENO) < 0 )
                        abort();
                fclose(logfile);

                pid = writepid();
                if (pid < 0)
                        return -1;

                /* disable low water mark check for io pages */
                if (setpriority(PRIO_PROCESS, pid, PRIO_SPECIAL_IO)) {
                        perror("Unable to prioritize tapdisk proc");
                        exit(1);
                }
                
                TRACE("Start pfilter PID %d\n", pid);
        }

        do {
                ssize_t status;
                int type;

                status = ipq_read(h, buf, BUFSIZE, 0);
                if (status < 0)
                        fail_retry(&h);

                type = ipq_message_type(buf);
                switch (type) {
                        case NLMSG_ERROR:
                                TRACE("pfilter: Received error message %d\n",
                                      ipq_get_msgerr(buf));
                                break;

                        case IPQM_PACKET: {
                                ipq_packet_msg_t *m = ipq_get_packet(buf);

                                verdict = filter();

                                status = ipq_set_verdict(h, m->packet_id,
                                                         verdict, 0, NULL);
                                if (status < 0)
                                        fail_retry(&h);

                                trace_data(verdict);
                                break;
                        }

                        default:
                                TRACE("pfilter: Unknown message type: %d\n", type);
                                break;
                }
        } while (1);

        ipq_destroy_handle(h);
        return 0;
}

