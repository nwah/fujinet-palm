/* transport_tcp.c
 *
 * TCP-socket FnTransport implementation. See transport_tcp.h.
 *
 * Host-only code: ordinary C99 + POSIX, not subject to the ANSI C89
 * constraints of the portable core library.
 */

#define _POSIX_C_SOURCE 200809L
#define _DARWIN_C_SOURCE

#include "transport_tcp.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <unistd.h>
#include <poll.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <netdb.h>

typedef struct {
    int fd;
    int verbose;
} TcpCtx;

static void hexdump(const char *prefix, const fn_u8 *buf, fn_u16 len)
{
    fn_u16 i;
    fprintf(stderr, "%s (%u bytes):", prefix, (unsigned)len);
    for (i = 0; i < len; i++) {
        if ((i % 16) == 0) {
            fprintf(stderr, "\n  %04x: ", (unsigned)i);
        }
        fprintf(stderr, "%02x ", buf[i]);
    }
    fprintf(stderr, "\n");
}

static fn_i16 tcp_send(void *ctx, const fn_u8 *buf, fn_u16 len)
{
    TcpCtx *tc = (TcpCtx *)ctx;
    fn_u16 sent = 0;

    if (tc->verbose) {
        hexdump("TX", buf, len);
    }

    while (sent < len) {
        ssize_t n = send(tc->fd, buf + sent, (size_t)(len - sent), 0);
        if (n < 0) {
            if (errno == EINTR) {
                continue;
            }
            fprintf(stderr, "transport_tcp: send failed: %s\n", strerror(errno));
            return -1;
        }
        if (n == 0) {
            fprintf(stderr, "transport_tcp: send returned 0 (connection closed)\n");
            return -1;
        }
        sent = (fn_u16)(sent + n);
    }
    return 0;
}

static fn_i16 tcp_recv(void *ctx, fn_u8 *buf, fn_u16 max, fn_u32 timeout_ms)
{
    TcpCtx *tc = (TcpCtx *)ctx;
    struct pollfd pfd;
    int rc;
    ssize_t n;

    pfd.fd = tc->fd;
    pfd.events = POLLIN;
    pfd.revents = 0;

    for (;;) {
        rc = poll(&pfd, 1, (int)timeout_ms);
        if (rc < 0) {
            if (errno == EINTR) {
                continue;
            }
            fprintf(stderr, "transport_tcp: poll failed: %s\n", strerror(errno));
            return -1;
        }
        break;
    }

    if (rc == 0) {
        return 0; /* timeout */
    }

    if (pfd.revents & (POLLERR | POLLNVAL)) {
        fprintf(stderr, "transport_tcp: poll error on socket\n");
        return -1;
    }

    n = recv(tc->fd, buf, (size_t)max, 0);
    if (n < 0) {
        fprintf(stderr, "transport_tcp: recv failed: %s\n", strerror(errno));
        return -1;
    }
    if (n == 0) {
        /* Peer closed the connection: treat as an error to the core. */
        return -1;
    }

    if (tc->verbose) {
        hexdump("RX", buf, (fn_u16)n);
    }

    return (fn_i16)n;
}

static void tcp_flush_rx(void *ctx)
{
    TcpCtx *tc = (TcpCtx *)ctx;
    struct pollfd pfd;
    unsigned char scratch[256];
    int drained = 0;

    pfd.fd = tc->fd;
    pfd.events = POLLIN;

    for (;;) {
        pfd.revents = 0;
        if (poll(&pfd, 1, 0) <= 0) {
            break;
        }
        if (!(pfd.revents & POLLIN)) {
            break;
        }
        {
            ssize_t n = recv(tc->fd, scratch, sizeof(scratch), 0);
            if (n <= 0) {
                break;
            }
            drained += (int)n;
        }
        if (drained >= 4096) {
            break;
        }
    }
}

FnTransport *fn_transport_tcp_open(const char *host, int port, int verbose)
{
    struct addrinfo hints, *res, *rp;
    char portstr[16];
    int fd = -1;
    int rc;
    TcpCtx *tc;
    FnTransport *t;

    memset(&hints, 0, sizeof(hints));
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    hints.ai_protocol = IPPROTO_TCP;

    snprintf(portstr, sizeof(portstr), "%d", port);

    rc = getaddrinfo(host, portstr, &hints, &res);
    if (rc != 0) {
        fprintf(stderr, "transport_tcp: getaddrinfo(%s:%d) failed: %s\n",
                host, port, gai_strerror(rc));
        return NULL;
    }

    for (rp = res; rp != NULL; rp = rp->ai_next) {
        fd = socket(rp->ai_family, rp->ai_socktype, rp->ai_protocol);
        if (fd < 0) {
            continue;
        }
        if (connect(fd, rp->ai_addr, rp->ai_addrlen) == 0) {
            break;
        }
        close(fd);
        fd = -1;
    }

    freeaddrinfo(res);

    if (fd < 0) {
        fprintf(stderr, "transport_tcp: could not connect to %s:%d: %s\n",
                host, port, strerror(errno));
        return NULL;
    }

    {
        int one = 1;
        if (setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one)) < 0) {
            fprintf(stderr, "transport_tcp: warning: setsockopt(TCP_NODELAY) failed: %s\n",
                    strerror(errno));
        }
    }

    tc = (TcpCtx *)malloc(sizeof(TcpCtx));
    if (tc == NULL) {
        fprintf(stderr, "transport_tcp: out of memory\n");
        close(fd);
        return NULL;
    }
    tc->fd = fd;
    tc->verbose = verbose;

    t = (FnTransport *)malloc(sizeof(FnTransport));
    if (t == NULL) {
        fprintf(stderr, "transport_tcp: out of memory\n");
        free(tc);
        close(fd);
        return NULL;
    }
    t->ctx = tc;
    t->send = tcp_send;
    t->recv = tcp_recv;
    t->flush_rx = tcp_flush_rx;

    return t;
}

void fn_transport_tcp_close(FnTransport *t)
{
    TcpCtx *tc;

    if (t == NULL) {
        return;
    }
    tc = (TcpCtx *)t->ctx;
    if (tc != NULL) {
        close(tc->fd);
        free(tc);
    }
    free(t);
}
