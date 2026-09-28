#ifndef FN_TRANSPORT_H
#define FN_TRANSPORT_H

#include "fn_types.h"

typedef struct FnTransport {
    void *ctx;
    /* Send exactly `len` bytes. Return 0 on success, <0 on error. Must send all bytes or fail. */
    fn_i16 (*send)(void *ctx, const fn_u8 *buf, fn_u16 len);
    /* Read whatever is available (1..max bytes) within timeout_ms, blocking.
       Return the number of bytes read (>=1), 0 on timeout with nothing read, <0 on error.
       Must return as soon as ANY bytes are available -- it must not wait to fill `max`. */
    fn_i16 (*recv)(void *ctx, fn_u8 *buf, fn_u16 max, fn_u32 timeout_ms);
    /* Discard any currently-buffered inbound bytes (best effort). */
    void (*flush_rx)(void *ctx);
} FnTransport;

#endif /* FN_TRANSPORT_H */
