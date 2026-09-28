/*
 * fujibus.c -- FujiBus core wire protocol: SLIP framing, checksum, request
 * encoding and the fn_bus_call() request/reply transaction primitive.
 *
 * Portability: ANSI C89. No libc calls anywhere (fn_memcpy/fn_memset below
 * stand in for the string.h equivalents). No static/global mutable state --
 * all mutable state lives in the caller-supplied FnCtx or on the stack. No
 * local array exceeds 64 bytes. Every multi-byte wire field is serialized
 * and deserialized explicitly, byte by byte, little-endian.
 */
#include "fujibus.h"

/* ------------------------------------------------------------------ */
/* Tiny libc replacements (see portability rules).                     */
/* ------------------------------------------------------------------ */

static void fn_memcpy(fn_u8 *dst, const fn_u8 *src, fn_u16 n)
{
    fn_u16 i;

    for (i = 0; i < n; i++) {
        dst[i] = src[i];
    }
}

static void fn_memset(fn_u8 *dst, fn_u8 v, fn_u16 n)
{
    fn_u16 i;

    for (i = 0; i < n; i++) {
        dst[i] = v;
    }
}

/* ------------------------------------------------------------------ */
/* Checksum.                                                            */
/* ------------------------------------------------------------------ */

fn_u16 fn_checksum_step(fn_u16 acc, const fn_u8 *buf, fn_u16 len)
{
    fn_u16 c;
    fn_u16 i;

    c = acc;
    for (i = 0; i < len; i++) {
        c = (fn_u16)(c + buf[i]);
        c = (fn_u16)((c >> 8) + (c & 0xFF));
    }
    return c;
}

fn_u8 fn_checksum(const fn_u8 *buf, fn_u16 len)
{
    return (fn_u8)fn_checksum_step(0, buf, len);
}

/* ------------------------------------------------------------------ */
/* Init / params.                                                       */
/* ------------------------------------------------------------------ */

void fn_init(FnCtx *ctx, const FnTransport *t)
{
    ctx->t = t;
    ctx->timeout_ms = FN_DEFAULT_TIMEOUT_MS;
    ctx->rx_len = 0;
    ctx->last_reply_cmd = 0;
}

void fn_params_none(FnParams *p)
{
    p->n = 0;
}

void fn_params_add_u8(FnParams *p, fn_u8 v)
{
    if (p->n < 4) {
        p->width[p->n] = 1;
        p->val[p->n] = (fn_u32)v;
        p->n = (fn_u8)(p->n + 1);
    }
}

void fn_params_add_u16(FnParams *p, fn_u16 v)
{
    if (p->n < 4) {
        p->width[p->n] = 2;
        p->val[p->n] = (fn_u32)v;
        p->n = (fn_u8)(p->n + 1);
    }
}

void fn_params_add_u32(FnParams *p, fn_u32 v)
{
    if (p->n < 4) {
        p->width[p->n] = 4;
        p->val[p->n] = v;
        p->n = (fn_u8)(p->n + 1);
    }
}

/* ------------------------------------------------------------------ */
/* Request header/descriptor/param-value builder.
 *
 * Fills `hdr` (caller-owned, at least 32 bytes: 5 fixed header bytes + up to
 * 4 descriptor bytes + up to 16 bytes of param values) with everything that
 * precedes the payload: dev, cmd, length (computed from data_len), a
 * placeholder checksum byte (0), the descriptor chain, and the param value
 * bytes. Returns the number of bytes written (NOT including the payload).
 * The caller still owes: checksum-stepping over [hdr,payload], patching
 * hdr[4], and SLIP-encoding everything.
 *
 * Descriptor grouping mirrors FujiBusPacket::serialize(): walk the params in
 * order, greedily grouping consecutive same-width params into a descriptor
 * as long as the group's byte count stays <= 4 (so up to 4 u8s, or up to 2
 * u16s, or exactly 1 u32 per group). All descriptor bytes but the last get
 * the 0x80 continuation bit.
 * ------------------------------------------------------------------ */
static fn_u16 fn_build_header(fn_u8 dev, fn_u8 cmd, const FnParams *params,
                               fn_u16 data_len, fn_u8 *hdr)
{
    fn_u16 pos;
    fn_u16 total_len;
    fn_u8 n;
    fn_u8 ndescr;
    fn_u8 descr[4];
    fn_u8 group_width[4];
    fn_u8 group_count[4];
    fn_u8 i;
    fn_u8 g;
    fn_u8 gi;

    fn_memset(hdr, 0, 32);

    n = (params != 0) ? params->n : 0;

    hdr[0] = dev;
    hdr[1] = cmd;
    hdr[4] = 0; /* checksum placeholder, patched by the caller */

    if (n == 0) {
        ndescr = 1;
        descr[0] = 0;
    } else {
        gi = 0;
        group_width[0] = params->width[0];
        group_count[0] = 1;

        for (i = 1; i < n; i++) {
            if (params->width[i] == group_width[gi] &&
                (fn_u8)((group_count[gi] + 1) * group_width[gi]) <= 4) {
                group_count[gi] = (fn_u8)(group_count[gi] + 1);
            } else {
                gi = (fn_u8)(gi + 1);
                group_width[gi] = params->width[i];
                group_count[gi] = 1;
            }
        }
        ndescr = (fn_u8)(gi + 1);

        for (g = 0; g < ndescr; g++) {
            fn_u8 d;

            d = group_count[g];
            if (group_width[g] > 1) {
                d = (fn_u8)(d + 4);
            }
            if (group_width[g] > 2) {
                d = (fn_u8)(d + 2);
            }
            if ((fn_u8)(g + 1) < ndescr) {
                d = (fn_u8)(d | 0x80);
            }
            descr[g] = d;
        }
    }

    for (g = 0; g < ndescr; g++) {
        hdr[5 + g] = descr[g];
    }

    pos = (fn_u16)(5 + ndescr);

    for (i = 0; i < n; i++) {
        fn_u32 v;

        v = params->val[i];
        if (params->width[i] == 1) {
            hdr[pos] = (fn_u8)(v & 0xFF);
            pos = (fn_u16)(pos + 1);
        } else if (params->width[i] == 2) {
            hdr[pos]     = (fn_u8)(v & 0xFF);
            hdr[pos + 1] = (fn_u8)((v >> 8) & 0xFF);
            pos = (fn_u16)(pos + 2);
        } else {
            hdr[pos]     = (fn_u8)(v & 0xFF);
            hdr[pos + 1] = (fn_u8)((v >> 8) & 0xFF);
            hdr[pos + 2] = (fn_u8)((v >> 16) & 0xFF);
            hdr[pos + 3] = (fn_u8)((v >> 24) & 0xFF);
            pos = (fn_u16)(pos + 4);
        }
    }

    total_len = (fn_u16)(pos + data_len);
    hdr[2] = (fn_u8)(total_len & 0xFF);
    hdr[3] = (fn_u8)((total_len >> 8) & 0xFF);

    return pos;
}

/* ------------------------------------------------------------------ */
/* SLIP encode (buffer form, used by fn_encode_request).                */
/* ------------------------------------------------------------------ */

static fn_bool fn_slip_emit(fn_u8 b, fn_u8 *out, fn_u16 *pos, fn_u16 out_cap)
{
    if (b == 0xC0 || b == 0xDB) {
        if ((fn_u16)(*pos + 2) > out_cap) {
            return FN_FALSE;
        }
        out[*pos]     = 0xDB;
        out[*pos + 1] = (fn_u8)((b == 0xC0) ? 0xDC : 0xDD);
        *pos = (fn_u16)(*pos + 2);
    } else {
        if ((fn_u16)(*pos + 1) > out_cap) {
            return FN_FALSE;
        }
        out[*pos] = b;
        *pos = (fn_u16)(*pos + 1);
    }
    return FN_TRUE;
}

fn_u16 fn_encode_request(fn_u8 dev, fn_u8 cmd, const FnParams *params,
                          const void *data, fn_u16 data_len,
                          fn_u8 *out, fn_u16 out_cap)
{
    fn_u8 hdr[32];
    fn_u16 hdr_len;
    fn_u16 ck;
    const fn_u8 *dp;
    fn_u16 pos;
    fn_u16 i;

    hdr_len = fn_build_header(dev, cmd, params, data_len, hdr);

    ck = fn_checksum_step(0, hdr, hdr_len);
    dp = (const fn_u8 *)data;
    if (data_len > 0 && dp != 0) {
        ck = fn_checksum_step(ck, dp, data_len);
    }
    hdr[4] = (fn_u8)ck;

    if (out_cap < 1) {
        return 0;
    }
    pos = 0;
    out[pos] = 0xC0;
    pos = (fn_u16)(pos + 1);

    for (i = 0; i < hdr_len; i++) {
        if (!fn_slip_emit(hdr[i], out, &pos, out_cap)) {
            return 0;
        }
    }

    if (data_len > 0 && dp != 0) {
        for (i = 0; i < data_len; i++) {
            if (!fn_slip_emit(dp[i], out, &pos, out_cap)) {
                return 0;
            }
        }
    }

    if ((fn_u16)(pos + 1) > out_cap) {
        return 0;
    }
    out[pos] = 0xC0;
    pos = (fn_u16)(pos + 1);

    return pos;
}

/* ------------------------------------------------------------------ */
/* SLIP encode (streamed form, used by fn_bus_call's send path). Bytes are
 * staged through ctx->stage[64] and flushed to ctx->t->send() whenever the
 * staging buffer fills, so no full-frame buffer is ever built.
 * ------------------------------------------------------------------ */

static fn_i16 fn_slip_stage_flush(FnCtx *ctx, fn_u16 *stage_pos)
{
    fn_i16 rc;

    if (*stage_pos == 0) {
        return 0;
    }
    rc = ctx->t->send(ctx->t->ctx, ctx->stage, *stage_pos);
    *stage_pos = 0;
    return rc;
}

static fn_i16 fn_slip_stage_byte(FnCtx *ctx, fn_u8 b, fn_u16 *stage_pos)
{
    fn_i16 rc;

    if (*stage_pos >= (fn_u16)sizeof(ctx->stage)) {
        rc = fn_slip_stage_flush(ctx, stage_pos);
        if (rc < 0) {
            return rc;
        }
    }
    ctx->stage[*stage_pos] = b;
    *stage_pos = (fn_u16)(*stage_pos + 1);
    return 0;
}

static fn_i16 fn_slip_stage_encoded(FnCtx *ctx, fn_u8 b, fn_u16 *stage_pos)
{
    fn_i16 rc;

    if (b == 0xC0 || b == 0xDB) {
        rc = fn_slip_stage_byte(ctx, 0xDB, stage_pos);
        if (rc < 0) {
            return rc;
        }
        rc = fn_slip_stage_byte(ctx, (fn_u8)((b == 0xC0) ? 0xDC : 0xDD), stage_pos);
        if (rc < 0) {
            return rc;
        }
    } else {
        rc = fn_slip_stage_byte(ctx, b, stage_pos);
        if (rc < 0) {
            return rc;
        }
    }
    return 0;
}

static fn_i16 fn_slip_stage_block(FnCtx *ctx, const fn_u8 *data, fn_u16 len,
                                   fn_u16 *stage_pos)
{
    fn_u16 i;
    fn_i16 rc;

    for (i = 0; i < len; i++) {
        rc = fn_slip_stage_encoded(ctx, data[i], stage_pos);
        if (rc < 0) {
            return rc;
        }
    }
    return 0;
}

/* ------------------------------------------------------------------ */
/* SLIP decode state machine, fed one byte at a time. Shared by
 * fn_parse_frame (fed a whole in-memory buffer) and fn_bus_recv_frame (fed
 * successive transport recv() chunks). Returns FN_TRUE exactly when this
 * byte completes a frame; decoded bytes accumulate in ctx->rx.
 * ------------------------------------------------------------------ */

static fn_bool fn_slip_decode_byte(fn_u8 b, FnCtx *ctx, fn_bool *in_frame,
                                    fn_bool *esc_pending, fn_u16 *decoded_len,
                                    fn_bool *overflow)
{
    fn_u8 out;

    out = 0;

    if (b == 0xC0) {
        if (*decoded_len == 0) {
            /* Fresh start marker (also absorbs a stray leading/empty C0). */
            *in_frame = FN_TRUE;
            *esc_pending = FN_FALSE;
            *overflow = FN_FALSE;
            return FN_FALSE;
        }
        return FN_TRUE;
    }

    if (!*in_frame) {
        return FN_FALSE; /* leading garbage before the first END */
    }

    if (*esc_pending) {
        *esc_pending = FN_FALSE;
        if (b == 0xDC) {
            out = 0xC0;
        } else if (b == 0xDD) {
            out = 0xDB;
        } else {
            return FN_FALSE; /* malformed escape: drop it */
        }
    } else if (b == 0xDB) {
        *esc_pending = FN_TRUE;
        return FN_FALSE;
    } else {
        out = b;
    }

    if (*decoded_len < (fn_u16)sizeof(ctx->rx)) {
        ctx->rx[*decoded_len] = out;
        *decoded_len = (fn_u16)(*decoded_len + 1);
    } else {
        *overflow = FN_TRUE;
    }

    return FN_FALSE;
}

/* Validates a just-decoded frame sitting in ctx->rx[0..decoded_len) and, on
 * success, sets ctx->rx_len / ctx->last_reply_cmd. Shared by fn_parse_frame
 * and fn_bus_recv_frame; neither knows an "expected device" here. */
static FnErr fn_validate_decoded_frame(FnCtx *ctx, fn_u16 decoded_len, fn_bool overflow)
{
    fn_u16 wire_len;
    fn_u8 ck1;
    fn_u8 ck2;

    if (overflow) {
        return FN_ERR_LENGTH;
    }
    if (decoded_len < FN_HDR_LEN) {
        return FN_ERR_LENGTH;
    }

    wire_len = (fn_u16)(ctx->rx[2] | (ctx->rx[3] << 8));
    if (wire_len != decoded_len) {
        return FN_ERR_LENGTH;
    }

    ck1 = ctx->rx[4];
    ctx->rx[4] = 0;
    ck2 = fn_checksum(ctx->rx, decoded_len);
    if (ck1 != ck2) {
        return FN_ERR_CHECKSUM;
    }

    ctx->rx_len = decoded_len;
    ctx->last_reply_cmd = ctx->rx[1];

    if (ctx->last_reply_cmd == FN_CMD_NAK) {
        return FN_ERR_NAK;
    }
    if (ctx->last_reply_cmd != FN_CMD_ACK) {
        return FN_ERR_LENGTH; /* replies are only ever ACK or NAK */
    }

    return FN_OK;
}

fn_i16 fn_parse_frame(const fn_u8 *frame, fn_u16 len, FnCtx *ctx)
{
    fn_bool in_frame;
    fn_bool esc_pending;
    fn_u16 decoded_len;
    fn_bool overflow;
    fn_bool complete;
    fn_u16 i;
    FnErr err;

    in_frame = FN_FALSE;
    esc_pending = FN_FALSE;
    decoded_len = 0;
    overflow = FN_FALSE;
    complete = FN_FALSE;

    for (i = 0; i < len; i++) {
        complete = fn_slip_decode_byte(frame[i], ctx, &in_frame, &esc_pending,
                                        &decoded_len, &overflow);
        if (complete) {
            break;
        }
    }

    if (!complete) {
        return (fn_i16)FN_ERR_LENGTH;
    }

    err = fn_validate_decoded_frame(ctx, decoded_len, overflow);
    return (fn_i16)err;
}

/* ------------------------------------------------------------------ */
/* fn_bus_call's receive path: reads via ctx->t->recv() into ctx->stage in
 * a loop, feeding bytes through the shared decoder, then validates and
 * checks the device byte (the one thing fn_parse_frame can't do).
 * ------------------------------------------------------------------ */

static FnErr fn_bus_recv_frame(FnCtx *ctx, fn_u8 dev)
{
    fn_bool in_frame;
    fn_bool esc_pending;
    fn_u16 decoded_len;
    fn_bool overflow;
    fn_bool complete;
    fn_bool got_any;
    fn_u32 timeout;
    fn_i16 n;
    fn_u16 i;
    FnErr err;

    in_frame = FN_FALSE;
    esc_pending = FN_FALSE;
    decoded_len = 0;
    overflow = FN_FALSE;
    complete = FN_FALSE;
    got_any = FN_FALSE;

    while (!complete) {
        timeout = got_any ? (fn_u32)FN_INTERBYTE_MS : ctx->timeout_ms;
        n = ctx->t->recv(ctx->t->ctx, ctx->stage, (fn_u16)sizeof(ctx->stage), timeout);

        if (n == 0) {
            return FN_ERR_TIMEOUT;
        }
        if (n < 0) {
            return FN_ERR_IO;
        }

        got_any = FN_TRUE;

        for (i = 0; i < (fn_u16)n; i++) {
            complete = fn_slip_decode_byte(ctx->stage[i], ctx, &in_frame,
                                            &esc_pending, &decoded_len, &overflow);
            if (complete) {
                break;
            }
        }
    }

    err = fn_validate_decoded_frame(ctx, decoded_len, overflow);
    if (err != FN_OK) {
        return err;
    }

    if (ctx->rx[0] != dev) {
        return FN_ERR_DEVICE;
    }

    return FN_OK;
}

/* ------------------------------------------------------------------ */
/* fn_bus_call.                                                         */
/* ------------------------------------------------------------------ */

FnErr fn_bus_call(FnCtx *ctx, fn_u8 dev, fn_u8 cmd, const FnParams *p,
                   const void *data, fn_u16 data_len,
                   void *reply, fn_u16 reply_max, fn_u16 *reply_len)
{
    fn_u8 hdr[32];
    fn_u16 hdr_len;
    fn_u16 ck;
    const fn_u8 *dp;
    fn_u16 stage_pos;
    fn_i16 rc;
    FnErr err;
    fn_u16 payload_len;
    fn_u16 copy_len;

    ctx->t->flush_rx(ctx->t->ctx);

    hdr_len = fn_build_header(dev, cmd, p, data_len, hdr);

    ck = fn_checksum_step(0, hdr, hdr_len);
    dp = (const fn_u8 *)data;
    if (data_len > 0 && dp != 0) {
        ck = fn_checksum_step(ck, dp, data_len);
    }
    hdr[4] = (fn_u8)ck;

    stage_pos = 0;

    rc = fn_slip_stage_byte(ctx, 0xC0, &stage_pos);
    if (rc < 0) {
        return FN_ERR_IO;
    }

    rc = fn_slip_stage_block(ctx, hdr, hdr_len, &stage_pos);
    if (rc < 0) {
        return FN_ERR_IO;
    }

    if (data_len > 0 && dp != 0) {
        rc = fn_slip_stage_block(ctx, dp, data_len, &stage_pos);
        if (rc < 0) {
            return FN_ERR_IO;
        }
    }

    rc = fn_slip_stage_byte(ctx, 0xC0, &stage_pos);
    if (rc < 0) {
        return FN_ERR_IO;
    }

    rc = fn_slip_stage_flush(ctx, &stage_pos);
    if (rc < 0) {
        return FN_ERR_IO;
    }

    err = fn_bus_recv_frame(ctx, dev);
    if (err != FN_OK) {
        return err;
    }

    payload_len = (fn_u16)(ctx->rx_len - FN_HDR_LEN);

    copy_len = payload_len;
    if (copy_len > reply_max) {
        copy_len = reply_max;
    }

    if (reply != 0 && copy_len > 0) {
        fn_memcpy((fn_u8 *)reply, &ctx->rx[FN_HDR_LEN], copy_len);
    }

    if (reply_len != 0) {
        *reply_len = payload_len;
    }

    if (payload_len > reply_max) {
        return FN_ERR_OVERFLOW;
    }

    return FN_OK;
}
