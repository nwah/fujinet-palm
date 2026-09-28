/* transport_serial.c
 *
 * POSIX serial-port FnTransport implementation. See transport_serial.h.
 *
 * Host-only code: ordinary C99 + POSIX, not subject to the ANSI C89
 * constraints of the portable core library.
 */

#define _POSIX_C_SOURCE 200809L
#define _DARWIN_C_SOURCE

#include "transport_serial.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <fcntl.h>
#include <unistd.h>
#include <poll.h>
#include <termios.h>

typedef struct {
    int fd;
    int verbose;
} SerialCtx;

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

static fn_i16 serial_send(void *ctx, const fn_u8 *buf, fn_u16 len)
{
    SerialCtx *sc = (SerialCtx *)ctx;
    fn_u16 sent = 0;

    if (sc->verbose) {
        hexdump("TX", buf, len);
    }

    while (sent < len) {
        ssize_t n = write(sc->fd, buf + sent, (size_t)(len - sent));
        if (n < 0) {
            if (errno == EINTR) {
                continue;
            }
            fprintf(stderr, "transport_serial: write failed: %s\n", strerror(errno));
            return -1;
        }
        if (n == 0) {
            fprintf(stderr, "transport_serial: write returned 0\n");
            return -1;
        }
        sent = (fn_u16)(sent + n);
    }
    return 0;
}

static fn_i16 serial_recv(void *ctx, fn_u8 *buf, fn_u16 max, fn_u32 timeout_ms)
{
    SerialCtx *sc = (SerialCtx *)ctx;
    struct pollfd pfd;
    int rc;
    ssize_t n;

    pfd.fd = sc->fd;
    pfd.events = POLLIN;
    pfd.revents = 0;

    for (;;) {
        rc = poll(&pfd, 1, (int)timeout_ms);
        if (rc < 0) {
            if (errno == EINTR) {
                continue;
            }
            fprintf(stderr, "transport_serial: poll failed: %s\n", strerror(errno));
            return -1;
        }
        break;
    }

    if (rc == 0) {
        return 0; /* timeout */
    }

    if (pfd.revents & (POLLERR | POLLNVAL)) {
        fprintf(stderr, "transport_serial: poll error on fd\n");
        return -1;
    }

    n = read(sc->fd, buf, (size_t)max);
    if (n < 0) {
        fprintf(stderr, "transport_serial: read failed: %s\n", strerror(errno));
        return -1;
    }
    if (n == 0) {
        return -1;
    }

    if (sc->verbose) {
        hexdump("RX", buf, (fn_u16)n);
    }

    return (fn_i16)n;
}

static void serial_flush_rx(void *ctx)
{
    SerialCtx *sc = (SerialCtx *)ctx;
    tcflush(sc->fd, TCIFLUSH);
}

/* Maps common integer baud rates to termios speed constants. */
static speed_t baud_to_speed(long baud)
{
    static const struct { long rate; speed_t speed; } table[] = {
        { 9600,   B9600 },
        { 19200,  B19200 },
        { 38400,  B38400 },
        { 57600,  B57600 },
        { 115200, B115200 },
        { 230400, B230400 }
    };
    size_t i;

    for (i = 0; i < sizeof(table) / sizeof(table[0]); i++) {
        if (table[i].rate == baud) {
            return table[i].speed;
        }
    }
    return B115200;
}

FnTransport *fn_transport_serial_open(const char *device, long baud, int verbose)
{
    int fd;
    struct termios tio;
    speed_t speed;
    SerialCtx *sc;
    FnTransport *t;

    fd = open(device, O_RDWR | O_NOCTTY | O_NONBLOCK);
    if (fd < 0) {
        fprintf(stderr, "transport_serial: open(%s) failed: %s\n", device, strerror(errno));
        return NULL;
    }

    /* Clear O_NONBLOCK now that the device is open; we want normal
     * blocking-with-poll semantics from here on. */
    if (fcntl(fd, F_SETFL, 0) < 0) {
        fprintf(stderr, "transport_serial: fcntl(F_SETFL) failed: %s\n", strerror(errno));
        close(fd);
        return NULL;
    }

    if (tcgetattr(fd, &tio) < 0) {
        fprintf(stderr, "transport_serial: tcgetattr failed: %s\n", strerror(errno));
        close(fd);
        return NULL;
    }

    cfmakeraw(&tio);

    tio.c_cflag = (tio.c_cflag & ~(unsigned)CSIZE) | CS8;
    tio.c_cflag &= ~(unsigned)PARENB;
    tio.c_cflag &= ~(unsigned)CSTOPB;
    tio.c_cflag |= (CLOCAL | CREAD);
#ifdef CRTSCTS
    tio.c_cflag &= ~(unsigned)CRTSCTS;
#endif
    tio.c_iflag &= ~(unsigned)(IXON | IXOFF | IXANY);

    speed = baud_to_speed(baud == 0 ? 115200 : baud);
    cfsetspeed(&tio, speed);

    if (tcsetattr(fd, TCSANOW, &tio) < 0) {
        fprintf(stderr, "transport_serial: tcsetattr failed: %s\n", strerror(errno));
        close(fd);
        return NULL;
    }

    sc = (SerialCtx *)malloc(sizeof(SerialCtx));
    if (sc == NULL) {
        fprintf(stderr, "transport_serial: out of memory\n");
        close(fd);
        return NULL;
    }
    sc->fd = fd;
    sc->verbose = verbose;

    t = (FnTransport *)malloc(sizeof(FnTransport));
    if (t == NULL) {
        fprintf(stderr, "transport_serial: out of memory\n");
        free(sc);
        close(fd);
        return NULL;
    }
    t->ctx = sc;
    t->send = serial_send;
    t->recv = serial_recv;
    t->flush_rx = serial_flush_rx;

    return t;
}

void fn_transport_serial_close(FnTransport *t)
{
    SerialCtx *sc;

    if (t == NULL) {
        return;
    }
    sc = (SerialCtx *)t->ctx;
    if (sc != NULL) {
        close(sc->fd);
        free(sc);
    }
    free(t);
}
