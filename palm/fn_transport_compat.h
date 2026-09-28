/* palm/fn_transport_compat.h
 *
 * Fallback copy of the fujinet-core transport interface (core/fn_transport.h
 * and core/fn_types.h). The core agent owns the real files; this copy exists
 * only so the Palm side can build before they land in core/.
 *
 * Nothing here is Palm-specific. Every .c/.h in palm/ reaches this content
 * through the neutral name "fn_transport.h" (see transport_ser.h), never by
 * including this file directly. Each app under palm/apps/ lists core's
 * directory ahead of the generated compat directory on the -I search path,
 * so once core/fn_transport.h exists it wins automatically and this file is
 * never read. Keep this byte-for-byte compatible with core's definition of
 * the interface described in the project spec; do not add anything
 * Palm-specific to it.
 */
#ifndef FN_TRANSPORT_H
#define FN_TRANSPORT_H

#ifndef FN_HAVE_STDINT
typedef unsigned char  fn_u8;
typedef unsigned short fn_u16;
typedef unsigned long  fn_u32;
typedef short          fn_i16;
typedef long           fn_i32;
#else
#include <stdint.h>
typedef uint8_t  fn_u8;
typedef uint16_t fn_u16;
typedef uint32_t fn_u32;
typedef int16_t  fn_i16;
typedef int32_t  fn_i32;
#endif

typedef struct FnTransport {
    void *ctx;

    /* 0 ok, <0 error; must send all `len` bytes before returning. */
    fn_i16 (*send)(void *ctx, const fn_u8 *buf, fn_u16 len);

    /* bytes read (>=1), 0 on timeout, <0 error; return as soon as any
     * bytes are available rather than waiting to fill `max`. */
    fn_i16 (*recv)(void *ctx, fn_u8 *buf, fn_u16 max, fn_u32 timeout_ms);

    void (*flush_rx)(void *ctx);
} FnTransport;

#endif /* FN_TRANSPORT_H */
