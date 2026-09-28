/* transport_serial.h
 *
 * POSIX serial-port FnTransport implementation for the macOS host CLI.
 * Talks to a real FujiNet adapter over a USB-serial device such as
 * /dev/cu.usbserial-XXXX.
 */

#ifndef FN_TRANSPORT_SERIAL_H
#define FN_TRANSPORT_SERIAL_H

#include "fujibus.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Opens `device` (e.g. "/dev/cu.usbserial-XXXX") at `baud` (0 means default
 * 115200). Returns a heap-allocated FnTransport, or NULL on failure (prints
 * error to stderr). `verbose` non-zero enables hex-dump tracing of
 * sent/received bytes to stderr. */
FnTransport *fn_transport_serial_open(const char *device, long baud, int verbose);

/* Closes the fd and frees the transport and its context. */
void fn_transport_serial_close(FnTransport *t);

#ifdef __cplusplus
}
#endif

#endif /* FN_TRANSPORT_SERIAL_H */
