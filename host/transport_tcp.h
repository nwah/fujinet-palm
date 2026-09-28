/* transport_tcp.h
 *
 * TCP-socket FnTransport implementation for the macOS/POSIX host CLI.
 * Connects to a FujiBus-over-TCP bridge (e.g. the fujinet-pc emulator or a
 * TCP<->serial bridge) at host:port.
 */

#ifndef FN_TRANSPORT_TCP_H
#define FN_TRANSPORT_TCP_H

#include "fujibus.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Connects to host:port over TCP (TCP_NODELAY set). Returns a heap-allocated
 * FnTransport ready to use, or NULL on failure (prints an error to stderr).
 * `verbose` non-zero enables hex-dump tracing of sent/received bytes to
 * stderr. */
FnTransport *fn_transport_tcp_open(const char *host, int port, int verbose);

/* Closes the socket and frees the transport and its context. */
void fn_transport_tcp_close(FnTransport *t);

#ifdef __cplusplus
}
#endif

#endif /* FN_TRANSPORT_TCP_H */
