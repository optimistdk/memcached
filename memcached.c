/* -*- Mode: C; tab-width: 4; c-basic-offset: 4; indent-tabs-mode: nil -*- */
/*
 *  memcached - memory caching daemon
 *
 *       http://www.danga.com/memcached/
 *
 *  Copyright 2003 Danga Interactive, Inc.  All rights reserved.
 *
 *  Use and distribution licensed under the BSD license.  See
 *  the LICENSE file for full text.
 *
 *  Authors:
 *      Anatoly Vorobey <mellon@pobox.com>
 *      Brad Fitzpatrick <brad@danga.com>
 *
 *  $Id$
 */
#include "generic.h"

#include <signal.h>
#include <sys/stat.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <sys/signal.h>
#include <sys/resource.h>
#include <sys/uio.h>

#include <pwd.h>
#include <fcntl.h>
#include <netinet/tcp.h>
#include <arpa/inet.h>
#include <netdb.h>
#include <errno.h>
#ifdef HAVE_STDARG_H
#include <stdarg.h>
#endif
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <time.h>
#include <assert.h>

#ifdef HAVE_MALLOC_H
/* OpenBSD has a malloc.h, but warns to use stdlib.h instead */
#ifndef __OpenBSD__
#include <malloc.h>
#endif
#endif

#include "assoc.h"
#include "binary_sm.h"
#include "items.h"
#include "memcached.h"
#include "stats.h"
#include "sigseg.h"
#include "conn_buffer.h"

#if defined(USE_SLAB_ALLOCATOR)
#include "slabs_items_support.h"
#endif /* #if defined(USE_SLAB_ALLOCATOR) */
#if defined(USE_FLAT_ALLOCATOR)
#include "flat_storage_support.h"
#endif /* #if defined(USE_FLAT_ALLOCATOR) */

#define LISTEN_DEPTH 4096

/*
 * forward declarations
 */
static void drive_machine(conn* c);
static int new_socket(const bool is_udp);
static int server_socket(const int port, const bool is_udp);
static int try_read_command(conn *c);

static void maximize_socket_buffer(const int sfd, int optname);

/* defaults */
static void settings_init(void);

/* event handling, network IO */
static void event_handler(const int fd, const short which, void *arg);
static void conn_init(void);
static void complete_nread(conn* c);
static void process_command(conn* c, char *command);
static int ensure_iov_space(conn* c);

void pre_gdb(void);
static void conn_free(conn* c);

/** exported globals **/
settings_t settings;
int maps_fd = -1;

/** file scope variables **/
static item **todelete = NULL;
static int delcurr;
static int deltotal;
static conn* listen_conn;
static conn* listen_binary_conn;
struct event_base *main_base;

static int *buckets = 0; /* bucket->generation array for a managed instance */

#define REALTIME_MAXDELTA 60*60*24*30
/*
 * given time value that's either unix time or delta from current unix time, return
 * unix time. Use the fact that delta can't exceed one month (and real time value can't
 * be that low).
 */
rel_time_t realtime(const time_t exptime) {
    /* no. of seconds in 30 days - largest possible delta exptime */

    if (exptime == 0) return 0; /* 0 means never expire */

    if (exptime > REALTIME_MAXDELTA) {
        /* if item expiration is at/before the server started, give it an
           expiration time of 1 second after the server started.
           (because 0 means don't expire).  without this, we'd
           underflow and wrap around to some large value way in the
           future, effectively making items expiring in the past
           really expiring never */
        if (exptime <= started)
            return (rel_time_t)1;
        return (rel_time_t)(exptime - started);
    } else {
        return (rel_time_t)(exptime + current_time);
    }
}

size_t append_to_buffer(char* const buffer_start,
                        const size_t buffer_size,
                        const size_t buffer_off,
                        const size_t reserved,
                        const char* fmt,
                        ...) {
    va_list ap;
    ssize_t written;
    size_t left = buffer_size - buffer_off - reserved;

    va_start(ap, fmt);
    written = vsnprintf(&buffer_start[buffer_off], left, fmt, ap);
    va_end(ap);

    if (written < 0) {
        return buffer_off;
    } else if (written >= left) {
        buffer_start[buffer_off] = 0;
        return buffer_off;
    }

    return buffer_off + written;
}

static void settings_init(void) {
    settings.port = 0;
    settings.udpport = 0;
    settings.binary_port = 0;
    settings.binary_udpport = 0;
    settings.interf.s_addr = htonl(INADDR_ANY);
    settings.maxbytes = 64 * 1024 * 1024; /* default is 64MB */
    settings.maxconns = 1024;         /* to limit connections-related memory to about 5MB */
    settings.verbose = 0;
    settings.oldest_live = 0;
    settings.evict_to_free = 1;       /* push old items out of cache when memory runs out */
    settings.socketpath = NULL;       /* by default, not using a unix socket */
    settings.managed = false;
    settings.factor = 1.25;
    settings.chunk_size = 48;         /* space for a modest key and value */
    settings.prefix_delimiter = ':';
    settings.detail_enabled = 0;
    settings.reqs_per_event = 1;

#ifdef HAVE__SC_NPROCESSORS_ONLN
    /*
     * If the system supports detecting the number of active processors
     * use that to determine the number of worker threads + 1 dispatcher.
     */
    settings.num_threads = sysconf(_SC_NPROCESSORS_ONLN) + 1;
#else
    settings.num_threads = 4 + 1;     /* N workers + 1 dispatcher */
#endif
}

/*
 * Adds a message header to a connection.
 *
 * Returns 0 on success, -1 on out-of-memory.
 */
int add_msghdr(conn* c)
{
    struct msghdr *msg;

    assert(c != NULL);

    if (c->msgsize == c->msgused) {
        msg = pool_realloc(c->msglist, c->msgsize * 2 * sizeof(struct msghdr),
                           c->msgsize * sizeof(struct msghdr), CONN_BUFFER_MSGLIST_POOL);
        if (! msg)
            return -1;
        c->msglist = msg;
        c->msgsize *= 2;
    }

    msg = c->msglist + c->msgused;

    /* this wipes msg_iovlen, msg_control, msg_controllen, and
       msg_flags, the last 3 of which aren't defined on solaris: */
    memset(msg, 0, sizeof(struct msghdr));

    if (ensure_iov_space(c) != 0) {
        return -1;
    }

    msg->msg_iov = &c->iov[c->iovused];
    msg->msg_name = &c->request_addr;
    msg->msg_namelen = c->request_addr_size;

    c->msgbytes = 0;
    c->msgused++;

    if (c->udp) {
        /* Leave room for the UDP header, which we'll fill in later. */
        return add_iov(c, NULL, UDP_HEADER_SIZE, false);
    }

    return 0;
}


/*
 * Free list management for connections.
 */

static conn* *freeconns;
static int freetotal;
static int freecurr;


static void conn_init(void) {
    freetotal = 200;
    freecurr = 0;
    if ((freeconns = (conn* *)pool_malloc(sizeof(conn*) * freetotal, CONN_POOL)) == NULL) {
        perror("malloc()");
    }
    return;
}

/*
 * Returns a connection from the freelist, if any. Should call this using
 * conn_from_freelist() for thread safety.
 */
conn* do_conn_from_freelist() {
    conn* c;

    if (freecurr > 0) {
        c = freeconns[--freecurr];
    } else {
        c = NULL;
    }

    return c;
}

/*
 * Adds a connection to the freelist. 0 = success. Should call this using
 * conn_add_to_freelist() for thread safety.
 */
bool do_conn_add_to_freelist(conn* c) {
    if (freecurr < freetotal) {
        freeconns[freecurr++] = c;
        return false;
    } else {
        /* try to enlarge free connections array */
        conn* *new_freeconns = pool_realloc(freeconns,
                                            sizeof(conn*) * freetotal * 2,
                                            sizeof(conn*)  * freetotal,
                                            CONN_POOL);
        if (new_freeconns) {
            freetotal *= 2;
            freeconns = new_freeconns;
            freeconns[freecurr++] = c;
            return false;
        }
    }
    return true;
}

#if defined(HAVE_UDP_REPLY_PORTS)
/* Allocate a port for udp reply transmission.

   Starting from the port of the reciever socket, increment by one
   until bind succeeds or we fail settings.num_threads times.
   No locking is needed since each thread is racing to get a port
   and the OS will only allow one thread to bind to any particular
   port. */
static int allocate_udp_reply_port(int sfd, int tries) {
    struct addrinfo hints, *res = NULL;
    struct sockaddr addr;
    socklen_t addr_len;
    char port[6];
    char host[100];
    int error;
    int xfd = -1;

    /* Need the address to lookup the host:port pair */
    addr_len = sizeof(addr);
    if (getsockname(sfd, &addr, &addr_len) < 0) {
        perror("getsockname");
        return -1;
    }

    /* Lookup the host:port pair for the recieve socket. */
    if (getnameinfo(&addr, addr_len, host, sizeof(host), port, sizeof(port), NI_NUMERICHOST|NI_NUMERICSERV)) {
        perror("getnameinfo");
        return -1;
    }

    /* Already bound to the recieve port, so start the search
       at the next one. */
    sprintf(port, "%d", atoi(port) + 1);

    for (; tries; tries--) {
        memset(&hints, 0, sizeof(hints));
        hints.ai_family = PF_INET;
        hints.ai_socktype = SOCK_DGRAM;
        hints.ai_flags = AI_PASSIVE | AI_ADDRCONFIG | AI_NUMERICHOST;

        if (res)
            freeaddrinfo(res);

        res = NULL;
        error = getaddrinfo(host, port, &hints, &res);
        if (error) {
            fprintf(stderr, "%s\n", gai_strerror(error));
            break;
        }

        xfd = socket(res->ai_family, res->ai_socktype, res->ai_protocol);
        if (xfd < 0) {
            perror("socket");
            break;
        }

        if (bind(xfd, res->ai_addr, res->ai_addrlen) < 0) {
            close(xfd);
            xfd = -1;
            if (errno == EADDRINUSE) {
                sprintf(port, "%d", atoi(port) + 1); /* next port */
                continue;
            }
            perror("bind");
        }
        maximize_socket_buffer(xfd, SO_SNDBUF);
        break; /* success or error, break out */
    }
    if (res)
        freeaddrinfo(res);
    return xfd;
}
#endif

conn *conn_new(const int sfd, const int init_state, const int event_flags,
               conn_buffer_group_t* cbg, const bool is_udp, const bool is_binary,
               const struct sockaddr* const addr, const socklen_t addrlen,
               struct event_base *base) {
    stats_t *stats = STATS_GET_TLS();
    conn* c = conn_from_freelist();

    if (NULL == c) {
        if (!(c = (conn*)pool_calloc(1, sizeof(conn), CONN_POOL))) {
            perror("malloc()");
            return NULL;
        }

        c->rsize = 0;
        c->wsize = DATA_BUFFER_SIZE;
        c->isize = ITEM_LIST_INITIAL;
        c->iovsize = 0;
        c->msgsize = MSG_LIST_INITIAL;
        c->hdrsize = 0;
        c->riov_size = 0;

        c->rbuf = NULL;
        c->wbuf = (char *)pool_malloc((size_t)c->wsize, CONN_BUFFER_WBUF_POOL);
        c->ilist = (item **)pool_malloc(sizeof(item *) * c->isize, CONN_BUFFER_ILIST_POOL);
        c->iov = NULL;
        c->msglist = (struct msghdr *)pool_malloc(sizeof(struct msghdr) * c->msgsize, CONN_BUFFER_MSGLIST_POOL);
        c->hdrbuf = NULL;
        c->riov = NULL;

        if (is_binary) {
            // because existing functions expects the key to be null-terminated,
            // we must do so as well.
            c->bp_key = (char*)pool_malloc(sizeof(char) * KEY_MAX_LENGTH + 1, CONN_BUFFER_BP_KEY_POOL);

            c->bp_hdr_pool = bp_allocate_hdr_pool(NULL);
        } else {
            c->bp_key = NULL;
            c->bp_hdr_pool = NULL;
        }

        if (c->wbuf == 0 ||
            c->ilist == 0 ||
            c->msglist == 0 ||
            (is_binary && c->bp_key == 0)) {
            if (c->wbuf != 0) pool_free(c->wbuf, c->wsize, CONN_BUFFER_WBUF_POOL);
            if (c->ilist !=0) pool_free(c->ilist, sizeof(item*) * c->isize, CONN_BUFFER_ILIST_POOL);
            if (c->msglist != 0) pool_free(c->msglist, sizeof(struct msghdr) * c->msgsize, CONN_BUFFER_MSGLIST_POOL);
            if (c->bp_key != 0) pool_free(c->bp_key, sizeof(char) * KEY_MAX_LENGTH + 1, CONN_BUFFER_BP_KEY_POOL);
            if (c->bp_hdr_pool != NULL) bp_release_hdr_pool(c);
            pool_free(c, 1 * sizeof(conn), CONN_POOL);
            perror("malloc()");
            return NULL;
        }

        STATS_LOCK(stats);
        stats->conn_structs++;
        STATS_UNLOCK(stats);
    }

    memcpy(&c->request_addr, addr, addrlen);
    if (settings.socketpath) {
        c->request_addr_size = 0;   /* for unix-domain sockets, don't store
                                     * a request addr. */
    } else {
        c->request_addr_size = addrlen;
    }
    c->cbg = cbg;

    if (settings.verbose > 1) {
        if (init_state == conn_listening)
            fprintf(stderr, "<%d server listening\n", sfd);
        else if (is_udp)
            fprintf(stderr, "<%d server listening (udp)\n", sfd);
        else
            fprintf(stderr, "<%d new client connection\n", sfd);
    }

    c->sfd = c->xfd = sfd;
#if defined(HAVE_UDP_REPLY_PORTS)
    /* The linux UDP transmit path is heavily contended when more than one
       thread is writing to the same socket.  If configured to support
       per-thread reply ports, allocate a per-thread udp socket and set the
       udp fd accordingly, otherwise use the recieve socket. The transmit
       fd is set in try_read_udp to validate that the client request
       can handle the number of reply ports the server is configured for. */
    if (is_udp) {
        c->ufd = allocate_udp_reply_port(sfd, settings.num_threads - 1);
        if (c->ufd == -1) {
            fprintf(stderr, "unable to allocate all udp reply ports.\n");
            exit(1);
        }
    }
#endif
    c->udp = is_udp;
    c->binary = is_binary;
    c->state = init_state;
    c->rbytes = c->wbytes = 0;
    c->rcurr = c->rbuf;
    c->wcurr = c->wbuf;
    c->icurr = c->ilist;
    c->ileft = 0;
    c->iovused = 0;
    c->msgcurr = 0;
    c->msgused = 0;
    c->riov_curr = 0;
    c->riov_left = 0;

    c->write_and_go = conn_read;
    c->write_and_free = 0;
    c->item = 0;
    c->bucket = -1;
    c->gen = 0;

    event_set(&c->event, sfd, event_flags, event_handler, (void *)c);
    event_base_set(base, &c->event);
    c->ev_flags = event_flags;

    if (event_add(&c->event, 0) == -1) {
        if (conn_add_to_freelist(c)) {
            conn_free(c);
        }
        return NULL;
    }

    STATS_LOCK(stats);
    stats->curr_conns++;
    stats->total_conns++;
    STATS_UNLOCK(stats);

    return c;
}

void conn_cleanup(conn* c) {
    assert(c != NULL);

    if (c->item) {
        item_deref(c->item);
        c->item = 0;
    }

    if (c->ileft != 0) {
        for (; c->ileft > 0; c->ileft--,c->icurr++) {
            item_deref(*(c->icurr));
        }
    }

    if (c->write_and_free) {
        free(c->write_and_free);
        c->write_and_free = 0;
    }

    if (c->rbuf) {
        free_conn_buffer(c->cbg, c->rbuf, 0);   /* no idea how much was used... */
        c->rbuf = NULL;
        c->rsize = 0;
    }
    if (c->iov) {
        free_conn_buffer(c->cbg, c->iov, 0);    /* no idea how much was used... */
        c->iov = NULL;
        c->iovsize = 0;
    }

    if (c->riov) {
        free_conn_buffer(c->cbg, c->riov, 0);   /* no idea how much was used... */
        c->riov = NULL;
        c->riov_size = 0;
    }
}

/*
 * Frees a connection.
 */
void conn_free(conn* c) {
    if (c) {
        if (c->hdrbuf)
            pool_free(c->hdrbuf, c->hdrsize * UDP_HEADER_SIZE, CONN_BUFFER_HDRBUF_POOL);
        if (c->msglist)
            pool_free(c->msglist, sizeof(struct msghdr) * c->msgsize, CONN_BUFFER_MSGLIST_POOL);
        if (c->rbuf)
            free_conn_buffer(c->cbg, c->rbuf, 0);
        if (c->wbuf)
            pool_free(c->wbuf, c->wsize, CONN_BUFFER_WBUF_POOL);
        if (c->ilist)
            pool_free(c->ilist, sizeof(item*) * c->isize, CONN_BUFFER_ILIST_POOL);
        if (c->iov)
            free_conn_buffer(c->cbg, c->iov, c->iovused * sizeof(struct iovec));
        if (c->riov)
            free_conn_buffer(c->cbg, c->riov, 0);
        if (c->bp_key)
            pool_free(c->bp_key, sizeof(char) * KEY_MAX_LENGTH + 1, CONN_BUFFER_BP_KEY_POOL);
        if (c->bp_hdr_pool)
            bp_release_hdr_pool(c);
        pool_free(c, 1 * sizeof(conn), CONN_POOL);
    }
}

void conn_close(conn* c) {
    stats_t *stats = STATS_GET_TLS();
    assert(c != NULL);

    /* delete the event, the socket and the conn */
    event_del(&c->event);

    if (settings.verbose > 1)
        fprintf(stderr, "<%d connection closed.\n", c->sfd);

    close(c->sfd);
    accept_new_conns(true, c->binary);
    conn_cleanup(c);

    /* if the connection has big buffers, just free it */
    if (c->rsize > READ_BUFFER_HIGHWAT ||
        c->wsize > WRITE_BUFFER_HIGHWAT ||
        conn_add_to_freelist(c)) {
        conn_free(c);
    }

    STATS_LOCK(stats);
    stats->curr_conns--;
    STATS_UNLOCK(stats);

    return;
}


/*
 * Shrinks a connection's buffers if they're too big.  This prevents
 * periodic large "get" requests from permanently chewing lots of server
 * memory.
 *
 * This should only be called in between requests since it can wipe output
 * buffers!
 */
void conn_shrink(conn* c) {
    assert(c != NULL);

    if (c->udp)
        return;

    if (c->rbytes == 0 && c->rbuf != NULL) {
        /* drop the buffer since we have no bytes to preserve. */
        free_conn_buffer(c->cbg, c->rbuf, 0);
        c->rbuf = NULL;
        c->rcurr = NULL;
        c->rsize = 0;
    } else {
        memmove(c->rbuf, c->rcurr, (size_t)c->rbytes);
        c->rcurr = c->rbuf;
    }

    if (c->wsize > WRITE_BUFFER_HIGHWAT) {
        char *newbuf;

        newbuf = (char*) pool_realloc((void*) c->wbuf, DATA_BUFFER_SIZE,
                                      c->wsize, CONN_BUFFER_WBUF_POOL);

        if (newbuf) {
            c->wbuf = newbuf;
            c->wsize = DATA_BUFFER_SIZE;
        }
    }

    if (c->isize > ITEM_LIST_HIGHWAT) {
        item **newbuf = (item**) pool_realloc((void *)c->ilist, ITEM_LIST_INITIAL * sizeof(c->ilist[0]),
                                              c->isize * sizeof(c->ilist[0]), CONN_BUFFER_ILIST_POOL);
        if (newbuf) {
            c->ilist = newbuf;
            c->isize = ITEM_LIST_INITIAL;
        }
    /* TODO check error condition? */
    }

    if (c->msgsize > MSG_LIST_HIGHWAT) {
        struct msghdr *newbuf = (struct msghdr *) pool_realloc((void *)c->msglist,
                                                               MSG_LIST_INITIAL * sizeof(c->msglist[0]),
                                                               c->msgsize * sizeof(c->msglist[0]),
                                                               CONN_BUFFER_MSGLIST_POOL);
        if (newbuf) {
            c->msglist = newbuf;
            c->msgsize = MSG_LIST_INITIAL;
        }
    /* TODO check error condition? */
    }

    if (c->riov) {
        free_conn_buffer(c->cbg, c->riov, 0);
        c->riov = NULL;
        c->riov_size = 0;
    }

    if (c->iov != NULL) {
        free_conn_buffer(c->cbg, c->iov, 0);
        c->iov = NULL;
        c->iovsize = 0;
    }

    if (c->binary) {
        bp_shrink_hdr_pool(c);
    }
}

/*
 * Sets a connection's current state in the state machine. Any special
 * processing that needs to happen on certain state transitions can
 * happen here.
 */
static void conn_set_state(conn* c, int state) {
    assert(c != NULL);

    if (state != c->state) {
        if (state == conn_read) {
            conn_shrink(c);
            assoc_move_next_bucket();

            c->msgcurr = 0;
            c->msgused = 0;
            c->iovused = 0;
        }
        c->state = state;
    }
}


/*
 * Ensures that there is room for another struct iovec in a connection's
 * iov list.
 *
 * Returns 0 on success, -1 on out-of-memory.
 */
static int ensure_iov_space(conn* c) {
    assert(c != NULL);

    if (c->iovsize == 0) {
        c->iov = (struct iovec *)alloc_conn_buffer(c->cbg, 0);
        if (c->iov != NULL) {
            c->iovsize = CONN_BUFFER_DATA_SZ / sizeof(struct iovec);
        }
    }

    if (c->iovused >= c->iovsize) {
        return -1;
    }

    report_max_rusage(c->cbg, c->iov, (c->iovused + 1) * sizeof(struct iovec));

    return 0;
}


/*
 * Adds data to the list of pending data that will be written out to a
 * connection.
 *
 * is_start should be true if this data represents the start of a protocol
 * response, e.g., a "VALUE" line.
 *
 * Returns 0 on success, -1 on out-of-memory.
 */

int add_iov(conn* c, const void *buf, int len, bool is_start) {
    struct msghdr *m;
    int leftover;
    bool limit_to_mtu;

    assert(c != NULL);
    assert(c->msgused > 0);

    do {
        m = &c->msglist[c->msgused - 1];

        /*
         * Limit UDP packets, and the first payloads of TCP replies, to
         * UDP_MAX_PAYLOAD_SIZE bytes.
         */
        limit_to_mtu = c->udp || (1 == c->msgused);

        /* We may need to start a new msghdr if this one is full. */
        if (m->msg_iovlen == IOV_MAX ||
            (limit_to_mtu && c->msgbytes >= UDP_MAX_PAYLOAD_SIZE)) {
            add_msghdr(c);
            m = &c->msglist[c->msgused - 1];
        }

        if (ensure_iov_space(c) != 0)
            return -1;

        /* If the fragment is too big to fit in the datagram, split it up */
        if (limit_to_mtu && len + c->msgbytes > UDP_MAX_PAYLOAD_SIZE) {
            leftover = len + c->msgbytes - UDP_MAX_PAYLOAD_SIZE;
            len -= leftover;
        } else {
            leftover = 0;
        }

        m = &c->msglist[c->msgused - 1];
        m->msg_iov[m->msg_iovlen].iov_base = (void *)buf;
        m->msg_iov[m->msg_iovlen].iov_len = len;

        /*
         * If this is the start of a response (e.g., a "VALUE" line),
         * and it's the first one so far in this message, mark it as
         * such so we can put its offset in the UDP header.
         */
        if (c->udp && is_start && ! m->msg_flags) {
          m->msg_flags = 1;
          m->msg_controllen = m->msg_iovlen;
        }

        c->msgbytes += len;
        c->iovused++;
        m->msg_iovlen++;

        buf = ((char *)buf) + len;
        len = leftover;
        is_start = false;
    } while (leftover > 0);

    return 0;
}


/*
 * Constructs a set of UDP headers and attaches them to the outgoing messages.
 */
int build_udp_headers(conn* c) {
    int i, j, offset;
    unsigned char *hdr;

    assert(c != NULL);

    if (c->msgused > c->hdrsize) {
        void *new_hdrbuf;
        if (c->hdrbuf)
            new_hdrbuf = pool_realloc(c->hdrbuf, c->msgused * 2 * UDP_HEADER_SIZE,
                                      c->hdrsize * UDP_HEADER_SIZE, CONN_BUFFER_HDRBUF_POOL);
        else
            new_hdrbuf = pool_malloc(c->msgused * 2 * UDP_HEADER_SIZE, CONN_BUFFER_HDRBUF_POOL);
        if (! new_hdrbuf)
            return -1;
        c->hdrbuf = (unsigned char *)new_hdrbuf;
        c->hdrsize = c->msgused * 2;
    }

    hdr = c->hdrbuf;
    for (i = 0; i < c->msgused; i++) {
        c->msglist[i].msg_iov[0].iov_base = hdr;
        c->msglist[i].msg_iov[0].iov_len = UDP_HEADER_SIZE;

        /* Find the offset of the first response line in the message, if any */
        offset = 0;
        if (c->msglist[i].msg_flags) {
            for (j = 0; j < c->msglist[i].msg_controllen; j++) {
                offset += c->msglist[i].msg_iov[j].iov_len;
            }
            c->msglist[i].msg_flags = 0;
            c->msglist[i].msg_controllen = 0;
        }

        *hdr++ = c->request_id / 256;
        *hdr++ = c->request_id % 256;
        *hdr++ = i / 256;
        *hdr++ = i % 256;
        *hdr++ = c->msgused / 256;
        *hdr++ = c->msgused % 256;
        *hdr++ = offset / 256;
        *hdr++ = offset % 256;
        assert((void *) hdr == (void *)c->msglist[i].msg_iov[0].iov_base + UDP_HEADER_SIZE);
    }

    return 0;
}


static void out_string(conn* c, const char *str) {
    size_t len;

    assert(c != NULL);
    assert(c->msgcurr == 0);
    c->msgused = 0;
    c->iovused = 0;

    if (settings.verbose > 1)
        fprintf(stderr, ">%d %s\n", c->sfd, str);

    len = strlen(str);
    if ((len + 2) > c->wsize) {
        /* ought to be always enough. just fail for simplicity */
        str = "SERVER_ERROR output line too long";
        len = strlen(str);
    }

    memcpy(c->wbuf, str, len);
    memcpy(c->wbuf + len, "\r\n", 2);
    c->wbytes = len + 2;
    c->wcurr = c->wbuf;

    conn_set_state(c, conn_write);
    c->write_and_go = conn_read;
    return;
}

/*
 * we get here after reading the value in set/add/replace commands. The command
 * has been stored in c->item_comm, and the item is ready in c->item.
 */

static void complete_nread(conn* c) {
    stats_t *stats = STATS_GET_TLS();
    assert(c != NULL);

    item *it = c->item;
    int comm = c->item_comm;

    STATS_LOCK(stats);
    stats->set_cmds++;
    STATS_UNLOCK(stats);

    if (memcmp("\r\n", c->crlf, 2) != 0) {
        out_string(c, "CLIENT_ERROR bad data chunk");
    } else {
        if (store_item(it, comm, c->update_key)) {
            out_string(c, "STORED");
        } else {
            out_string(c, "NOT_STORED");
        }
    }

    item_deref(c->item);       /* release the c->item reference */
    c->item = 0;
}

/*
 * Stores an item in the cache according to the semantics of one of the set
 * commands. In threaded mode, this is protected by the cache lock.
 *
 * Returns true if the item was stored.
 */
int do_store_item(item *it, int comm, const char* key) {
    bool delete_locked = false;
    item *old_it;
    int stored = 0;
    size_t nkey = ITEM_nkey(it);

    old_it = do_item_get_notedeleted(key, nkey, &delete_locked);

    if (old_it != NULL && comm == NREAD_ADD) {
        /* add only adds a nonexistent item, but promote to head of LRU */
        do_item_update(old_it);
    } else if (!old_it && comm == NREAD_REPLACE) {
        /* replace only replaces an existing value; don't store */
    } else if (delete_locked && (comm == NREAD_REPLACE || comm == NREAD_ADD)) {
        /* replace and add can't override delete locks; don't store */
    } else {
        /* "set" commands can override the delete lock
           window... in which case we have to find the old hidden item
           that's in the namespace/LRU but wasn't returned by
           item_get.... because we need to replace it */
        if (delete_locked) {
            old_it = do_item_get_nocheck(key, nkey);
        }

        if (settings.detail_enabled) {
            int prefix_stats_flags = PREFIX_INCR_ITEM_COUNT;

            if (old_it != NULL) {
                prefix_stats_flags |= PREFIX_IS_OVERWRITE;
            }
            stats_prefix_record_byte_total_change(key, nkey, ITEM_nkey(it) + ITEM_nbytes(it),
                                                  prefix_stats_flags);
        }

        stats_set(ITEM_nkey(it) + ITEM_nbytes(it),
                  (old_it == NULL) ? 0 : ITEM_nkey(old_it) + ITEM_nbytes(old_it));

        if (old_it != NULL) {
            do_item_replace(old_it, it, key);
        } else {
            do_item_link(it, key);
        }

        stored = 1;
    }

    if (old_it)
        do_item_deref(old_it);         /* release our reference */
    return stored;
}

typedef struct token_s {
    char *value;
    size_t length;
} token_t;

#define COMMAND_TOKEN 0
#define SUBCOMMAND_TOKEN 1
#define KEY_TOKEN 1

#define MAX_TOKENS 6

/*
 * Tokenize the command string by replacing whitespace with '\0' and update
 * the token array tokens with pointer to start of each token and length.
 * Returns total number of tokens.  The last valid token is the terminal
 * token (value points to the first unprocessed character of the string and
 * length zero).
 *
 * Usage example:
 *
 *  while(tokenize_command(command, ncommand, tokens, max_tokens) > 0) {
 *      for(int ix = 0; tokens[ix].length != 0; ix++) {
 *          ...
 *      }
 *      ncommand = tokens[ix].value - command;
 *      command  = tokens[ix].value;
 *   }
 */
static size_t tokenize_command(char *command, token_t *tokens, const size_t max_tokens) {
    char *s, *e;
    size_t ntokens = 0;

    assert(command != NULL && tokens != NULL && max_tokens > 1);

    for (s = e = command; ntokens < max_tokens - 1; ++e) {
        if (*e == ' ') {
            if (s != e) {
                tokens[ntokens].value = s;
                tokens[ntokens].length = e - s;
                ntokens++;
                *e = '\0';
            }
            s = e + 1;
        }
        else if (*e == '\0') {
            if (s != e) {
                tokens[ntokens].value = s;
                tokens[ntokens].length = e - s;
                ntokens++;
            }

            break; /* string end */
        }
    }

    /*
     * If we scanned the whole string, the terminal value pointer is null,
     * otherwise it is the first unprocessed character.
     */
    tokens[ntokens].value =  *e == '\0' ? NULL : e;
    tokens[ntokens].length = 0;
    ntokens++;

    return ntokens;
}

/* set up a connection to write a buffer then free it, used for stats */
static void write_and_free(conn* c, char *buf, int bytes) {
    assert(c->msgcurr == 0);
    c->msgused = 0;
    c->iovused = 0;

    if (buf) {
        c->write_and_free = buf;
        c->wcurr = buf;
        c->wbytes = bytes;
        conn_set_state(c, conn_write);
        c->write_and_go = conn_read;
    } else {
        out_string(c, "SERVER_ERROR out of memory");
    }
}

inline static void process_stats_detail(conn* c, const char *command) {
    assert(c != NULL);

    if (strcmp(command, "on") == 0) {
        settings.detail_enabled = 1;
        out_string(c, "OK");
    }
    else if (strcmp(command, "off") == 0) {
        settings.detail_enabled = 0;
        out_string(c, "OK");
    }
    else if (strcmp(command, "dump") == 0) {
        int len;
        char *stats;
        stats = stats_prefix_dump(&len);
        write_and_free(c, stats, len);
    }
    else {
        out_string(c, "CLIENT_ERROR usage: stats detail on|off|dump");
    }
}

static void process_stat(conn* c, token_t *tokens, const size_t ntokens) {
    rel_time_t now = current_time;
    char *command;
    char *subcommand;
    stats_t stats;

    assert(c != NULL);

    if(ntokens < 2) {
        out_string(c, "CLIENT_ERROR bad command line");
        return;
    }

    command = tokens[COMMAND_TOKEN].value;

    STATS_AGGREGATE(&stats);
    if (ntokens == 2 && strcmp(command, "stats") == 0) {
        size_t bufsize = 2048, offset = 0;
        char temp[bufsize];
        char terminator[] = "END";
        pid_t pid = getpid();

#ifndef WIN32
        struct rusage usage;
        getrusage(RUSAGE_SELF, &usage);
#endif /* !WIN32 */

        offset = append_to_buffer(temp, bufsize, offset, sizeof(terminator), "STAT pid %u\r\n", pid);
        offset = append_to_buffer(temp, bufsize, offset, sizeof(terminator), "STAT uptime %u\r\n", now);
        offset = append_to_buffer(temp, bufsize, offset, sizeof(terminator), "STAT time %ld\r\n", now + started);
        offset = append_to_buffer(temp, bufsize, offset, sizeof(terminator), "STAT version " VERSION "\r\n");
        offset = append_to_buffer(temp, bufsize, offset, sizeof(terminator), "STAT pointer_size %lu\r\n", 8 * sizeof(void *));
#if defined(USE_SLAB_ALLOCATOR)
        offset = append_to_buffer(temp, bufsize, offset, sizeof(terminator), "STAT allocator slab\r\n");
#endif /* #if defined(USE_SLAB_ALLOCATOR) */
#if defined(USE_FLAT_ALLOCATOR)
        offset = append_to_buffer(temp, bufsize, offset, sizeof(terminator), "STAT allocator flat-sk\r\n");
#endif /* #if defined(USE_FLAT_ALLOCATOR) */
#ifndef WIN32
        offset = append_to_buffer(temp, bufsize, offset, sizeof(terminator), "STAT rusage_user %ld.%06d\r\n", usage.ru_utime.tv_sec, (int) usage.ru_utime.tv_usec);
        offset = append_to_buffer(temp, bufsize, offset, sizeof(terminator), "STAT rusage_system %ld.%06d\r\n", usage.ru_stime.tv_sec, (int) usage.ru_stime.tv_usec);
#endif /* !WIN32 */
        offset = append_to_buffer(temp, bufsize, offset, sizeof(terminator), "STAT curr_items %u\r\n", stats.curr_items);
        offset = append_to_buffer(temp, bufsize, offset, sizeof(terminator), "STAT total_items %u\r\n", stats.total_items);
        offset = append_to_buffer(temp, bufsize, offset, sizeof(terminator), "STAT item_allocated %" PRINTF_INT64_MODIFIER "u\r\n", stats.item_storage_allocated);
        offset = append_to_buffer(temp, bufsize, offset, sizeof(terminator), "STAT item_total_size %" PRINTF_INT64_MODIFIER "u\r\n", stats.item_total_size);
        offset = append_to_buffer(temp, bufsize, offset, sizeof(terminator), "STAT curr_connections %u\r\n", stats.curr_conns - 1); /* ignore listening conn */
        offset = append_to_buffer(temp, bufsize, offset, sizeof(terminator), "STAT total_connections %u\r\n", stats.total_conns);
        offset = append_to_buffer(temp, bufsize, offset, sizeof(terminator), "STAT connection_structures %u\r\n", stats.conn_structs);
        offset = append_to_buffer(temp, bufsize, offset, sizeof(terminator), "STAT cmd_get %" PRINTF_INT64_MODIFIER "u\r\n", stats.get_cmds);
        offset = append_to_buffer(temp, bufsize, offset, sizeof(terminator), "STAT cmd_set %" PRINTF_INT64_MODIFIER "u\r\n", stats.set_cmds);
        offset = append_to_buffer(temp, bufsize, offset, sizeof(terminator), "STAT get_hits %" PRINTF_INT64_MODIFIER "u\r\n", stats.get_hits);
        offset = append_to_buffer(temp, bufsize, offset, sizeof(terminator), "STAT get_misses %" PRINTF_INT64_MODIFIER "u\r\n", stats.get_misses);
        offset = append_to_buffer(temp, bufsize, offset, sizeof(terminator), "STAT cmd_arith %" PRINTF_INT64_MODIFIER "u\r\n", stats.arith_cmds);
        offset = append_to_buffer(temp, bufsize, offset, sizeof(terminator), "STAT arith_hits %" PRINTF_INT64_MODIFIER "u\r\n", stats.arith_hits);
        offset = append_to_buffer(temp, bufsize, offset, sizeof(terminator), "STAT hit_rate %g%%\r\n", (stats.get_hits + stats.get_misses) == 0 ? 0.0 : (double)stats.get_hits * 100 / (stats.get_hits + stats.get_misses));
        offset = append_to_buffer(temp, bufsize, offset, sizeof(terminator), "STAT evictions %" PRINTF_INT64_MODIFIER "u\r\n", stats.evictions);
        offset = append_to_buffer(temp, bufsize, offset, sizeof(terminator), "STAT bytes_read %" PRINTF_INT64_MODIFIER "u\r\n", stats.bytes_read);
        offset = append_to_buffer(temp, bufsize, offset, sizeof(terminator), "STAT bytes_written %" PRINTF_INT64_MODIFIER "u\r\n", stats.bytes_written);
        offset = append_to_buffer(temp, bufsize, offset, sizeof(terminator), "STAT limit_maxbytes %lu\r\n", settings.maxbytes);
        offset = append_to_buffer(temp, bufsize, offset, sizeof(terminator), "STAT get_bytes %" PRINTF_INT64_MODIFIER "u\r\n", stats.get_bytes);
        offset = append_to_buffer(temp, bufsize, offset, sizeof(terminator), "STAT byte_seconds %" PRINTF_INT64_MODIFIER "u\r\n", stats.byte_seconds);
        offset = append_to_buffer(temp, bufsize, offset, sizeof(terminator), "STAT threads %u\r\n", settings.num_threads);
        offset = append_thread_stats(temp, bufsize, offset, sizeof(terminator));
#if defined(USE_SLAB_ALLOCATOR)
        offset = append_to_buffer(temp, bufsize, offset, sizeof(terminator), "STAT slabs_rebalance %d\r\n", slabs_get_rebalance_interval());
#endif /* #if defined(USE_SLAB_ALLOCATOR) */
        offset = append_to_buffer(temp, bufsize, offset, 0, terminator);
        out_string(c, temp);
        return;
    }

    subcommand = tokens[SUBCOMMAND_TOKEN].value;

    if (strcmp(subcommand, "reset") == 0) {
        stats_reset();
        out_string(c, "RESET");
        return;
    }

#ifdef HAVE_MALLOC_H
#ifdef HAVE_STRUCT_MALLINFO
    if (strcmp(subcommand, "malloc") == 0) {
        size_t bufsize = 512, offset = 0;
        char temp[bufsize];
        char terminator[] = "END";
        struct mallinfo info;

        info = mallinfo();
        offset = append_to_buffer(temp, bufsize, offset, sizeof(terminator), "STAT arena_size %d\r\n", info.arena);
        offset = append_to_buffer(temp, bufsize, offset, sizeof(terminator), "STAT free_chunks %d\r\n", info.ordblks);
        offset = append_to_buffer(temp, bufsize, offset, sizeof(terminator), "STAT fastbin_blocks %d\r\n", info.smblks);
        offset = append_to_buffer(temp, bufsize, offset, sizeof(terminator), "STAT mmapped_regions %d\r\n", info.hblks);
        offset = append_to_buffer(temp, bufsize, offset, sizeof(terminator), "STAT mmapped_space %d\r\n", info.hblkhd);
        offset = append_to_buffer(temp, bufsize, offset, sizeof(terminator), "STAT max_total_alloc %d\r\n", info.usmblks);
        offset = append_to_buffer(temp, bufsize, offset, sizeof(terminator), "STAT fastbin_space %d\r\n", info.fsmblks);
        offset = append_to_buffer(temp, bufsize, offset, sizeof(terminator), "STAT total_alloc %d\r\n", info.uordblks);
        offset = append_to_buffer(temp, bufsize, offset, sizeof(terminator), "STAT total_free %d\r\n", info.fordblks);
        offset = append_to_buffer(temp, bufsize, offset, sizeof(terminator), "STAT releasable_space %d\r\n", info.keepcost);
        offset = append_to_buffer(temp, bufsize, offset, 0, terminator);
        out_string(c, temp);
        return;
    }
#endif /* HAVE_STRUCT_MALLINFO */
#endif /* HAVE_MALLOC_H */

#if !defined(WIN32) || !defined(__APPLE__)
    if (strcmp(subcommand, "maps") == 0) {
        char *wbuf;
        int wsize = 8192; /* should be enough */
        int res;

        if ((wbuf = (char *)malloc(wsize)) == NULL) {
            out_string(c, "SERVER_ERROR out of memory");
            return;
        }

        if (maps_fd == -1) {
            out_string(c, "SERVER_ERROR cannot open the maps file");
            free(wbuf);
            return;
        }

        // rewind the maps fd because previous calls would have advanced the
        // file pointer.
        lseek(maps_fd, 0, SEEK_SET);

        res = read(maps_fd, wbuf, wsize - 6);  /* 6 = END\r\n\0 */
        if (res == wsize - 6) {
            out_string(c, "SERVER_ERROR buffer overflow");
            free(wbuf);
            return;
        }
        if (res == 0 || res == -1) {
            out_string(c, "SERVER_ERROR can't read the maps file");
            free(wbuf);
            return;
        }
        memcpy(wbuf + res, "END\r\n", 5);
        write_and_free(c, wbuf, res + 5);
        return;
    }
#endif

    if (strcmp(subcommand, "cachedump") == 0) {
#if defined(USE_SLAB_ALLOCATOR)
        char *buf;
        unsigned int bytes, id, limit = 0;

        if(ntokens < 5) {
            out_string(c, "CLIENT_ERROR bad command line");
            return;
        }

        /* the opengroup spec says that if we care about errno after strtol/strtoul, we have to zero
         * it out beforehard.  see
         * http://www.opengroup.org/onlinepubs/000095399/functions/strtoul.html */
        errno = 0;
        id = strtoul(tokens[2].value, NULL, 10);
        limit = strtoul(tokens[3].value, NULL, 10);

        if(errno == ERANGE) {
            out_string(c, "CLIENT_ERROR bad command line format");
            return;
        }

        buf = item_cachedump(id, limit, &bytes);
        write_and_free(c, buf, bytes);
        return;
#endif /* #if defined(USE_SLAB_ALLOCATOR) */
#if defined(USE_FLAT_ALLOCATOR)
        char *buf;
        unsigned int bytes, limit = 0;
        chunk_type_t chunk_type;

        if(ntokens < 5) {
            out_string(c, "CLIENT_ERROR bad command line");
            return;
        }

        if (strcmp("large", tokens[2].value) == 0) {
            chunk_type = LARGE_CHUNK;
        } else if (strcmp("small", tokens[2].value) == 0) {
            chunk_type = SMALL_CHUNK;
        } else {
            out_string(c, "CLIENT_ERROR bad command line");
            return;
        }
        limit = strtoul(tokens[3].value, NULL, 10);

        if(errno == ERANGE) {
            out_string(c, "CLIENT_ERROR bad command line format");
            return;
        }

        buf = item_cachedump(chunk_type, limit, &bytes);
        write_and_free(c, buf, bytes);
        return;
#endif /* #if defined(USE_FLAT_ALLOCATOR) */
    }

#if defined(USE_SLAB_ALLOCATOR)
    if (strcmp(subcommand, "slabs") == 0) {
        int bytes = 0;
        char *buf = slabs_stats(&bytes);
        write_and_free(c, buf, bytes);
        return;
    }
#endif /* #if defined(USE_SLAB_ALLOCATOR) */

#if defined(USE_SLAB_ALLOCATOR)
    if (strcmp(subcommand, "items") == 0) {
        int bytes = 0;
        char *buf = item_stats(&bytes);
        write_and_free(c, buf, bytes);
        return;
    }
#endif /* #if defined(USE_SLAB_ALLOCATOR) */

#if defined(USE_FLAT_ALLOCATOR)
    if (strcmp(subcommand, "flat_allocator") == 0) {
        size_t bytes = 0;
        char* buf = flat_allocator_stats(&bytes);

        write_and_free(c, buf, bytes);
        return;
    }
#endif /* #if defined(USE_FLAT_ALLOCATOR) */

    if (strcmp(subcommand, "detail") == 0) {
        if (ntokens < 4)
            process_stats_detail(c, "");  /* outputs the error message */
        else
            process_stats_detail(c, tokens[2].value);
        return;
    }

    if (strcmp(subcommand, "sizes") == 0) {
        int bytes = 0;
        char *buf = item_stats_sizes(&bytes);
        write_and_free(c, buf, bytes);
        return;
    }

    if (strcmp(subcommand, "buckets") == 0) {
        int bytes = 0;
        char *buf = item_stats_buckets(&bytes);
        write_and_free(c, buf, bytes);
        return;
    }

    if (strcmp(subcommand, "pools") == 0) {
        size_t bufsize = 2048, offset = 0;
        char temp[bufsize];
        char terminator[] = "END";

#define MEMORY_POOL(pool_enum, pool_counter, pool_string) \
        offset = append_to_buffer(temp, bufsize, offset, sizeof(terminator), "STAT " pool_string " %" PRINTF_INT64_MODIFIER "u\r\n", stats.pool_counter);
#include "memory_pool_classes.h"
#if defined(MEMORY_POOL_CHECKS)
#if defined(MEMORY_POOL_ERROR_BREAKDOWN)
#define MEMORY_POOL(pool_enum, pool_counter, pool_string) \
        offset = append_to_buffer(temp, bufsize, offset, sizeof(terminator), "STAT free_errors_" pool_string " %" PRINTF_INT64_MODIFIER "u\r\n", stats.mp_bytecount_errors_free_split.pool_counter);
#include "memory_pool_classes.h"
#define MEMORY_POOL(pool_enum, pool_counter, pool_string) \
        offset = append_to_buffer(temp, bufsize, offset, sizeof(terminator), "STAT realloc_errors_" pool_string " %" PRINTF_INT64_MODIFIER "u\r\n", stats.mp_bytecount_errors_realloc_split.pool_counter);
#include "memory_pool_classes.h"
#endif /* #if defined(MEMORY_POOL_ERROR_BREAKDOWN) */
        offset = append_to_buffer(temp, bufsize, offset, sizeof(terminator), "STAT blk_errors %" PRINTF_INT64_MODIFIER "u\r\n", stats.mp_blk_errors);
        offset = append_to_buffer(temp, bufsize, offset, sizeof(terminator), "STAT bytecount_errors %" PRINTF_INT64_MODIFIER "u\r\n", stats.mp_bytecount_errors);
        offset = append_to_buffer(temp, bufsize, offset, sizeof(terminator), "STAT pool_errors %" PRINTF_INT64_MODIFIER "u\r\n", stats.mp_pool_errors);
#endif /* #if defined(MEMORY_POOL_CHECKS) */
        offset = append_to_buffer(temp, bufsize, offset, 0, terminator);
        out_string(c, temp);
        return;
    }

    if (strcmp(subcommand, "cost-benefit") == 0) {
        int bytes = 0;
        char *buf = cost_benefit_stats(&bytes);
        write_and_free(c, buf, bytes);
        return;
    }

    if (strcmp(subcommand, "conn_buffer") == 0) {
        size_t bytes = 0;
        char* buf = conn_buffer_stats(&bytes);

        write_and_free(c, buf, bytes);
        return;
    }

    out_string(c, "ERROR");
}


/*
 * given a set of tokens, which may not be fully tokenized (see
 * tokenize_command(..)), count the number of tokens.
 */
static size_t count_total_tokens(const token_t* const tokens)
{
    const token_t* key_token = &tokens[KEY_TOKEN];
    int count = 0;

    /* count already tokenized keys */
    while (key_token->length != 0) {
        key_token ++;
        count ++;
    }

    /* scan untokenized keys */
    if (key_token->value != NULL) {
        const char* iterator = key_token->value;

        while (*iterator != 0) {
            if (*iterator == ' ') {
                count ++;
            }
            iterator ++;
        }
    }

    return count;
}


/*
 * ensure that the buffer managed by the tuple (buf, curr, size, bytes) have
 * enough capacity to hold req_bytes of data.
 *
 * @param buf   points to the start of the buffer.
 * @param curr  points to where the buffer will be written to next.
 * @param size  the size of the buffer.
 * @param bytes the number of bytes consumed so far.
 */
static int ensure_wbuf(conn* const c, const size_t req_bytes)
{
    char* newbuf;
    size_t new_size;

    if (req_bytes <= (c->wsize - c->wbytes)) {
        return 0;
    }

    new_size = req_bytes + c->wbytes;
    if ((newbuf = pool_realloc(c->wbuf, new_size, c->wsize, CONN_BUFFER_WBUF_POOL)) == NULL) {
        /* error... */
        return -1;
    }

    /* size needs to be adjusted. */
    c->wsize = new_size;

    /* if the new location is the same, we don't need to adjust anything else. */
    if (newbuf == c->wbuf) {
        return 0;
    }

    /* adjust the pointers. */
    c->wbuf = newbuf;
    c->wcurr = newbuf + c->wbytes;

    return 0;
}


#define FLAGS_LENGTH_STRING_LEN (sizeof(" 4xxxyyyzzz 1xxxyyy\r\n") - 1)


/* ntokens is overwritten here... shrug.. */
static inline void process_get_command(conn* c, token_t *tokens, size_t ntokens) {
    stats_t *stats = STATS_GET_TLS();
    char *key;
    size_t nkey;
    int i = 0;
    item *it;
    token_t *key_token = &tokens[KEY_TOKEN];
    size_t token_count;

    assert(c != NULL);

    if (settings.managed) {
        int bucket = c->bucket;
        if (bucket == -1) {
            out_string(c, "CLIENT_ERROR no BG data in managed mode");
            return;
        }
        c->bucket = -1;
        if (buckets[bucket] != c->gen) {
            out_string(c, "ERROR_NOT_OWNER");
            return;
        }
    }

    /*
     * count the number of tokens, and ensure that we have enough space at
     * c->wbuf to hold all the " flags length\r\n" that we might transmit.
     */
    token_count = count_total_tokens(tokens);
    assert(c->wbytes == 0);             // there should be no one using the wbuf
                                        // at this point.

    /* ensure we have enough spaces for each of the flags + length strings, plus
     * a null terminator at the very end (artifact of using sprintf, we will not
     * send the null) */
    if (ensure_wbuf(c, (token_count * FLAGS_LENGTH_STRING_LEN) + 1)) {
        out_string(c, "SERVER_ERROR cannot allocate sufficient memory");
    }

    do {
        while(key_token->length != 0) {

            key = key_token->value;
            nkey = key_token->length;

            if(nkey > KEY_MAX_LENGTH) {
                out_string(c, "CLIENT_ERROR bad command line format");
                return;
            }

            it = item_get(key, nkey);

            STATS_LOCK(stats);
            stats->get_cmds++;
            stats->get_bytes += (NULL != it) ? ITEM_nbytes(it) : 0;
            STATS_UNLOCK(stats);

            if (settings.detail_enabled) {
                stats_prefix_record_get(key, nkey, (NULL != it) ? ITEM_nbytes(it) : 0, NULL != it);
            }

            if (it) {
                char* flags_len_string_start;
                ssize_t flags_len_string_len;

                if (i >= c->isize) {
                    item **new_list = pool_realloc(c->ilist, sizeof(item *) * c->isize * 2,
                                                   sizeof(item*) * c->isize, CONN_BUFFER_ILIST_POOL);
                    if (new_list) {
                        c->isize *= 2;
                        c->ilist = new_list;
                    } else break;
                }

                /* write flags + length to the buffer. */
                assert(c->wsize - c->wbytes >= FLAGS_LENGTH_STRING_LEN + 1);

                flags_len_string_start = c->wcurr;
                flags_len_string_len = snprintf(c->wcurr, FLAGS_LENGTH_STRING_LEN + 1,
                                                " %u %u\r\n", ITEM_flags(it),
                                                (unsigned int) (ITEM_nbytes(it)));
                c->wcurr += flags_len_string_len;
                c->wbytes += flags_len_string_len;

                /*
                 * Construct the response. Each hit adds three elements to the
                 * outgoing data list:
                 *   "VALUE "
                 *   key
                 *   " " + flags + " " + data length + "\r\n" + data (with \r\n)
                 */
                if (add_iov(c, "VALUE ", 6, true) != 0 ||
                    add_item_key_to_iov(c, it) != 0 ||
                    add_iov(c, flags_len_string_start, flags_len_string_len, false) != 0 ||
                    add_item_value_to_iov(c, it, true /* send cr-lf */) != 0)
                    {
                        break;
                    }
                if (settings.verbose > 1) {
                    fprintf(stderr, ">%d sending key %*s\n", c->sfd, (int) nkey, key);
                }

                /* item_get() has incremented it->refcount for us */
                STATS_LOCK(stats);
                stats->get_hits++;
                STATS_UNLOCK(stats);

                stats_get(ITEM_nkey(it) + ITEM_nbytes(it));
                item_update(it);
#if defined(USE_SLAB_ALLOCATOR)
                item_mark_visited(it);
#endif /* #if defined(USE_SLAB_ALLOCATOR) */
                *(c->ilist + i) = it;
                i++;

            } else {
                STATS_LOCK(stats);
                stats->get_misses++;
                STATS_UNLOCK(stats);
            }

            key_token++;
        }
        /*
         * If the command string hasn't been fully processed, get the next set
         * of tokens.
         */
        if(key_token->value != NULL) {
            ntokens = tokenize_command(key_token->value, tokens, MAX_TOKENS);
            key_token = tokens;
        }

    } while(key_token->value != NULL);

    c->icurr = c->ilist;
    c->ileft = i;

    if (settings.verbose > 1)
        fprintf(stderr, ">%d END\n", c->sfd);
    add_iov(c, "END\r\n", 5, true);

    if (c->udp && build_udp_headers(c) != 0) {
        out_string(c, "SERVER_ERROR out of memory");
    }
    else {
        conn_set_state(c, conn_mwrite);
        c->msgcurr = 0;
    }
    return;
}

/* ntokens is overwritten here... shrug.. */
static inline void process_metaget_command(conn *c, token_t *tokens, size_t ntokens) {
    char *key;
    size_t nkey;
    item *it;
    token_t *key_token = &tokens[KEY_TOKEN];

    assert(c != NULL);

    key = key_token->value;
    nkey = key_token->length;

    if(nkey > KEY_MAX_LENGTH) {
        out_string(c, "CLIENT_ERROR bad command line format");
        return;
    }

    it = item_get(key, nkey);
    if (it) {
        ssize_t written, avail = c->wsize - c->wbytes;
        char* txstart;
        size_t txcount;
        rel_time_t now = current_time;
        char* ip_addr_str;
        char* age_str;
        char scratch[20];
        size_t offset = 0;

        txstart = c->wcurr;

        if (ITEM_has_timestamp(it)) {
            rel_time_t timestamp;

            item_memcpy_from(&timestamp, it, ITEM_nbytes(it) + offset, sizeof(timestamp), true);
            snprintf(scratch, 20, "%d", (now - timestamp));
            age_str = scratch;
            offset += sizeof(timestamp);
        } else {
            age_str = "unknown";
        }

        if (ITEM_has_ip_address(it)) {
            struct in_addr in;

            item_memcpy_from(&in, it, ITEM_nbytes(it) + offset, sizeof(in), true);
            ip_addr_str = inet_ntoa(in);
            offset += sizeof(in);
        } else {
            ip_addr_str = "unknown";
        }

        written = snprintf(c->wcurr, avail,
                           " age: %s; exptime: %d; from: %s\r\n", age_str, ITEM_exptime(it), ip_addr_str);

        if (written > avail) {
            txcount = avail;
        } else if (written == -1) {
            txcount = 0;
        } else {
            txcount = written;
        }

        if (add_iov(c, "META ", 5, true) == 0 &&
            add_item_key_to_iov(c, it) == 0 &&
            add_iov(c, txstart, txcount, false) == 0) {
            if (settings.verbose > 1) {
                fprintf(stderr, ">%d sending metadata for key %*s\n", c->sfd, (int) nkey, key);
            }
        }

        item_deref(it);
    }

    if (add_iov(c, "END\r\n", 5, false) != 0 ||
        (c->udp &&
         build_udp_headers(c) != 0)) {
        out_string(c, "SERVER_ERROR out of memory");
    } else {
        conn_set_state(c, conn_mwrite);
        c->msgcurr = 0;
    }
    return;
}

static void process_update_command(conn *c, token_t *tokens, const size_t ntokens, int comm) {
    char *key;
    size_t nkey;
    int flags;
    time_t exptime;
    int vlen;
    item *it;

    assert(c != NULL);

    if (tokens[KEY_TOKEN].length > KEY_MAX_LENGTH) {
        out_string(c, "CLIENT_ERROR bad command line format");
        return;
    }

    key = tokens[KEY_TOKEN].value;
    nkey = tokens[KEY_TOKEN].length;

    /* the opengroup spec says that if we care about errno after strtol/strtoul, we have to zero it
     * out beforehard.  see http://www.opengroup.org/onlinepubs/000095399/functions/strtoul.html */
    errno = 0;
    flags = strtoul(tokens[2].value, NULL, 10);
    exptime = strtol(tokens[3].value, NULL, 10);
    vlen = strtol(tokens[4].value, NULL, 10);

    if(errno == ERANGE || ((flags == 0 || exptime == 0) && errno == EINVAL)) {
        out_string(c, "CLIENT_ERROR bad command line format");
        return;
    }

    if (settings.detail_enabled) {
        stats_prefix_record_set(key, nkey);
    }

    if (settings.managed) {
        int bucket = c->bucket;
        if (bucket == -1) {
            out_string(c, "CLIENT_ERROR no BG data in managed mode");
            return;
        }
        c->bucket = -1;
        if (buckets[bucket] != c->gen) {
            out_string(c, "ERROR_NOT_OWNER");
            return;
        }
    }

    it = item_alloc(key, nkey, flags, realtime(exptime), vlen,
                    get_request_addr(c));

    if (it == 0 ||
        item_setup_receive(it, c) == false) {
        if (! item_size_ok(nkey, flags, vlen))
            out_string(c, "SERVER_ERROR object too large for cache");
        else
            out_string(c, "SERVER_ERROR out of memory");
        /* swallow the data line */
        c->write_and_go = conn_swallow;
        c->sbytes = vlen + 2;
        return;
    }

    memset(c->crlf, 0, sizeof(c->crlf)); /* clear out the previous CR-LF so when
                                          * we get to complete_nread and check
                                          * for the CR-LF, we're sure that we're
                                          * not reading stale data. */

    c->update_key = key;
    c->item_comm = comm;
    c->item = it;
    conn_set_state(c, conn_nread);
}

static void process_arithmetic_command(conn* c, token_t *tokens, const size_t ntokens, const int incr) {
    char temp[32];
    unsigned int delta;
    char *key;
    size_t nkey;

    assert(c != NULL);

    if(tokens[KEY_TOKEN].length > KEY_MAX_LENGTH) {
        out_string(c, "CLIENT_ERROR bad command line format");
        return;
    }

    key = tokens[KEY_TOKEN].value;
    nkey = tokens[KEY_TOKEN].length;

    if (settings.managed) {
        int bucket = c->bucket;
        if (bucket == -1) {
            out_string(c, "CLIENT_ERROR no BG data in managed mode");
            return;
        }
        c->bucket = -1;
        if (buckets[bucket] != c->gen) {
            out_string(c, "ERROR_NOT_OWNER");
            return;
        }
    }

    /* the opengroup spec says that if we care about errno after strtol/strtoul, we have to zero it
     * out beforehard.  see http://www.opengroup.org/onlinepubs/000095399/functions/strtoul.html */
    errno = 0;
    delta = strtoul(tokens[2].value, NULL, 10);

    if(errno == ERANGE) {
        out_string(c, "CLIENT_ERROR bad command line format");
        return;
    }

    out_string(c, add_delta(key, nkey, incr, delta, temp, NULL, get_request_addr(c)));
}

/*
 * adds a delta value to a numeric item.
 *
 * it    item to adjust
 * incr  true to increment value, false to decrement
 * delta amount to adjust value by
 * buf   buffer for response string
 *
 * returns a response string to send back to the client.
 */
char *do_add_delta(const char* key, const size_t nkey, const int incr, const unsigned int delta,
                   char *buf, uint32_t* res_val, const struct in_addr addr) {
    stats_t *stats = STATS_GET_TLS();
    uint32_t value;
    int res;
    rel_time_t now;
    item* it;

    it = do_item_get_notedeleted(key, nkey, NULL);
    if (!it) {
        STATS_LOCK(stats);
        stats->arith_cmds ++;
        STATS_UNLOCK(stats);
        if (settings.detail_enabled) {
            stats_prefix_record_get(key, nkey, 0, false);
        }
        return "NOT_FOUND";
    }

    now = current_time;

    /* the opengroup spec says that if we care about errno after strtol/strtoul, we have to zero it
     * out beforehard.  see http://www.opengroup.org/onlinepubs/000095399/functions/strtoul.html */
    errno = 0;
    value = item_strtoul(it, 10);

    if (incr != 0)
        value += delta;
    else {
        if (delta >= value) value = 0;
        else value -= delta;
    }
    if (res_val) {
        *res_val = value;
    }
    snprintf(buf, 32, "%u", value);
    res = strlen(buf);
    assert(ITEM_refcount(it) >= 1);

    // arithmetic operations are essentially a set+get operation.
    STATS_LOCK(stats);
    stats->arith_cmds ++;
    stats->arith_hits ++;
    stats->get_bytes += res;
    STATS_UNLOCK(stats);
    stats_set(ITEM_nkey(it) + res, ITEM_nkey(it) + ITEM_nbytes(it));
    stats_get(ITEM_nkey(it) + res);
    if (settings.detail_enabled) {
        stats_prefix_record_set(key, nkey);
        stats_prefix_record_get(key, nkey, res, true);
        if (res != ITEM_nbytes(it)) {
            stats_prefix_record_byte_total_change(key, nkey, res - ITEM_nbytes(it), PREFIX_IS_OVERWRITE);
        }
    }

    if (item_need_realloc(it, ITEM_nkey(it), ITEM_flags(it), res) ||
        ITEM_refcount(it) > 1) {
        /* need to realloc */
        item *new_it;

        if (settings.detail_enabled) {
            /* because we're replacing an item, we need to bump the item count and
             * re-add the byte count of the item block we're evicting.. */
            stats_prefix_record_byte_total_change(key, nkey, ITEM_nkey(it) + ITEM_nbytes(it), PREFIX_INCR_ITEM_COUNT);
        }

        new_it = do_item_alloc(key, nkey,
                               ITEM_flags(it), ITEM_exptime(it),
                               res, addr);
        if (new_it == 0) {
            do_item_deref(it);
            return "SERVER_ERROR out of memory";
        }
        item_memcpy_to(new_it, 0, buf, res, false);
        do_item_replace(it, new_it, key);
        do_item_deref(new_it);       /* release our reference */
    } else { /* replace in-place */
        ITEM_set_nbytes(it, res);               /* update the length field. */
        item_memcpy_to(it, 0, buf, res, false);
        do_item_update(it);

        do_try_item_stamp(it, now, addr);
    }

    do_item_deref(it);
    return buf;
}

static void process_delete_command(conn* c, token_t *tokens, const size_t ntokens) {
    char *key;
    size_t nkey;
    item *it;
    time_t exptime = 0;

    assert(c != NULL);

    if (settings.managed) {
        int bucket = c->bucket;
        if (bucket == -1) {
            out_string(c, "CLIENT_ERROR no BG data in managed mode");
            return;
        }
        c->bucket = -1;
        if (buckets[bucket] != c->gen) {
            out_string(c, "ERROR_NOT_OWNER");
            return;
        }
    }

    key = tokens[KEY_TOKEN].value;
    nkey = tokens[KEY_TOKEN].length;

    if(nkey > KEY_MAX_LENGTH) {
        out_string(c, "CLIENT_ERROR bad command line format");
        return;
    }

    if(ntokens == 4) {
        /* the opengroup spec says that if we care about errno after strtol/strtoul, we have to zero
         * it out beforehard.  see
         * http://www.opengroup.org/onlinepubs/000095399/functions/strtoul.html */
        errno = 0;
        exptime = strtol(tokens[2].value, NULL, 10);

        if(errno == ERANGE) {
            out_string(c, "CLIENT_ERROR bad command line format");
            return;
        }
    }

    if (settings.detail_enabled) {
        stats_prefix_record_delete(key, nkey);
    }

    it = item_get(key, nkey);
    if (it) {
        if (exptime == 0) {
            stats_delete(ITEM_nkey(it) + ITEM_nbytes(it));

            item_unlink(it, UNLINK_NORMAL, key);
            item_deref(it);      /* release our reference */
            out_string(c, "DELETED");
        } else {
            /* our reference will be transfered to the delete queue */
            switch (defer_delete(it, exptime)) {
                case 0:
                    out_string(c, "DELETED");
                    break;

                case -1:
                    out_string(c, "SERVER_ERROR out of memory");
                    break;

                default:
                    assert(0);
            }
        }
    } else {
        out_string(c, "NOT_FOUND");
    }
}

/*
 * Adds an item to the deferred-delete list so it can be reaped later.
 *
 * Returns 0 if successfully deleted, -1 if there is a memory allocation error.
 */
int do_defer_delete(item *it, time_t exptime)
{
    if (delcurr >= deltotal) {
        item **new_delete = pool_realloc(todelete, sizeof(item *) * deltotal * 2,
                                         sizeof(item*) * deltotal, DELETE_POOL);
        if (new_delete) {
            todelete = new_delete;
            deltotal *= 2;
        } else {
            /*
             * can't delete it immediately, user wants a delay,
             * but we ran out of memory for the delete queue
             */
            item_deref(it);    /* release reference */
            return -1;
        }
    }

    /* use its expiration time as its deletion time now */
    ITEM_set_exptime(it, realtime(exptime));
    ITEM_mark_deleted(it);
    todelete[delcurr++] = it;

    return 0;
}

static void process_verbosity_command(conn* c, token_t *tokens, const size_t ntokens) {
    unsigned int level;

    assert(c != NULL);

    level = strtoul(tokens[1].value, NULL, 10);
    settings.verbose = level > MAX_VERBOSITY_LEVEL ? MAX_VERBOSITY_LEVEL : level;
    out_string(c, "OK");
    return;
}

static void process_command(conn* c, char *command) {

    token_t tokens[MAX_TOKENS];
    size_t ntokens;
    int comm;

    assert(c != NULL);

    if (settings.verbose > 1)
        fprintf(stderr, "<%d %s\n", c->sfd, command);

    /* ensure that conn_set_state going into the conn_read state cleared the
     * c->msg* and c->iov* counters.
     */
    assert(c->msgcurr == 0);
    assert(c->msgused == 0);
    assert(c->iovused == 0);

    if (add_msghdr(c) != 0) {
        /* if we can't allocate the msghdr, we can't really send the error
         * message.  so just close the connection. */
        conn_set_state(c, conn_closing);
        return;
    }

    ntokens = tokenize_command(command, tokens, MAX_TOKENS);

    if (ntokens >= 3 &&
        ((strcmp(tokens[COMMAND_TOKEN].value, "get") == 0) ||
         (strcmp(tokens[COMMAND_TOKEN].value, "bget") == 0))) {

        process_get_command(c, tokens, ntokens);

    } else if (ntokens == 3 &&
               (strcmp(tokens[COMMAND_TOKEN].value, "metaget") == 0)) {

        process_metaget_command(c, tokens, ntokens);

    } else if (ntokens == 6 &&
               ((strcmp(tokens[COMMAND_TOKEN].value, "add") == 0 && (comm = NREAD_ADD)) ||
                (strcmp(tokens[COMMAND_TOKEN].value, "set") == 0 && (comm = NREAD_SET)) ||
                (strcmp(tokens[COMMAND_TOKEN].value, "replace") == 0 && (comm = NREAD_REPLACE)))) {

        process_update_command(c, tokens, ntokens, comm);

    } else if (ntokens == 4 && (strcmp(tokens[COMMAND_TOKEN].value, "incr") == 0)) {

        process_arithmetic_command(c, tokens, ntokens, 1);

    } else if (ntokens == 4 && (strcmp(tokens[COMMAND_TOKEN].value, "decr") == 0)) {

        process_arithmetic_command(c, tokens, ntokens, 0);

    } else if (ntokens >= 3 && ntokens <= 4 && (strcmp(tokens[COMMAND_TOKEN].value, "delete") == 0)) {

        process_delete_command(c, tokens, ntokens);

    } else if (ntokens == 3 && strcmp(tokens[COMMAND_TOKEN].value, "own") == 0) {
        unsigned int bucket, gen;
        if (!settings.managed) {
            out_string(c, "CLIENT_ERROR not a managed instance");
            return;
        }

        if (sscanf(tokens[1].value, "%u:%u", &bucket,&gen) == 2) {
            if ((bucket < 0) || (bucket >= MAX_BUCKETS)) {
                out_string(c, "CLIENT_ERROR bucket number out of range");
                return;
            }
            buckets[bucket] = gen;
            out_string(c, "OWNED");
            return;
        } else {
            out_string(c, "CLIENT_ERROR bad format");
            return;
        }

    } else if (ntokens == 3 && (strcmp(tokens[COMMAND_TOKEN].value, "disown")) == 0) {

        int bucket;
        if (!settings.managed) {
            out_string(c, "CLIENT_ERROR not a managed instance");
            return;
        }
        if (sscanf(tokens[1].value, "%u", &bucket) == 1) {
            if ((bucket < 0) || (bucket >= MAX_BUCKETS)) {
                out_string(c, "CLIENT_ERROR bucket number out of range");
                return;
            }
            buckets[bucket] = 0;
            out_string(c, "DISOWNED");
            return;
        } else {
            out_string(c, "CLIENT_ERROR bad format");
            return;
        }

    } else if (ntokens == 3 && (strcmp(tokens[COMMAND_TOKEN].value, "bg")) == 0) {
        int bucket, gen;
        if (!settings.managed) {
            out_string(c, "CLIENT_ERROR not a managed instance");
            return;
        }
        if (sscanf(tokens[1].value, "%u:%u", &bucket, &gen) == 2) {
            /* we never write anything back, even if input's wrong */
            if ((bucket < 0) || (bucket >= MAX_BUCKETS) || (gen <= 0)) {
                /* do nothing, bad input */
            } else {
                c->bucket = bucket;
                c->gen = gen;
            }
            conn_set_state(c, conn_read);
            return;
        } else {
            out_string(c, "CLIENT_ERROR bad format");
            return;
        }

    } else if (ntokens >= 2 && (strcmp(tokens[COMMAND_TOKEN].value, "stats") == 0)) {

        process_stat(c, tokens, ntokens);

    } else if (ntokens >= 2 && ntokens <= 3 && (strcmp(tokens[COMMAND_TOKEN].value, "flush_all") == 0)) {
        time_t exptime = 0;
        set_current_time();

        if(ntokens == 2) {
            settings.oldest_live = current_time - 1;
            item_flush_expired();
            out_string(c, "OK");
            return;
        }

        /* the opengroup spec says that if we care about errno after strtol/strtoul, we have to zero
         * it out beforehard.  see
         * http://www.opengroup.org/onlinepubs/000095399/functions/strtoul.html */
        errno = 0;
        exptime = strtol(tokens[1].value, NULL, 10);
        if(errno == ERANGE) {
            out_string(c, "CLIENT_ERROR bad command line format");
            return;
        }

        settings.oldest_live = realtime(exptime) - 1;
        item_flush_expired();
        out_string(c, "OK");
        return;

    } else if (ntokens == 2 && (strcmp(tokens[COMMAND_TOKEN].value, "version") == 0)) {

        out_string(c, "VERSION " VERSION);

    } else if (ntokens == 2 && (strcmp(tokens[COMMAND_TOKEN].value, "quit") == 0)) {

        conn_set_state(c, conn_closing);

#if defined(USE_SLAB_ALLOCATOR)
    } else if (ntokens == 5 && (strcmp(tokens[COMMAND_TOKEN].value, "slabs") == 0 &&
                                strcmp(tokens[COMMAND_TOKEN + 1].value, "reassign") == 0)) {

        int src, dst, rv;

        /* the opengroup spec says that if we care about errno after strtol/strtoul, we have to zero
         * it out beforehard.  see
         * http://www.opengroup.org/onlinepubs/000095399/functions/strtoul.html */
        errno = 0;
        src = strtol(tokens[2].value, NULL, 10);
        dst  = strtol(tokens[3].value, NULL, 10);

        if(errno == ERANGE) {
            out_string(c, "CLIENT_ERROR bad command line format");
            return;
        }

        rv = slabs_reassign(src, dst);
        if (rv == 1) {
            out_string(c, "DONE");
            return;
        }
        if (rv == 0) {
            out_string(c, "CANT");
            return;
        }
        if (rv == -1) {
            out_string(c, "BUSY");
            return;
        }

    } else if (ntokens == 4 && (strcmp(tokens[COMMAND_TOKEN].value, "slabs") == 0 &&
                                strcmp(tokens[COMMAND_TOKEN + 1].value, "rebalance") == 0)) {

        int interval;

        /* the opengroup spec says that if we care about errno after strtol/strtoul, we have to zero
         * it out beforehard.  see
         * http://www.opengroup.org/onlinepubs/000095399/functions/strtoul.html */
        errno = 0;
        interval = strtol(tokens[2].value, NULL, 10);
        if (errno == ERANGE) {
            out_string(c, "CLIENT_ERROR bad command line format");
            return;
        }
        slabs_set_rebalance_interval(interval);
        out_string(c, "INTERVAL RESET");
        return;

#endif /* #if defined(USE_SLAB_ALLOCATOR) */
    } else if (ntokens == 3 && (strcmp(tokens[COMMAND_TOKEN].value, "flush_regex") == 0)) {
        if (assoc_expire_regex(tokens[COMMAND_TOKEN + 1].value)) {
            out_string(c, "DELETED");
        }
        else {
            out_string(c, "CLIENT_ERROR Bad regular expression (or regex not supported)");
        }
    } else if (ntokens == 3 && (strcmp(tokens[COMMAND_TOKEN].value, "verbosity") == 0)) {
        process_verbosity_command(c, tokens, ntokens);
    } else {
        out_string(c, "ERROR");
    }
    return;
}

/*
 * if we have a complete line in the buffer, process it.
 */
static int try_read_command(conn* c) {
    char *el, *cont;

    assert(c != NULL);

    /* we have no allocated buffers, so we definitely don't have any commands to
     * read. */
    if (c->rbuf == NULL) {
        return 0;
    }

    assert(c->rcurr < (c->rbuf + c->rsize));

    if (c->rbytes == 0)
        return 0;
    el = memchr(c->rcurr, '\n', c->rbytes);
    if (!el)
        return 0;
    cont = el + 1;
    if ((el - c->rcurr) > 1 && *(el - 1) == '\r') {
        el--;
    }
    *el = '\0';

    assert(cont <= (c->rbuf + c->rsize));
    assert(cont <= (c->rcurr + c->rbytes));

    process_command(c, c->rcurr);

    c->rbytes -= (cont - c->rcurr);
    c->rcurr = cont;

    assert(c->rcurr < (c->rbuf + c->rsize));

    return 1;
}

/*
 * read a UDP request.
 * return 0 if there's nothing to read.
 */
int try_read_udp(conn* c) {
    stats_t *stats = STATS_GET_TLS();
    int res;

    assert(c != NULL);
    assert(c->rbytes == 0);

    if (c->rbuf == NULL) {
        /* no idea how big the buffer will need to be. */
        c->rbuf = (char*) alloc_conn_buffer(c->cbg, 0);

        if (c->rbuf != NULL) {
            c->rcurr = c->rbuf;
            c->rsize = CONN_BUFFER_DATA_SZ;
        } else {
            if (c->binary) {
                bp_write_err_msg(c, "out of memory");
            } else {
                out_string(c, "SERVER_ERROR out of memory");
            }
            return 0;
        }
    }

    c->request_addr_size = sizeof(c->request_addr);
    res = recvfrom(c->sfd, c->rbuf, c->rsize,
                   0, &c->request_addr, &c->request_addr_size);
    if (res > 8) {
        unsigned char *buf = (unsigned char *)c->rbuf;
#if defined(HAVE_UDP_REPLY_PORTS)
        uint16_t reply_ports;
#endif
        STATS_LOCK(stats);
        stats->bytes_read += res;
        STATS_UNLOCK(stats);

        /* Beginning of UDP packet is the request ID; save it. */
        c->request_id = buf[0] * 256 + buf[1];

        /* If this is a multi-packet request, drop it. */
        if (buf[4] != 0 || buf[5] != 1) {
            if (c->binary) {
                bp_write_err_msg(c, "multi-packet request not supported");
            } else {
                out_string(c, "SERVER_ERROR multi-packet request not supported");
            }
            return 0;
        }

        /* report peak usage here */
        report_max_rusage(c->cbg, c->rbuf, res);

#if defined(HAVE_UDP_REPLY_PORTS)
        reply_ports = ntohs(*((uint16_t*)(buf + 6)));
        c->xfd = c->ufd;
        /* If the client cannot support the number of reply sockets
           use the receive socket instead.  We check against num_threads
           to account for the entire range of ports in use, including the
           receive port. */
        if (reply_ports < settings.num_threads) {
            c->xfd = c->sfd;
        }
#endif /* defined(HAVE_UDP_REPLY_PORTS) */

        /* Don't care about any of the rest of the header. */
        res -= 8;
        memmove(c->rbuf, c->rbuf + 8, res);

        c->rbytes += res;
        c->rcurr = c->rbuf;
        return 1;
    } else {
        /* return the conn buffer. */
        free_conn_buffer(c->cbg, c->rbuf, 8 - 1 /* worst case for memory usage */);
        c->rbuf = NULL;
        c->rcurr = NULL;
        c->rsize = 0;
    }

    return 0;
}

/*
 * read from network as much as we can, handle buffer overflow and connection
 * close.
 * before reading, move the remaining incomplete fragment of a command
 * (if any) to the beginning of the buffer.
 * return 0 if there's nothing to read on the first read.
 */
int try_read_network(conn* c) {
    stats_t *stats = STATS_GET_TLS();
    int gotdata = 0;
    int res;
    int avail;

    assert(c != NULL);

    if (c->rbuf != NULL) {
        if (c->rcurr != c->rbuf) {
            if (c->rbytes != 0) /* otherwise there's nothing to copy */
                memmove(c->rbuf, c->rcurr, c->rbytes);
            c->rcurr = c->rbuf;
        }
    } else {
        c->rbuf = (char*) alloc_conn_buffer(c->cbg, 0);
        if (c->rbuf != NULL) {
            c->rcurr = c->rbuf;
            c->rsize = CONN_BUFFER_DATA_SZ;
        } else {
            if (c->binary) {
                bp_write_err_msg(c, "out of memory");
            } else {
                out_string(c, "SERVER_ERROR out of memory");
            }
            return 0;
        }
    }

    while (1) {
        avail = c->rsize - c->rbytes;

        res = read(c->sfd, c->rbuf + c->rbytes, avail);
        if (res > 0) {
            STATS_LOCK(stats);
            stats->bytes_read += res;
            STATS_UNLOCK(stats);
            gotdata = 1;
            c->rbytes += res;

            /* report peak usage here */
            report_max_rusage(c->cbg, c->rbuf, c->rbytes);

            if (res < avail) {
                break;
            }
        }
        else if (res == 0) {
            if (c->binary) {
                c->state = conn_closing;
            } else {
                /* connection closed */
                conn_set_state(c, conn_closing);
            }
            return 1;
        }
        else {
            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                /* if we have no data, release the connection buffer */
                if (c->rbytes == 0) {
                    free_conn_buffer(c->cbg, c->rbuf, 0);
                    c->rbuf = NULL;
                    c->rcurr = NULL;
                    c->rsize = 0;
                }
                break;
            }
            else return 0;
        }
    }
    return gotdata;
}

bool update_event(conn* c, const int new_flags) {
    assert(c != NULL);

    struct event_base *base = c->event.ev_base;
    if (c->ev_flags == new_flags)
        return true;
    if (event_del(&c->event) == -1) return false;
    event_set(&c->event, c->sfd, new_flags, event_handler, (void *)c);
    event_base_set(base, &c->event);
    c->ev_flags = new_flags;
    if (event_add(&c->event, 0) == -1) return false;
    return true;
}

/*
 * Sets whether we are listening for new connections or not.
 */
void accept_new_conns(const bool do_accept, const bool binary) {
    conn* conn;
    if (! is_listen_thread())
        return;

    if (binary) {
        conn = listen_binary_conn;
    } else {
        conn = listen_conn;
    }

    if (do_accept) {
        update_event(conn, EV_READ | EV_PERSIST);
        if (listen(conn->sfd, LISTEN_DEPTH) != 0) {
            perror("listen");
        }
    }
    else {
        update_event(conn, 0);
        if (listen(conn->sfd, 0) != 0) {
            perror("listen");
        }
    }
}


/*
 * Transmit the next chunk of data from our list of msgbuf structures.
 *
 * Returns:
 *   TRANSMIT_COMPLETE   All done writing.
 *   TRANSMIT_INCOMPLETE More data remaining to write.
 *   TRANSMIT_SOFT_ERROR Can't write any more right now.
 *   TRANSMIT_HARD_ERROR Can't write (c->state is set to conn_closing)
 */
int transmit(conn* c) {
    stats_t *stats = STATS_GET_TLS();
    assert(c != NULL);

    if (c->msgcurr < c->msgused &&
            c->msglist[c->msgcurr].msg_iovlen == 0) {
        /* Finished writing the current msg; advance to the next. */
        c->msgcurr++;
    }
    if (c->msgcurr < c->msgused) {
        ssize_t res;
        struct msghdr *m = &c->msglist[c->msgcurr];

        res = sendmsg(c->xfd, m, 0);
        if (res > 0) {
            STATS_LOCK(stats);
            stats->bytes_written += res;
            STATS_UNLOCK(stats);

            /* We've written some of the data. Remove the completed
               iovec entries from the list of pending writes. */
            while (m->msg_iovlen > 0 && res >= m->msg_iov->iov_len) {
                res -= m->msg_iov->iov_len;
                m->msg_iovlen--;
                m->msg_iov++;
            }

            /* Might have written just part of the last iovec entry;
               adjust it so the next write will do the rest. */
            if (res > 0) {
                m->msg_iov->iov_base += res;
                m->msg_iov->iov_len -= res;
            }
            return TRANSMIT_INCOMPLETE;
        }
        if (res == -1 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
            if (!update_event(c, EV_WRITE | EV_PERSIST)) {
                if (settings.verbose > 0)
                    fprintf(stderr, "Couldn't update event\n");
                if (c->binary) {
                    c->state = conn_closing;
                } else {
                    conn_set_state(c, conn_closing);
                }
                return TRANSMIT_HARD_ERROR;
            }
            return TRANSMIT_SOFT_ERROR;
        }
        /* if res==0 or res==-1 and error is not EAGAIN or EWOULDBLOCK,
           we have a real error, on which we close the connection */
        if (settings.verbose > 0)
            perror("Failed to write, and not due to blocking");

        if (c->binary) {
            if (c->udp) {
                c->state = conn_bp_header_size_unknown;
            } else {
                c->state = conn_closing;
            }
        } else {
            if (c->udp)
                conn_set_state(c, conn_read);
            else
                conn_set_state(c, conn_closing);
        }
        return TRANSMIT_HARD_ERROR;
    } else {
        return TRANSMIT_COMPLETE;
    }
}

static void drive_machine(conn* c) {
    stats_t *stats = STATS_GET_TLS();
    bool stop = false;
    int sfd, flags = 1;
    socklen_t addrlen;
    struct sockaddr addr;
    int nreqs = settings.reqs_per_event;
    ssize_t res;

    assert(c != NULL);

    while (!stop) {

        switch(c->state) {
        case conn_listening:
            addrlen = sizeof(addr);
            if ((sfd = accept(c->sfd, &addr, &addrlen)) == -1) {
                if (errno == EAGAIN || errno == EWOULDBLOCK) {
                    /* these are transient, so don't log anything */
                    stop = true;
                } else if (errno == EMFILE) {
                    if (settings.verbose > 0)
                        fprintf(stderr, "Too many open connections\n");
                    accept_new_conns(false, c->binary);
                    stop = true;
                } else {
                    perror("accept()");
                    stop = true;
                }
                break;
            }

            if ((flags = fcntl(sfd, F_GETFL, 0)) < 0 ||
                fcntl(sfd, F_SETFL, flags | O_NONBLOCK) < 0) {
                perror("setting O_NONBLOCK");
                close(sfd);
                break;
            }
            dispatch_conn_new(sfd, conn_read, EV_READ | EV_PERSIST,
                              NULL, false, c->binary,
                              &addr, addrlen);

            break;

        case conn_read:
            if (try_read_command(c) != 0) {
                continue;
            }
            /* If we haven't exhausted our request-per-event limit and there's more
               to read, keep going, otherwise stop to give another conn a
               chance or wait */
            if(nreqs && ((c->udp ? try_read_udp(c) : try_read_network(c)) != 0)) {
                nreqs--;
                continue;
            }
            /* we have no command line and no data to read from network */
            if (!update_event(c, EV_READ | EV_PERSIST)) {
                if (settings.verbose > 0)
                    fprintf(stderr, "Couldn't update event\n");
                conn_set_state(c, conn_closing);
                break;
            }
            stop = true;
            break;

        case conn_nread:
            /* we are reading rlbytes into ritem; */
            if (c->riov_left == 0) {
                complete_nread(c);
                break;
            }
            /* first check if we have leftovers in the conn_read buffer */
            if (c->rbytes > 0) {
                while (c->rbytes > 0 &&
                       c->riov_left > 0) {
                    struct iovec* current_iov = &c->riov[c->riov_curr];

                    int tocopy = c->rbytes <= current_iov->iov_len ? c->rbytes : current_iov->iov_len;

                    memcpy(current_iov->iov_base, c->rcurr, tocopy);
                    c->rcurr += tocopy;
                    c->rbytes -= tocopy;
                    current_iov->iov_base += tocopy;
                    current_iov->iov_len -= tocopy;

                    /* are we done with the current IOV? */
                    if (current_iov->iov_len == 0) {
                        c->riov_curr ++;
                        c->riov_left --;
                    }
                }
                break;
            }

            /*  now try reading from the socket */
            res = readv(c->sfd, &c->riov[c->riov_curr],
                        c->riov_left <= IOV_MAX ? c->riov_left : IOV_MAX);
            if (res > 0) {
                STATS_LOCK(stats);
                stats->bytes_read += res;
                STATS_UNLOCK(stats);

                while (res > 0) {
                    struct iovec* current_iov = &c->riov[c->riov_curr];
                    int copied_to_current_iov = current_iov->iov_len <= res ? current_iov->iov_len : res;

                    res -= copied_to_current_iov;
                    current_iov->iov_base += copied_to_current_iov;
                    current_iov->iov_len -= copied_to_current_iov;

                    /* are we done with the current IOV? */
                    if (current_iov->iov_len == 0) {
                        c->riov_curr ++;
                        c->riov_left --;
                    }
                }
                break;
            }
            if (res == 0) { /* end of stream */
                conn_set_state(c, conn_closing);
                break;
            }
            if (res == -1 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
                if (!update_event(c, EV_READ | EV_PERSIST)) {
                    if (settings.verbose > 0)
                        fprintf(stderr, "Couldn't update event\n");
                    conn_set_state(c, conn_closing);
                    break;
                }
                stop = true;
                break;
            }
            /* otherwise we have a real error, on which we close the connection */
            if (settings.verbose > 0)
                fprintf(stderr, "Failed to read, and not due to blocking\n");
            conn_set_state(c, conn_closing);
            break;

        case conn_swallow:
            /* we are reading sbytes and throwing them away */
            if (c->sbytes == 0) {
                conn_set_state(c, conn_read);
                break;
            }

            /* first check if we have leftovers in the conn_read buffer */
            if (c->rbytes > 0) {
                int tocopy = c->rbytes > c->sbytes ? c->sbytes : c->rbytes;
                c->sbytes -= tocopy;
                c->rcurr += tocopy;
                c->rbytes -= tocopy;
                break;
            }

            assert(c->rbuf != NULL);

            /*  now try reading from the socket */
            res = read(c->sfd, c->rbuf, c->rsize > c->sbytes ? c->sbytes : c->rsize);
            if (res > 0) {
                STATS_LOCK(stats);
                stats->bytes_read += res;
                STATS_UNLOCK(stats);
                c->sbytes -= res;

                /* report peak usage here */
                report_max_rusage(c->cbg, c->rbuf, res);

                break;
            }
            if (res == 0) { /* end of stream */
                conn_set_state(c, conn_closing);
                break;
            }
            if (res == -1 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
                if (!update_event(c, EV_READ | EV_PERSIST)) {
                    if (settings.verbose > 0)
                        fprintf(stderr, "Couldn't update event\n");
                    conn_set_state(c, conn_closing);
                    break;
                }
                stop = true;
                break;
            }
            /* otherwise we have a real error, on which we close the connection */
            if (settings.verbose > 0)
                fprintf(stderr, "Failed to read, and not due to blocking\n");
            conn_set_state(c, conn_closing);
            break;

        case conn_write:
            /*
             * We want to write out a simple response.  If we haven't already,
             * assemble it into a msgbuf list (this will be a single-entry list
             * for TCP or a two-entry list for UDP).
             */

            if (c->iovused == 0) {
                assert(c->msgused == 0);
                if (add_msghdr(c) != 0 ||
                    add_iov(c, c->wcurr, c->wbytes, true) != 0 ||
                    (c->udp && build_udp_headers(c) != 0)) {
                    if (settings.verbose > 0)
                        fprintf(stderr, "Couldn't build response\n");
                    conn_set_state(c, conn_closing);
                    break;
                }
            }

            /* fall through... */

        case conn_mwrite:
            switch (transmit(c)) {
            case TRANSMIT_COMPLETE:
                if (c->state == conn_mwrite) {
                    while (c->ileft > 0) {
                        item *it = *(c->icurr);
                        assert(ITEM_is_valid(it));
                        item_deref(it);
                        c->icurr++;
                        c->ileft--;
                    }
                    conn_set_state(c, conn_read);
                } else if (c->state == conn_write) {
                    if (c->write_and_free) {
                        free(c->write_and_free);
                        c->write_and_free = 0;
                    }
                    conn_set_state(c, c->write_and_go);
                } else {
                    if (settings.verbose > 0)
                        fprintf(stderr, "Unexpected state %d\n", c->state);
                    conn_set_state(c, conn_closing);
                }

                /* clear and reset wbuf. */
                c->wcurr = c->wbuf;
                c->wbytes = 0;

                break;

            case TRANSMIT_INCOMPLETE:
            case TRANSMIT_HARD_ERROR:
                break;                   /* Continue in state machine. */

            case TRANSMIT_SOFT_ERROR:
                stop = true;
                break;
            }
            break;

        case conn_closing:
            if (c->udp)
                conn_cleanup(c);
            else
                conn_close(c);
            stop = true;
            break;

        default:
            abort();
        }
    }

    return;
}

void event_handler(const int fd, const short which, void *arg) {
    conn* c;

    c = (conn*) arg;
    assert(c != NULL);

    c->which = which;

    /* sanity */
    if (fd != c->sfd) {
        if (settings.verbose > 0)
            fprintf(stderr, "Catastrophic: event fd doesn't match conn fd!\n");
        conn_close(c);
        return;
    }

    if (c->binary) {
        process_binary_protocol(c);
    } else {
        drive_machine(c);
    }

    /* wait for next event */
    return;
}

static int new_socket(const bool is_udp) {
    int sfd;
    int flags;

    if ((sfd = socket(AF_INET, is_udp ? SOCK_DGRAM : SOCK_STREAM, 0)) == -1) {
        perror("socket()");
        return -1;
    }

    if ((flags = fcntl(sfd, F_GETFL, 0)) < 0 ||
        fcntl(sfd, F_SETFL, flags | O_NONBLOCK) < 0) {
        perror("setting O_NONBLOCK");
        close(sfd);
        return -1;
    }
    return sfd;
}


/*
 * Sets a socket's buffer size to the maximum allowed by the system.
 */
static void maximize_socket_buffer(const int sfd, int optname) {
    socklen_t intsize = sizeof(int);
    int last_good = 0;
    int min, max, avg;
    int old_size;
    const char* optname_str;

    /* Start with the default size. */
    if (getsockopt(sfd, SOL_SOCKET, optname, &old_size, &intsize) != 0) {
        if (settings.verbose > 0)
            perror("getsockopt()");
        return;
    }

    /* Binary-search for the real maximum. */
    min = old_size + 1;
    max = MAX_SENDBUF_SIZE;

    while (min <= max) {
        int success = 1;
        int current;

        avg = ((unsigned int)(min + max)) / 2;

        if (setsockopt(sfd, SOL_SOCKET, optname, (void *)&avg, intsize) != 0) {
            success = 0;
        }

        if (success == 1) {
            if (getsockopt(sfd, SOL_SOCKET, optname, &current, &intsize) != 0) {
                if (settings.verbose > 0)
                    perror("getsockopt()");
                return;
            }

            if (current == avg) {
                /* success */
                last_good = avg;
                min = avg + 1;
            }
            if (current >= min &&
                current < avg) {
                /* the setsockopt did something, but it didn't set it at the desired
                   value.  this probably means we found the max. */
                last_good = current;
                break;
            }
        }

        /* failed to increase */
        max = avg - 1;
    }

    if (settings.verbose > 1) {
        switch (optname) {
            case SO_SNDBUF:
                optname_str = "send";
                break;

            case SO_RCVBUF:
                optname_str = "receive";
                break;

            default:
                optname_str = "(unknown)";
                break;
        }
        fprintf(stderr, "<%d %s buffer was %d, now %d\n", sfd, optname_str, old_size, last_good);
    }
}


static int server_socket(const int port, const bool is_udp) {
    int sfd;
    struct linger ling = {0, 0};
    struct sockaddr_in addr;
    int flags =1;

    if ((sfd = new_socket(is_udp)) == -1) {
        return -1;
    }

    setsockopt(sfd, SOL_SOCKET, SO_REUSEADDR, (void *)&flags, sizeof(flags));
    if (is_udp) {
        maximize_socket_buffer(sfd, SO_SNDBUF);
        maximize_socket_buffer(sfd, SO_RCVBUF);
    } else {
        setsockopt(sfd, SOL_SOCKET, SO_KEEPALIVE, (void *)&flags, sizeof(flags));
        setsockopt(sfd, SOL_SOCKET, SO_LINGER, (void *)&ling, sizeof(ling));
        setsockopt(sfd, IPPROTO_TCP, TCP_NODELAY, (void *)&flags, sizeof(flags));
    }

    /*
     * the memset call clears nonstandard fields in some impementations
     * that otherwise mess things up.
     */
    memset(&addr, 0, sizeof(addr));

    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);
    addr.sin_addr = settings.interf;
    if (bind(sfd, (struct sockaddr *)&addr, sizeof(addr)) == -1) {
        perror("bind()");
        close(sfd);
        return -1;
    }
    if (!is_udp && listen(sfd, 1024) == -1) {
        perror("listen()");
        close(sfd);
        return -1;
    }
    return sfd;
}

static int new_socket_unix(void) {
    int sfd;
    int flags;

    if ((sfd = socket(AF_UNIX, SOCK_STREAM, 0)) == -1) {
        perror("socket()");
        return -1;
    }

    if ((flags = fcntl(sfd, F_GETFL, 0)) < 0 ||
        fcntl(sfd, F_SETFL, flags | O_NONBLOCK) < 0) {
        perror("setting O_NONBLOCK");
        close(sfd);
        return -1;
    }
    return sfd;
}

static int server_socket_unix(const char *path) {
    int sfd;
    struct linger ling = {0, 0};
    struct sockaddr_un addr;
    struct stat tstat;
    int flags =1;

    if (!path) {
        return -1;
    }

    if ((sfd = new_socket_unix()) == -1) {
        return -1;
    }

    /*
     * Clean up a previous socket file if we left it around
     */
    if (lstat(path, &tstat) == 0) {
        if (S_ISSOCK(tstat.st_mode))
            unlink(path);
    }

    setsockopt(sfd, SOL_SOCKET, SO_REUSEADDR, (void *)&flags, sizeof(flags));
    setsockopt(sfd, SOL_SOCKET, SO_KEEPALIVE, (void *)&flags, sizeof(flags));
    setsockopt(sfd, SOL_SOCKET, SO_LINGER, (void *)&ling, sizeof(ling));

    /*
     * the memset call clears nonstandard fields in some impementations
     * that otherwise mess things up.
     */
    memset(&addr, 0, sizeof(addr));

    addr.sun_family = AF_UNIX;
    strcpy(addr.sun_path, path);
    if (bind(sfd, (struct sockaddr *)&addr, sizeof(addr)) == -1) {
        perror("bind()");
        close(sfd);
        return -1;
    }
    if (listen(sfd, LISTEN_DEPTH) == -1) {
        perror("listen()");
        close(sfd);
        return -1;
    }
    return sfd;
}

/* listening socket */
static int l_socket = 0;

/* udp socket */
static int u_socket = -1;

/* binary listening socket */
static int b_socket = 0;

/* binary udp socket */
static int bu_socket = -1;


/* invoke right before gdb is called, on assert */
void pre_gdb(void) {
    int i;
    if (l_socket > -1) close(l_socket);
    if (u_socket > -1) close(u_socket);
    for (i = 3; i <= 500; i++) close(i); /* so lame */
    kill(getpid(), SIGABRT);
}

/*
 * We keep the current time of day in a global variable that's updated by a
 * timer event. This saves us a bunch of time() system calls (we really only
 * need to get the time once a second, whereas there can be tens of thousands
 * of requests a second) and allows us to use server-start-relative timestamps
 * rather than absolute UNIX timestamps, a space savings on systems where
 * sizeof(time_t) > sizeof(unsigned int).
 */
volatile rel_time_t current_time;
time_t started;

/* time-sensitive callers can call it by hand with this, outside the normal ever-1-second timer */
void set_current_time(void) {
    current_time = (rel_time_t) (time(0) - started);
}

void update_stats(void) {
    stats_t *stats = STATS_GET_TLS();

    STATS_LOCK(stats);
    stats->byte_seconds += stats->item_total_size;
    STATS_UNLOCK(stats);
}

static struct event deleteevent;

static void delete_handler(const int fd, const short which, void *arg) {
    struct timeval t = {.tv_sec = 5, .tv_usec = 0};
    static bool initialized = false;

    if (initialized) {
        /* some versions of libevent don't like deleting events that don't exist,
           so only delete once we know this event has been added. */
        evtimer_del(&deleteevent);
    } else {
        initialized = true;
    }

    evtimer_set(&deleteevent, delete_handler, 0);
    event_base_set(main_base, &deleteevent);
    evtimer_add(&deleteevent, &t);
    run_deferred_deletes();
}

/* Call run_deferred_deletes instead of this. */
void do_run_deferred_deletes(void)
{
    int i, j = 0;

    for (i = 0; i < delcurr; i++) {
        item *it = todelete[i];
        if (item_delete_lock_over(it)) {
            assert(ITEM_refcount(it) > 0);
            ITEM_unmark_deleted(it);
            do_item_unlink(it, UNLINK_NORMAL, NULL);
            do_item_deref(it);
        } else {
            todelete[j++] = it;
        }
    }
    delcurr = j;
}

static void usage(void) {
    printf(PACKAGE " " VERSION "\n");
    printf("-p <num>      TCP port number to listen on (default: 0, off)\n"
           "-U <num>      UDP port number to listen on (default: 0, off)\n"
           "-n <num>      TCP port number to listen on for binary connections (default: 0, off)\n"
           "-N <num>      UDP port number to listen on for binary connections (default: 0, off)\n"
           "-s <file>     unix socket path to listen on (disables network support)\n"
           "-l <ip_addr>  interface to listen on, default is INDRR_ANY\n"
           "-d            run as a daemon\n"
           "-r            maximize core file limit\n"
           "-u <username> assume identity of <username> (only when run as root)\n"
           "-m <num>      max memory to use for items in megabytes, default is 64 MB\n"
           "-M            return error on memory exhausted (rather than removing items)\n"
           "-c <num>      max simultaneous connections, default is 1024\n"
           "-k            lock down all paged memory\n"
           "-v            verbose (print errors/warnings while in event loop)\n"
           "-vv           very verbose (also print client commands/reponses)\n"
           "-h            print this help and exit\n"
           "-i            print memcached and libevent license\n"
           "-b            run a managed instanced (mnemonic: buckets)\n"
           "-P <file>     save PID in <file>, only used with -d option\n"
           "-f <factor>   chunk size growth factor, default 1.25\n"
           "-n <bytes>    minimum space allocated for key+value+flags, default 48\n");
    printf("-t <num>      number of threads to use, default 4\n");
    printf("-R            Maximum number of requests per event\n"
           "              limits the number of requests process for a given connection\n"
           "              to prevent starvation.  default 1\n");
    printf("-C            Maximum bytes used for connection buffers\n"
           "              default 16MB\n");
    return;
}

static void usage_license(void) {
    printf(PACKAGE " " VERSION "\n\n");
    printf(
    "Copyright (c) 2003, Danga Interactive, Inc. <http://www.danga.com/>\n"
    "All rights reserved.\n"
    "\n"
    "Redistribution and use in source and binary forms, with or without\n"
    "modification, are permitted provided that the following conditions are\n"
    "met:\n"
    "\n"
    "    * Redistributions of source code must retain the above copyright\n"
    "notice, this list of conditions and the following disclaimer.\n"
    "\n"
    "    * Redistributions in binary form must reproduce the above\n"
    "copyright notice, this list of conditions and the following disclaimer\n"
    "in the documentation and/or other materials provided with the\n"
    "distribution.\n"
    "\n"
    "    * Neither the name of the Danga Interactive nor the names of its\n"
    "contributors may be used to endorse or promote products derived from\n"
    "this software without specific prior written permission.\n"
    "\n"
    "THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS\n"
    "\"AS IS\" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT\n"
    "LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR\n"
    "A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT\n"
    "OWNER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL,\n"
    "SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT\n"
    "LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,\n"
    "DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY\n"
    "THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT\n"
    "(INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE\n"
    "OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.\n"
    "\n"
    "\n"
    "This product includes software developed by Niels Provos.\n"
    "\n"
    "[ libevent ]\n"
    "\n"
    "Copyright 2000-2003 Niels Provos <provos@citi.umich.edu>\n"
    "All rights reserved.\n"
    "\n"
    "Redistribution and use in source and binary forms, with or without\n"
    "modification, are permitted provided that the following conditions\n"
    "are met:\n"
    "1. Redistributions of source code must retain the above copyright\n"
    "   notice, this list of conditions and the following disclaimer.\n"
    "2. Redistributions in binary form must reproduce the above copyright\n"
    "   notice, this list of conditions and the following disclaimer in the\n"
    "   documentation and/or other materials provided with the distribution.\n"
    "3. All advertising materials mentioning features or use of this software\n"
    "   must display the following acknowledgement:\n"
    "      This product includes software developed by Niels Provos.\n"
    "4. The name of the author may not be used to endorse or promote products\n"
    "   derived from this software without specific prior written permission.\n"
    "\n"
    "THIS SOFTWARE IS PROVIDED BY THE AUTHOR ``AS IS'' AND ANY EXPRESS OR\n"
    "IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED WARRANTIES\n"
    "OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE DISCLAIMED.\n"
    "IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR ANY DIRECT, INDIRECT,\n"
    "INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT\n"
    "NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,\n"
    "DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY\n"
    "THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT\n"
    "(INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF\n"
    "THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.\n"
    );

    return;
}

static void save_pid(const pid_t pid, const char *pid_file) {
    FILE *fp;
    if (pid_file == NULL)
        return;

    if ((fp = fopen(pid_file, "w")) == NULL) {
        fprintf(stderr, "Could not open the pid file %s for writing\n", pid_file);
        return;
    }

    fprintf(fp,"%ld\n", (long)pid);
    if (fclose(fp) == -1) {
        fprintf(stderr, "Could not close the pid file %s.\n", pid_file);
        return;
    }
}

static void remove_pidfile(const char *pid_file) {
  if (pid_file == NULL)
      return;

  if (unlink(pid_file) != 0) {
      fprintf(stderr, "Could not remove the pid file %s.\n", pid_file);
  }

}


static void sig_handler(const int sig) {
    printf("SIGINT handled.\n");
    exit(EXIT_SUCCESS);
}

int main (int argc, char **argv) {
    int c;
    struct in_addr addr;
    bool lock_memory = false;
    bool daemonize = false;
    int maxcore = 0;
    char *username = NULL;
    char *pid_file = NULL;
    struct passwd *pw;
    struct sigaction sa;
    struct rlimit rlim;

    /* handle SIGINT */
    signal(SIGINT, sig_handler);

    /* init settings */
    settings_init();

    /* set stderr non-buffering (for running under, say, daemontools) */
    setbuf(stderr, NULL);

    /* process arguments */
    while ((c = getopt(argc, argv, "bp:s:U:m:Mc:khirvdl:u:P:f:s:n:t:D:n:N:R:C:")) != -1) {
        switch (c) {
        case 'U':
            settings.udpport = atoi(optarg);
            break;
        case 'b':
            settings.managed = true;
            break;
        case 'p':
            settings.port = atoi(optarg);
            break;
        case 's':
            settings.socketpath = optarg;
            break;
        case 'm':
            settings.maxbytes = ((size_t)atoi(optarg)) * 1024 * 1024;
            break;
        case 'M':
            settings.evict_to_free = 0;
            break;
        case 'c':
            settings.maxconns = atoi(optarg);
            break;
        case 'h':
            usage();
            exit(EXIT_SUCCESS);
        case 'i':
            usage_license();
            exit(EXIT_SUCCESS);
        case 'k':
            lock_memory = true;
            break;
        case 'v':
            settings.verbose++;
            break;
        case 'l':
            if (inet_pton(AF_INET, optarg, &addr) <= 0) {
                fprintf(stderr, "Illegal address: %s\n", optarg);
                return 1;
            } else {
                settings.interf = addr;
            }
            break;
        case 'd':
            daemonize = true;
            setup_sigsegv();
            break;
        case 'r':
            maxcore = 1;
            break;
        case 'R':
            settings.reqs_per_event = atoi(optarg);
            if (settings.reqs_per_event == 0) {
                fprintf(stderr, "Number of requests per event must be greater than 0\n");
                return 1;
            }
            break;
        case 'u':
            username = optarg;
            break;
        case 'P':
            pid_file = optarg;
            break;
        case 'f':
            settings.factor = atof(optarg);
            if (settings.factor <= 1.0) {
                fprintf(stderr, "Factor must be greater than 1\n");
                return 1;
            }
            break;
        case 't':
            settings.num_threads = atoi(optarg) + 1; /* extra thread for dispatcher */
            if (settings.num_threads == 0) {
                fprintf(stderr, "Number of threads must be greater than 0\n");
                return 1;
            }
            break;
        case 'D':
            if (! optarg || ! optarg[0]) {
                fprintf(stderr, "No delimiter specified\n");
                return 1;
            }
            settings.prefix_delimiter = optarg[0];
            settings.detail_enabled = 1;
            break;
        case 'n':
            settings.binary_port = atoi(optarg);
            break;
        case 'N':
            settings.binary_udpport = atoi(optarg);
            break;

        case 'C':
            settings.max_conn_buffer_bytes = atoi(optarg);
            break;

        default:
            fprintf(stderr, "Illegal argument \"%c\"\n", c);
            return 1;
        }
    }

    if (maxcore != 0) {
        struct rlimit rlim_new;
        /*
         * First try raising to infinity; if that fails, try bringing
         * the soft limit to the hard.
         */
        if (getrlimit(RLIMIT_CORE, &rlim) == 0) {
            rlim_new.rlim_cur = rlim_new.rlim_max = RLIM_INFINITY;
            if (setrlimit(RLIMIT_CORE, &rlim_new)!= 0) {
                /* failed. try raising just to the old max */
                rlim_new.rlim_cur = rlim_new.rlim_max = rlim.rlim_max;
                (void)setrlimit(RLIMIT_CORE, &rlim_new);
            }
        }
        /*
         * getrlimit again to see what we ended up with. Only fail if
         * the soft limit ends up 0, because then no core files will be
         * created at all.
         */

        if ((getrlimit(RLIMIT_CORE, &rlim) != 0) || rlim.rlim_cur == 0) {
            fprintf(stderr, "failed to ensure corefile creation\n");
            exit(EXIT_FAILURE);
        }
    }

    /*
     * If needed, increase rlimits to allow as many connections
     * as needed.
     */

    if (getrlimit(RLIMIT_NOFILE, &rlim) != 0) {
        fprintf(stderr, "failed to getrlimit number of files\n");
        exit(EXIT_FAILURE);
    } else {
        int maxfiles = settings.maxconns;
        if (rlim.rlim_cur < maxfiles)
            rlim.rlim_cur = maxfiles + 3;
        if (rlim.rlim_max < rlim.rlim_cur)
            rlim.rlim_max = rlim.rlim_cur;
        if (setrlimit(RLIMIT_NOFILE, &rlim) != 0) {
            fprintf(stderr, "failed to set rlimit for open files. Try running as root or requesting smaller maxconns value.\n");
            exit(EXIT_FAILURE);
        }
    }

    /*
     * initialization order: first create the listening sockets
     * (may need root on low ports), then drop root if needed,
     * then daemonise if needed, then init libevent (in some cases
     * descriptors created by libevent wouldn't survive forking).
     */

    /* create the listening socket and bind it */
    if (settings.socketpath == NULL) {
        if (settings.port == 0 && settings.binary_port == 0) {
            fprintf(stderr, "Either -p or -n must be specified.\n");
            exit(1);
        }

        if (settings.port > 0) {
            l_socket = server_socket(settings.port, 0);
            if (l_socket == -1) {
                fprintf(stderr, "failed to listen\n");
                exit(1);
            }
        }
        if (settings.binary_port > 0) {
            if ((b_socket = server_socket(settings.binary_port, 0)) == -1) {
                fprintf(stderr, "bp failed to listen\n");
                exit(1);
            }
        }
    }

    if (settings.udpport > 0 && settings.socketpath == NULL) {
        /* create the UDP listening socket and bind it */
        u_socket = server_socket(settings.udpport, 1);
        if (u_socket == -1) {
            fprintf(stderr, "failed to listen on UDP port %d\n", settings.udpport);
            exit(EXIT_FAILURE);
        }
    }
    if (settings.binary_udpport > 0 && ! settings.socketpath) {
        /* create the UDP listening socket and bind it */
        if ((bu_socket = server_socket(settings.binary_udpport, 1)) == -1) {
            fprintf(stderr, "failed to listen on UDP port %d\n", settings.binary_udpport);
            exit(1);
        }
    }

    /* before we drop root privileges, we should open /proc/self/maps if this is
       a supported OS */
#if !defined(WIN32) || !defined(__APPLE__)
    {
        char proc_pid_maps[6 /* '/proc/' */
                           + 5 /* pid */
                           + 5 /* /maps */
                           + 1 /* null terminator */];
        int written;

        written = snprintf(proc_pid_maps, sizeof(proc_pid_maps), "/proc/%d/maps", getpid());

        if (written < 0 ||
            written >= sizeof(proc_pid_maps)) {
            fprintf(stderr, "can't fit maps filename in array\n");
        } else {
            maps_fd = open(proc_pid_maps, O_RDONLY);
        }
    }
#endif /* #if !defined(WIN32) || !defined(__APPLE__) */

    /* lose root privileges if we have them */
    if (getuid() == 0 || geteuid() == 0) {
        if (username == 0 || *username == '\0') {
            fprintf(stderr, "can't run as root without the -u switch\n");
            return 1;
        }
        if ((pw = getpwnam(username)) == 0) {
            fprintf(stderr, "can't find the user %s to switch to\n", username);
            return 1;
        }
        if (setgid(pw->pw_gid) < 0 || setuid(pw->pw_uid) < 0) {
            fprintf(stderr, "failed to assume identity of user %s\n", username);
            return 1;
        }
    }

    /* create unix mode sockets after dropping privileges */
    if (settings.socketpath != NULL) {
        l_socket = server_socket_unix(settings.socketpath);
        if (l_socket == -1) {
            fprintf(stderr, "failed to listen\n");
            exit(EXIT_FAILURE);
        }
        settings.binary_port = 0;
        settings.binary_udpport = 0;
    }

    /* daemonize if requested */
    /* if we want to ensure our ability to dump core, don't chdir to / */
    if (daemonize) {
        int res;
        res = daemon(maxcore, settings.verbose);
        if (res == -1) {
            fprintf(stderr, "failed to daemon() in order to daemonize\n");
            return 1;
        }
    }

    /* initialize main thread libevent instance */
    main_base = event_init();

    /* make the time we started always be 2 seconds before we really
       did, so time(0) - time.started is never zero.  if so, things
       like 'settings.oldest_live' which act as booleans as well as
       values are now false in boolean context... */
    started = time(0) - 2;

    /* initialize other stuff */
    item_init();
    stats_init(settings.num_threads);
    STATS_SET_TLS(0);
    assoc_init();
    conn_init();
#if defined(USE_SLAB_ALLOCATOR)
    slabs_init(settings.maxbytes, settings.factor);
#endif /* #if defined(USE_SLAB_ALLOCATOR) */
#if defined(USE_FLAT_ALLOCATOR)
    flat_storage_init(settings.maxbytes);
#endif /* #if defined(USE_FLAT_ALLOCATOR) */
    conn_buffer_init(settings.num_threads - 1, 0, 0, settings.max_conn_buffer_bytes / 2, settings.max_conn_buffer_bytes);

    /* managed instance? alloc and zero a bucket array */
    if (settings.managed) {
        buckets = malloc(sizeof(int) * MAX_BUCKETS);
        if (buckets == 0) {
            fprintf(stderr, "failed to allocate the bucket array");
            exit(EXIT_FAILURE);
        }
        memset(buckets, 0, sizeof(int) * MAX_BUCKETS);
    }

    /* lock paged memory if needed */
    if (lock_memory) {
#ifdef HAVE_MLOCKALL
        mlockall(MCL_CURRENT | MCL_FUTURE);
#else
        fprintf(stderr, "warning: mlockall() not supported on this platform.  proceeding without.\n");
#endif
    }

    /*
     * ignore SIGPIPE signals; we can use errno==EPIPE if we
     * need that information
     */
    sa.sa_handler = SIG_IGN;
    sa.sa_flags = 0;
    if (sigemptyset(&sa.sa_mask) == -1 ||
        sigaction(SIGPIPE, &sa, 0) == -1) {
        perror("failed to ignore SIGPIPE; sigaction");
        exit(EXIT_FAILURE);
    }
    /* create the initial listening connection */
    if (l_socket != 0) {
        if (!(listen_conn = conn_new(l_socket, conn_listening,
                                     EV_READ | EV_PERSIST, NULL, false, false,
                                     NULL, 0,
                                     main_base))) {
            fprintf(stderr, "failed to create listening connection");
            exit(1);
        }
    }
    if ((settings.binary_port != 0) &&
        (listen_binary_conn = conn_new(b_socket, conn_listening,
                                       EV_READ | EV_PERSIST, NULL, false, true,
                                       NULL, 0,
                                       main_base)) == NULL) {
        fprintf(stderr, "failed to create listening connection");
        exit(EXIT_FAILURE);
    }
    /* start up worker threads if MT mode */
    thread_init(settings.num_threads, main_base);
    /* save the PID in if we're a daemon, do this after thread_init due to
       a file descriptor handling bug somewhere in libevent */
    if (daemonize)
        save_pid(getpid(), pid_file);
    /* initialise clock event */
    clock_handler(0, 0, NULL);
    /* initialise deletion array and timer event */
    deltotal = 200;
    delcurr = 0;
    if ((todelete = pool_malloc(sizeof(item *) * deltotal, DELETE_POOL)) == NULL) {
        perror("failed to allocate memory for deletion array");
        exit(EXIT_FAILURE);
    }
    delete_handler(0, 0, 0); /* sets up the event */
    /* create the initial listening udp connection, monitored on all threads */
    if (u_socket > -1) {
        /* Skip thread 0, the tcp accept socket dispatcher
           if running with > 1 thread. */
        for (c = 1; c < settings.num_threads; c++) {
            /* this is guaranteed to hit all threads because we round-robin */
            dispatch_conn_new(u_socket, conn_read, EV_READ | EV_PERSIST,
                              get_conn_buffer_group(c - 1), 1, 0, NULL, 0);
        }
    }
    /* create the initial listening udp connection, monitored on all threads */
    if (bu_socket > -1) {
        /* Skip thread 0, the tcp accept socket dispatcher
           if running with > 1 thread. */
        for (c = 1; c < settings.num_threads; c++) {
            /* this is guaranteed to hit all threads because we round-robin */
            dispatch_conn_new(bu_socket, conn_bp_header_size_unknown, EV_READ | EV_PERSIST,
                              get_conn_buffer_group(c - 1), true, true, NULL, 0);
        }
    }
    /* enter the event loop */
    event_base_loop(main_base, 0);
    /* remove the PID file if we're a daemon */
    if (daemonize)
        remove_pidfile(pid_file);
    return 0;
}
