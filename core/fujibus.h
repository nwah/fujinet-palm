#ifndef FUJIBUS_H
#define FUJIBUS_H

#include "fn_types.h"
#include "fn_transport.h"

#define FN_MAX_DATA 512            /* max request payload and max reply payload */
#define FN_HDR_LEN 6
#define FN_INTERBYTE_MS 500        /* timeout for each recv() call once we are mid-frame */
#define FN_DEFAULT_TIMEOUT_MS 15000

#define FN_CMD_ACK 0x06
#define FN_CMD_NAK 0x15

typedef struct FnCtx {
    const FnTransport *t;
    fn_u32 timeout_ms;                       /* overall deadline for the FIRST byte of a reply; default FN_DEFAULT_TIMEOUT_MS, set by fn_init, caller may change afterward */
    fn_u8  rx[FN_HDR_LEN + FN_MAX_DATA + 8]; /* decoded reply frame lands here during fn_bus_call/fn_parse_frame */
    fn_u16 rx_len;                           /* length of the last decoded frame in rx[] */
    fn_u8  stage[64];                        /* scratch for chunked SLIP encode and for chunked transport recv() */
    fn_u8  last_reply_cmd;                   /* FN_CMD_ACK or FN_CMD_NAK from the most recent fn_bus_call */
    fn_u8  scratch[FN_MAX_DATA];             /* general-purpose staging buffer for higher-level wrappers (fn_fuji_*, fn_net_*) to build/receive raw payloads into; fn_bus_call itself never touches this field */
} FnCtx;

void fn_init(FnCtx *ctx, const FnTransport *t);

typedef struct {
    fn_u8  n;          /* number of params, 0-4 */
    fn_u8  width[4];   /* 1, 2, or 4 for each param */
    fn_u32 val[4];
} FnParams;

void fn_params_none(FnParams *p);
void fn_params_add_u8(FnParams *p, fn_u8 v);
void fn_params_add_u16(FnParams *p, fn_u16 v);
void fn_params_add_u32(FnParams *p, fn_u32 v);

/* Performs one request/reply transaction.
   - flushes the receive buffer first
   - builds and sends the SLIP-framed request for device/cmd/params/payload
   - reads and validates the reply (SLIP decode, length, checksum, ACK/NAK, device match)
   - copies up to reply_max bytes of the reply payload into `reply` (may be NULL if reply_max is 0)
   - if reply_len is non-NULL, stores the actual payload length there (even when truncated) */
FnErr fn_bus_call(FnCtx *ctx, fn_u8 dev, fn_u8 cmd, const FnParams *p,
                   const void *data, fn_u16 data_len,
                   void *reply, fn_u16 reply_max, fn_u16 *reply_len);

/* Pure helpers, exposed mainly for unit tests. */
fn_u8  fn_checksum(const fn_u8 *buf, fn_u16 len);
fn_u16 fn_checksum_step(fn_u16 acc, const fn_u8 *buf, fn_u16 len); /* chainable: pass the previous return value as `acc` to continue a running checksum across separate buffers (e.g. header then payload) */

/* Builds a complete SLIP-framed request (identical bytes to what fn_bus_call sends) into `out`.
   Returns the frame length, or 0 if it would not fit in out_cap. */
fn_u16 fn_encode_request(fn_u8 dev, fn_u8 cmd, const FnParams *params,
                          const void *data, fn_u16 data_len,
                          fn_u8 *out, fn_u16 out_cap);

/* Decodes and validates one complete SLIP frame already sitting in memory (both END bytes included,
   leading garbage before the first END is tolerated exactly like fn_bus_call's receive path).
   On success, decoded reply bytes are left in ctx->rx / ctx->rx_len and ctx->last_reply_cmd is set;
   returns (fn_i16) FN_OK. On any framing/length/checksum/NAK problem returns (fn_i16) of the matching FnErr.
   NOTE: this does not know which device was "expected", so it never returns FN_ERR_DEVICE -- that check only
   happens in fn_bus_call, which knows the device it called. Tests that want to check device mismatch should
   call fn_bus_call with a mock transport instead. */
fn_i16 fn_parse_frame(const fn_u8 *frame, fn_u16 len, FnCtx *ctx);

#endif /* FUJIBUS_H */
