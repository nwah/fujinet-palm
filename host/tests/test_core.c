/*
 * test_core.c - unit tests for the FujiBus transport-level engine
 * (fn_checksum / fn_encode_request / fn_parse_frame / fn_bus_call).
 *
 * Written against the FIXED spec for core/fn_types.h, core/fn_transport.h
 * and core/fujibus.h/.c (developed in parallel). Do not assume anything
 * about the core implementation beyond that spec.
 *
 * Build: cc -Wall -Wextra -I../core -o test_core test_core.c ../core/fujibus.c
 */

#include <stdio.h>
#include <string.h>

#include "fn_types.h"
#include "fn_transport.h"
#include "fujibus.h"

/* -------------------------------------------------------------------- */
/* Test harness                                                          */
/* -------------------------------------------------------------------- */

static int g_fail_count = 0;
static int g_test_count = 0;

#define CHECK(cond, msg) do { \
    g_test_count++; \
    if (!(cond)) { \
        g_fail_count++; \
        fprintf(stderr, "FAIL: %s (%s:%d)\n", msg, __FILE__, __LINE__); \
    } \
} while (0)

/* -------------------------------------------------------------------- */
/* Small local reference helpers (independent of the code under test)   */
/* -------------------------------------------------------------------- */

/* Reference SLIP encoder: wraps `in_len` decoded bytes into a framed
 * (leading/trailing 0xC0, escaped) buffer. Returns bytes written. */
static fn_u16 ref_slip_encode(const fn_u8 *in, fn_u16 in_len,
                               fn_u8 *out, fn_u16 out_cap)
{
    fn_u16 o = 0;
    fn_u16 i;
    (void)out_cap; /* test buffers are always sized generously */
    out[o++] = 0xC0;
    for (i = 0; i < in_len; i++) {
        fn_u8 b = in[i];
        if (b == 0xC0) {
            out[o++] = 0xDB;
            out[o++] = 0xDC;
        } else if (b == 0xDB) {
            out[o++] = 0xDB;
            out[o++] = 0xDD;
        } else {
            out[o++] = b;
        }
    }
    out[o++] = 0xC0;
    return o;
}

/* Reference SLIP decoder: finds the first 0xC0 in `frame`, then
 * unescapes bytes until the next 0xC0 (or end of buffer). Returns 1 on
 * success (out/out_len filled), 0 on malformed input. */
static int ref_slip_decode(const fn_u8 *frame, fn_u16 frame_len,
                            fn_u8 *out, fn_u16 *out_len)
{
    fn_u16 i = 0;
    fn_u16 o = 0;

    while (i < frame_len && frame[i] != 0xC0) i++;
    if (i >= frame_len) return 0;
    i++; /* skip leading END */

    while (i < frame_len && frame[i] != 0xC0) {
        fn_u8 b = frame[i++];
        if (b == 0xDB) {
            if (i >= frame_len) return 0;
            b = frame[i++];
            if (b == 0xDC) out[o++] = 0xC0;
            else if (b == 0xDD) out[o++] = 0xDB;
            else return 0;
        } else {
            out[o++] = b;
        }
    }
    *out_len = o;
    return 1;
}

/* -------------------------------------------------------------------- */
/* Mock transport, used by the fn_bus_call tests                        */
/* -------------------------------------------------------------------- */

#define MOCK_BUF_SIZE 700

typedef struct MockCtx {
    fn_u8  sent[MOCK_BUF_SIZE];
    fn_u16 sent_len;

    const fn_u8 *reply;
    fn_u16       reply_len;
    fn_u16       reply_pos;
    fn_u16       chunk_size; /* 0 = deliver everything remaining in one call */
} MockCtx;

static void mock_reset(MockCtx *m, const fn_u8 *reply, fn_u16 reply_len,
                        fn_u16 chunk_size)
{
    m->sent_len = 0;
    memset(m->sent, 0, sizeof(m->sent));
    m->reply = reply;
    m->reply_len = reply_len;
    m->reply_pos = 0;
    m->chunk_size = chunk_size;
}

static fn_i16 mock_send(void *ctx, const fn_u8 *buf, fn_u16 len)
{
    MockCtx *m = (MockCtx *)ctx;
    memcpy(m->sent + m->sent_len, buf, len);
    m->sent_len = (fn_u16)(m->sent_len + len);
    return (fn_i16)len;
}

static fn_i16 mock_recv(void *ctx, fn_u8 *buf, fn_u16 max, fn_u32 timeout_ms)
{
    MockCtx *m = (MockCtx *)ctx;
    fn_u16 remaining;
    fn_u16 n;

    (void)timeout_ms;

    remaining = (fn_u16)(m->reply_len - m->reply_pos);
    if (remaining == 0) {
        /* No more scripted data: this is indistinguishable from a real
         * timeout per the transport contract (a 0 return). */
        return 0;
    }

    n = (m->chunk_size == 0) ? remaining : m->chunk_size;
    if (n > remaining) n = remaining;
    if (n > max) n = max;

    memcpy(buf, m->reply + m->reply_pos, n);
    m->reply_pos = (fn_u16)(m->reply_pos + n);
    return (fn_i16)n;
}

static void mock_flush(void *ctx)
{
    (void)ctx;
}

/* Dummy transport used only for fn_parse_frame() tests, where the
 * callbacks are never expected to be invoked, but we still want a
 * fully-formed, non-NULL FnTransport to hand to fn_init(). */
static fn_i16 dummy_send(void *ctx, const fn_u8 *buf, fn_u16 len)
{
    (void)ctx; (void)buf; (void)len;
    return 0;
}
static fn_i16 dummy_recv(void *ctx, fn_u8 *buf, fn_u16 max, fn_u32 timeout_ms)
{
    (void)ctx; (void)buf; (void)max; (void)timeout_ms;
    return 0;
}
static void dummy_flush(void *ctx)
{
    (void)ctx;
}

static const FnTransport g_dummy_transport = {
    NULL, dummy_send, dummy_recv, dummy_flush
};

/* -------------------------------------------------------------------- */
/* Verified-correct wire vectors                                        */
/* -------------------------------------------------------------------- */

static const fn_u8 VEC1[] = { 0xC0, 0x70, 0x00, 0x06, 0x00, 0x76, 0x00, 0xC0 };
static const fn_u8 VEC2[] = { 0xC0, 0x70, 0xC4, 0x06, 0x00, 0x3B, 0x00, 0xC0 };
static const fn_u8 VEC3[] = { 0xC0, 0x71, 0x52, 0x08, 0x00, 0xD1, 0x05, 0x00, 0x01, 0xC0 };
static const fn_u8 VEC4[] = { 0xC0, 0x31, 0x52, 0x0A, 0x00, 0x94, 0x07, 0x00, 0x00, 0x00, 0x00, 0xC0 };
static const fn_u8 VEC5[] = { 0xC0, 0x70, 0x06, 0x06, 0x00, 0x7C, 0x00, 0xC0 };
static const fn_u8 VEC6[] = { 0xC0, 0x70, 0x15, 0x06, 0x00, 0x8B, 0x00, 0xC0 };
static const fn_u8 VEC7_BUF[] = { 0x70, 0x00, 0x06, 0x00, 0x00, 0x00 }; /* checksum byte zeroed */
static const fn_u8 VEC8_BUF[] = { 0x71, 0x52, 0x08, 0x00, 0x00, 0x05, 0x00, 0x01 }; /* checksum byte zeroed */

/* -------------------------------------------------------------------- */
/* Test 1: fn_checksum / fn_checksum_step                                */
/* -------------------------------------------------------------------- */

static void test_checksum(void)
{
    fn_u8 c7 = fn_checksum(VEC7_BUF, (fn_u16)sizeof(VEC7_BUF));
    fn_u8 c8 = fn_checksum(VEC8_BUF, (fn_u16)sizeof(VEC8_BUF));
    fn_u16 chained;
    fn_u8 whole;

    CHECK(c7 == 0x76, "fn_checksum vector 7 (== 0x76)");
    CHECK(c8 == 0xD1, "fn_checksum vector 8 (== 0xD1)");

    /* Chaining: splitting VEC7_BUF (6 bytes) into two calls of 3 bytes
     * each via fn_checksum_step must equal one call to fn_checksum. */
    chained = fn_checksum_step(fn_checksum_step(0, VEC7_BUF, 3), VEC7_BUF + 3, 3);
    whole = fn_checksum(VEC7_BUF, (fn_u16)sizeof(VEC7_BUF));
    CHECK((fn_u8)chained == whole, "fn_checksum_step chaining matches fn_checksum");
}

/* -------------------------------------------------------------------- */
/* Test 2: SLIP escaping round-trip                                     */
/* -------------------------------------------------------------------- */

static void test_slip_roundtrip(void)
{
    /* Decoded payload deliberately exercising every escape case:
     *  [0]  lone 0xC0 (also: at the very start)
     *  [2]  lone 0xC0 (not adjacent to a 0xDB, not at an edge)
     *  [4]  lone 0xDB
     *  [6],[7]  0xC0 immediately followed by 0xDB
     *  [9],[10] 0xDB immediately followed by 0xC0
     *  [12] a 0xDB at the very end
     */
    static const fn_u8 payload[] = {
        0xC0,
        0x01,
        0xC0,
        0x02,
        0xDB,
        0x03,
        0xC0, 0xDB,
        0x04,
        0xDB, 0xC0,
        0x05,
        0xDB
    };
    const fn_u16 payload_len = (fn_u16)sizeof(payload);

    fn_u8 hdr_and_payload[6 + sizeof(payload)];
    fn_u8 expected[2 * sizeof(hdr_and_payload) + 4];
    fn_u16 expected_len;
    fn_u8 actual[2 * sizeof(hdr_and_payload) + 4];
    fn_u16 actual_len;
    FnParams p;
    fn_u8 chk;
    fn_u16 decoded_len;
    fn_u8 decoded[sizeof(hdr_and_payload)];
    FnCtx ctx;

    fn_params_none(&p);

    /* Build the expected decoded header+payload ourselves (dev=0x70,
     * cmd=FN_CMD_ACK, no params -> descr=0x00), independently of
     * fn_encode_request, to derive an expected SLIP-encoded frame via
     * our own reference escaper. cmd must be ACK (or NAK) here, not an
     * arbitrary request command, because this buffer is also fed through
     * fn_parse_frame below to check the SLIP decode round-trip, and
     * fn_parse_frame is a *reply* parser -- it rejects any cmd byte other
     * than ACK/NAK as a protocol violation (FN_ERR_LENGTH), independent of
     * SLIP/checksum correctness. */
    hdr_and_payload[0] = 0x70;                                  /* dev */
    hdr_and_payload[1] = FN_CMD_ACK;                            /* cmd */
    hdr_and_payload[2] = (fn_u8)((6 + payload_len) & 0xFF);     /* len lo */
    hdr_and_payload[3] = (fn_u8)((6 + payload_len) >> 8);       /* len hi */
    hdr_and_payload[4] = 0;                                     /* checksum, zeroed for now */
    hdr_and_payload[5] = 0x00;                                  /* descr, n=0 params */
    memcpy(hdr_and_payload + 6, payload, payload_len);

    chk = fn_checksum(hdr_and_payload, (fn_u16)sizeof(hdr_and_payload));
    hdr_and_payload[4] = chk;

    expected_len = ref_slip_encode(hdr_and_payload, (fn_u16)sizeof(hdr_and_payload),
                                    expected, (fn_u16)sizeof(expected));

    actual_len = fn_encode_request(0x70, FN_CMD_ACK, &p, payload, payload_len,
                                    actual, (fn_u16)sizeof(actual));

    CHECK(actual_len == expected_len, "SLIP roundtrip: encoded length matches reference");
    CHECK(actual_len == expected_len &&
          memcmp(actual, expected, expected_len) == 0,
          "SLIP roundtrip: encoded bytes match reference escaper");

    /* Now decode the real fn_encode_request output via fn_parse_frame
     * and confirm the payload comes back byte-for-byte. */
    fn_init(&ctx, &g_dummy_transport);
    {
        fn_i16 rc = fn_parse_frame(actual, actual_len, &ctx);
        CHECK(rc == (fn_i16)FN_OK, "SLIP roundtrip: fn_parse_frame succeeds");
        CHECK(ctx.rx_len == (fn_u16)(6 + payload_len),
              "SLIP roundtrip: decoded rx_len matches header+payload length");
        CHECK(memcmp(ctx.rx + FN_HDR_LEN, payload, payload_len) == 0,
              "SLIP roundtrip: decoded payload matches original");
    }

    /* Sanity-check our own reference decoder too, since it is reused
     * later for the length-field endianness test. */
    CHECK(ref_slip_decode(actual, actual_len, decoded, &decoded_len) &&
          decoded_len == (fn_u16)sizeof(hdr_and_payload) &&
          memcmp(decoded, hdr_and_payload, decoded_len) == 0,
          "SLIP roundtrip: reference decoder recovers original decoded bytes");
}

/* -------------------------------------------------------------------- */
/* Test 3: wire vectors 1-4 via fn_encode_request                        */
/* -------------------------------------------------------------------- */

static void test_wire_vectors(void)
{
    fn_u8 out[64];
    fn_u16 out_len;
    FnParams p;

    /* Vector 1: DEVICE_READY test, no params. */
    fn_params_none(&p);
    out_len = fn_encode_request(0x70, 0x00, &p, NULL, 0, out, (fn_u16)sizeof(out));
    CHECK(out_len == (fn_u16)sizeof(VEC1) && memcmp(out, VEC1, sizeof(VEC1)) == 0,
          "wire vector 1 (DEVICE_READY)");

    /* Vector 2: GET_ADAPTERCONFIG_EXTENDED, no params. */
    fn_params_none(&p);
    out_len = fn_encode_request(0x70, 0xC4, &p, NULL, 0, out, (fn_u16)sizeof(out));
    CHECK(out_len == (fn_u16)sizeof(VEC2) && memcmp(out, VEC2, sizeof(VEC2)) == 0,
          "wire vector 2 (GET_ADAPTERCONFIG_EXTENDED)");

    /* Vector 3: one u16 param = 256. */
    fn_params_none(&p);
    fn_params_add_u16(&p, 256);
    out_len = fn_encode_request(0x71, 0x52, &p, NULL, 0, out, (fn_u16)sizeof(out));
    CHECK(out_len == (fn_u16)sizeof(VEC3) && memcmp(out, VEC3, sizeof(VEC3)) == 0,
          "wire vector 3 (one u16 param)");

    /* Vector 4: one u32 param = 0. */
    fn_params_none(&p);
    fn_params_add_u32(&p, 0);
    out_len = fn_encode_request(0x31, 0x52, &p, NULL, 0, out, (fn_u16)sizeof(out));
    CHECK(out_len == (fn_u16)sizeof(VEC4) && memcmp(out, VEC4, sizeof(VEC4)) == 0,
          "wire vector 4 (one u32 param)");
}

/* -------------------------------------------------------------------- */
/* Test 4: mixed-width param vector                                      */
/* -------------------------------------------------------------------- */

/*
 * NOTE ON DEVIATION FROM THE ORIGINAL 5-PARAM EXAMPLE:
 *
 * FnParams is defined by the fixed core spec as:
 *   typedef struct { fn_u8 n; fn_u8 width[4]; fn_u32 val[4]; } FnParams;
 * i.e. it can hold AT MOST 4 individual parameters (n <= 4), since
 * width[] and val[] are fixed 4-element arrays with one slot per raw
 * parameter (not per wire descriptor/group). A 5-parameter list
 * (four u8 values plus one u16 value) therefore cannot be constructed
 * through fn_params_add_u8/fn_params_add_u16 at all -- the 5th add
 * call would have no slot to write into.
 *
 * This test instead uses a 4-parameter list -- three u8 values
 * (0x01, 0x02, 0x03) followed by one u16 value (0xABCD) -- which fits
 * within FnParams's capacity while still exercising the same
 * greedy same-width-grouping rule non-trivially: a *partial* u8 group
 * (3 of a possible 4) followed by a u16 group.
 *
 * Applying the grouping rule:
 *   group 1: three u8 params, count=3, width=1 -> descriptor byte
 *            = 3 (no +4/+2, width==1), continuation bit set since a
 *            second group follows -> 0x83
 *   group 2: one u16 param, count=1, width=2 -> descriptor byte
 *            = 1 + 4 (width>1) = 5, last group so no continuation
 *            bit -> 0x05
 * Decoded packet: dev, cmd, len_lo, len_hi, chk, 0x83 (header, 6
 * bytes), then body: 0x05 (2nd descriptor), 0x01, 0x02, 0x03 (u8
 * values), 0xCD, 0xAB (u16 LE) -- 6 body bytes, 12 total.
 */
static void test_mixed_width(void)
{
    fn_u8 decoded[12];
    fn_u8 chk;
    fn_u8 expected[32];
    fn_u16 expected_len;
    fn_u8 actual[32];
    fn_u16 actual_len;
    FnParams p;

    decoded[0] = 0x70;             /* dev */
    decoded[1] = 0xC4;             /* cmd */
    decoded[2] = 0x0C;             /* len lo: total 12 = 0x000C */
    decoded[3] = 0x00;             /* len hi */
    decoded[4] = 0;                /* checksum, zeroed for now */
    decoded[5] = 0x83;             /* descriptor 1: count=3, width=1, continuation set */
    decoded[6] = 0x05;             /* descriptor 2: count=1, width=2 (u16), last */
    decoded[7] = 0x01;
    decoded[8] = 0x02;
    decoded[9] = 0x03;
    decoded[10] = 0xCD;            /* 0xABCD little-endian, low byte */
    decoded[11] = 0xAB;            /* 0xABCD little-endian, high byte */

    chk = fn_checksum(decoded, (fn_u16)sizeof(decoded));
    decoded[4] = chk;

    expected_len = ref_slip_encode(decoded, (fn_u16)sizeof(decoded),
                                    expected, (fn_u16)sizeof(expected));

    fn_params_none(&p);
    fn_params_add_u8(&p, 0x01);
    fn_params_add_u8(&p, 0x02);
    fn_params_add_u8(&p, 0x03);
    fn_params_add_u16(&p, 0xABCD);

    actual_len = fn_encode_request(0x70, 0xC4, &p, NULL, 0,
                                    actual, (fn_u16)sizeof(actual));

    CHECK(actual_len == expected_len,
          "mixed-width vector: encoded length matches hand-assembled reference");
    CHECK(actual_len == expected_len &&
          memcmp(actual, expected, expected_len) == 0,
          "mixed-width vector: encoded bytes match hand-assembled reference");
}

/* -------------------------------------------------------------------- */
/* Test 5: reply parsing via fn_parse_frame                              */
/* -------------------------------------------------------------------- */

static void test_reply_parsing(void)
{
    FnCtx ctx;

    /* ACK with data: build via fn_encode_request as a synthetic
     * "reply-shaped" frame (wire format is identical for requests and
     * replies). */
    {
        static const fn_u8 payload[] = { 0xAA, 0xBB, 0xCC, 0xDD, 0xEE };
        FnParams p;
        fn_u8 frame[64];
        fn_u16 frame_len;
        fn_i16 rc;

        fn_params_none(&p);
        frame_len = fn_encode_request(0x70, FN_CMD_ACK, &p, payload,
                                       (fn_u16)sizeof(payload),
                                       frame, (fn_u16)sizeof(frame));

        fn_init(&ctx, &g_dummy_transport);
        rc = fn_parse_frame(frame, frame_len, &ctx);
        CHECK(rc == (fn_i16)FN_OK, "ACK-with-data: fn_parse_frame returns FN_OK");
        CHECK(ctx.last_reply_cmd == FN_CMD_ACK, "ACK-with-data: last_reply_cmd is ACK");
        CHECK(ctx.rx_len == (fn_u16)(FN_HDR_LEN + sizeof(payload)),
              "ACK-with-data: rx_len covers header+payload");
        CHECK(memcmp(ctx.rx + FN_HDR_LEN, payload, sizeof(payload)) == 0,
              "ACK-with-data: decoded payload bytes match");
    }

    /* ACK with no data (vector 5). */
    {
        fn_i16 rc;
        fn_init(&ctx, &g_dummy_transport);
        rc = fn_parse_frame(VEC5, (fn_u16)sizeof(VEC5), &ctx);
        CHECK(rc == (fn_i16)FN_OK, "ACK-no-data (vector 5): fn_parse_frame returns FN_OK");
        CHECK(ctx.last_reply_cmd == FN_CMD_ACK, "ACK-no-data: last_reply_cmd is ACK");
        CHECK(ctx.rx_len == 6, "ACK-no-data: rx_len == 6 (header only)");
    }

    /* NAK (vector 6). */
    {
        fn_i16 rc;
        fn_init(&ctx, &g_dummy_transport);
        rc = fn_parse_frame(VEC6, (fn_u16)sizeof(VEC6), &ctx);
        CHECK(rc == (fn_i16)FN_ERR_NAK, "NAK (vector 6): fn_parse_frame returns FN_ERR_NAK");
    }

    /* Bad checksum: corrupt vector 5's checksum byte. */
    {
        fn_u8 bad[sizeof(VEC5)];
        fn_i16 rc;
        memcpy(bad, VEC5, sizeof(VEC5));
        CHECK(bad[5] == 0x7C, "sanity: VEC5 checksum byte is at index 5");
        bad[5] = 0x00; /* definitely wrong */
        fn_init(&ctx, &g_dummy_transport);
        rc = fn_parse_frame(bad, (fn_u16)sizeof(bad), &ctx);
        CHECK(rc == (fn_i16)FN_ERR_CHECKSUM, "bad checksum: fn_parse_frame returns FN_ERR_CHECKSUM");
    }

    /* Wrong length: declared length (6) does not match the actual
     * number of decoded bytes (7). Built by hand, checksum derived via
     * fn_checksum (not hand-computed), then SLIP-encoded via our own
     * reference escaper. */
    {
        fn_u8 decoded[7];
        fn_u8 frame[32];
        fn_u16 frame_len;
        fn_i16 rc;

        decoded[0] = 0x70;  /* dev */
        decoded[1] = 0x06;  /* cmd = ACK */
        decoded[2] = 0x06;  /* declared len lo = 6 (header only) */
        decoded[3] = 0x00;  /* declared len hi */
        decoded[4] = 0;     /* checksum, zeroed for now */
        decoded[5] = 0x00;  /* descr, n=0 params */
        decoded[6] = 0x99;  /* extra trailing byte not accounted for by declared length */

        decoded[4] = fn_checksum(decoded, 6);

        frame_len = ref_slip_encode(decoded, (fn_u16)sizeof(decoded),
                                     frame, (fn_u16)sizeof(frame));

        fn_init(&ctx, &g_dummy_transport);
        rc = fn_parse_frame(frame, frame_len, &ctx);
        CHECK(rc == (fn_i16)FN_ERR_LENGTH, "wrong length: fn_parse_frame returns FN_ERR_LENGTH");
    }

    /* Leading garbage before the first 0xC0. */
    {
        fn_u8 buf[3 + sizeof(VEC5)];
        fn_i16 rc;
        buf[0] = 0x01; buf[1] = 0x02; buf[2] = 0x03;
        memcpy(buf + 3, VEC5, sizeof(VEC5));
        fn_init(&ctx, &g_dummy_transport);
        rc = fn_parse_frame(buf, (fn_u16)sizeof(buf), &ctx);
        CHECK(rc == (fn_i16)FN_OK, "leading garbage: fn_parse_frame still succeeds");
        CHECK(ctx.last_reply_cmd == FN_CMD_ACK, "leading garbage: correctly decoded ACK");
    }

    /* Empty C0 C0 frame immediately followed by a real frame. */
    {
        fn_u8 buf[2 + sizeof(VEC5)];
        fn_i16 rc;
        buf[0] = 0xC0; buf[1] = 0xC0;
        memcpy(buf + 2, VEC5, sizeof(VEC5));
        fn_init(&ctx, &g_dummy_transport);
        rc = fn_parse_frame(buf, (fn_u16)sizeof(buf), &ctx);
        CHECK(rc == (fn_i16)FN_OK, "empty C0 C0 frame: fn_parse_frame still succeeds");
        CHECK(ctx.last_reply_cmd == FN_CMD_ACK, "empty C0 C0 frame: correctly decoded ACK");
    }

    /* Overflow truncation and wrong-device are fn_bus_call-level
     * concerns (fn_parse_frame has no notion of a separate reply
     * buffer or an expected device) -- see test_mock_transport(). */
}

/* -------------------------------------------------------------------- */
/* Test 6: request header length field is little-endian                  */
/* -------------------------------------------------------------------- */

static void test_length_endianness(void)
{
    /* Vector 3's frame has no bytes needing escaping, so encoded-frame
     * bytes line up 1:1 with decoded-packet bytes (after the leading
     * 0xC0). Check the length field byte order directly. */
    CHECK((fn_u16)sizeof(VEC3) >= 5, "vector 3 sanity length");
    CHECK(VEC3[3] == 0x08 && VEC3[4] == 0x00,
          "vector 3: length field bytes are 08 00 (low byte first)");

    /* Build a request whose decoded length exceeds 255 (6-byte header +
     * 250-byte payload of a fixed non-special value = 256 = 0x0100),
     * so the high length byte is meaningfully nonzero. To sidestep any
     * accidental escaping-driven offset shifts (e.g. if the computed
     * checksum happens to equal 0xC0/0xDB), decode the frame back with
     * our own reference SLIP decoder before checking offsets. */
    {
        fn_u8 payload[250];
        fn_u8 frame[700];
        fn_u16 frame_len;
        fn_u8 decoded[300];
        fn_u16 decoded_len;
        FnParams p;
        fn_u16 i;

        for (i = 0; i < (fn_u16)sizeof(payload); i++) payload[i] = 0x42;

        fn_params_none(&p);
        frame_len = fn_encode_request(0x70, 0x01, &p, payload, (fn_u16)sizeof(payload),
                                       frame, (fn_u16)sizeof(frame));

        CHECK(ref_slip_decode(frame, frame_len, decoded, &decoded_len),
              "256-byte-length case: reference decoder parses the frame");
        CHECK(decoded_len == 256,
              "256-byte-length case: decoded length is 6 (header) + 250 (payload) = 256");
        CHECK(decoded_len == 256 && decoded[2] == 0x00 && decoded[3] == 0x01,
              "256-byte-length case: length field bytes are 00 01 (low byte first, 0x0100)");
    }
}

/* -------------------------------------------------------------------- */
/* Test 7: mock transport + fn_bus_call                                  */
/* -------------------------------------------------------------------- */

static void test_mock_transport(void)
{
    MockCtx m;
    FnTransport t;
    FnCtx ctx;

    t.ctx = &m;
    t.send = mock_send;
    t.recv = mock_recv;
    t.flush_rx = mock_flush;

    /* Timeout mid-frame: script only a partial frame (leading 0xC0 plus
     * 2 more bytes); recv() will return 0 once the scripted bytes are
     * exhausted, since fn_bus_call's receive loop needs more. */
    {
        static const fn_u8 partial[] = { 0xC0, 0x70, 0x06 };
        FnErr err;

        mock_reset(&m, partial, (fn_u16)sizeof(partial), 0);
        fn_init(&ctx, &t);
        err = fn_bus_call(&ctx, 0x70, 0x00, NULL, NULL, 0, NULL, 0, NULL);
        CHECK(err == FN_ERR_TIMEOUT, "timeout mid-frame: fn_bus_call returns FN_ERR_TIMEOUT");
    }

    /* Byte-at-a-time chunking: script a full ACK-with-payload reply,
     * delivered one byte per recv() call. */
    {
        static const fn_u8 payload[] = { 0xAA, 0xBB, 0xCC, 0xDD, 0xEE };
        FnParams p;
        fn_u8 scripted[64];
        fn_u16 scripted_len;
        fn_u8 replybuf[64];
        fn_u16 replylen = 0;
        FnErr err;

        fn_params_none(&p);
        scripted_len = fn_encode_request(0x70, FN_CMD_ACK, &p, payload,
                                          (fn_u16)sizeof(payload),
                                          scripted, (fn_u16)sizeof(scripted));

        mock_reset(&m, scripted, scripted_len, 1 /* one byte per recv() */);
        fn_init(&ctx, &t);
        err = fn_bus_call(&ctx, 0x70, 0x00, NULL, NULL, 0,
                           replybuf, (fn_u16)sizeof(replybuf), &replylen);

        CHECK(err == FN_OK, "byte-at-a-time: fn_bus_call returns FN_OK");
        CHECK(replylen == (fn_u16)sizeof(payload), "byte-at-a-time: reply_len matches payload size");
        CHECK(replylen == (fn_u16)sizeof(payload) &&
              memcmp(replybuf, payload, sizeof(payload)) == 0,
              "byte-at-a-time: reply bytes match scripted payload");
    }

    /* Overflow: mocked ACK reply carries 20 bytes, but reply_max is 10. */
    {
        fn_u8 payload[20];
        FnParams p;
        fn_u8 scripted[64];
        fn_u16 scripted_len;
        fn_u8 replybuf[10];
        fn_u16 replylen = 0;
        FnErr err;
        fn_u16 i;

        for (i = 0; i < (fn_u16)sizeof(payload); i++) payload[i] = (fn_u8)(i + 1);

        fn_params_none(&p);
        scripted_len = fn_encode_request(0x70, FN_CMD_ACK, &p, payload,
                                          (fn_u16)sizeof(payload),
                                          scripted, (fn_u16)sizeof(scripted));

        mock_reset(&m, scripted, scripted_len, 0);
        fn_init(&ctx, &t);
        err = fn_bus_call(&ctx, 0x70, 0x00, NULL, NULL, 0,
                           replybuf, (fn_u16)sizeof(replybuf), &replylen);

        /* Per the fn_bus_call contract, reply_len reports the *actual*
         * (untruncated) payload length even when the copy into `reply` was
         * truncated -- this lets the caller detect how much data it missed,
         * matching common truncating-copy APIs (e.g. snprintf's return
         * value). The copied *bytes* in replybuf are still clamped to
         * reply_max (10). */
        CHECK(err == FN_ERR_OVERFLOW, "overflow: fn_bus_call returns FN_ERR_OVERFLOW");
        CHECK(replylen == (fn_u16)sizeof(payload),
              "overflow: reply_len reports the actual untruncated payload length (20)");
        CHECK(memcmp(replybuf, payload, sizeof(replybuf)) == 0,
              "overflow: first reply_max (10) reply bytes match mocked payload");
    }

    /* Wrong device: mocked ACK reply's device byte differs from the
     * device fn_bus_call was called with. */
    {
        FnParams p;
        fn_u8 scripted[32];
        fn_u16 scripted_len;
        FnErr err;

        fn_params_none(&p);
        scripted_len = fn_encode_request(0x71 /* reply device */, FN_CMD_ACK, &p,
                                          NULL, 0, scripted, (fn_u16)sizeof(scripted));

        mock_reset(&m, scripted, scripted_len, 0);
        fn_init(&ctx, &t);
        err = fn_bus_call(&ctx, 0x70 /* request device */, 0x00, NULL, NULL, 0, NULL, 0, NULL);
        CHECK(err == FN_ERR_DEVICE, "wrong device: fn_bus_call returns FN_ERR_DEVICE");
    }

    /* Send path agrees byte-for-byte with fn_encode_request: script
     * vector 5's bare-ACK reply, call fn_bus_call for vector 1's
     * request (DEVICE_READY, no params), and check what was actually
     * transmitted. */
    {
        FnErr err;

        mock_reset(&m, VEC5, (fn_u16)sizeof(VEC5), 0);
        fn_init(&ctx, &t);
        err = fn_bus_call(&ctx, 0x70, 0x00, NULL, NULL, 0, NULL, 0, NULL);

        CHECK(err == FN_OK, "send-path check: fn_bus_call returns FN_OK");
        CHECK(m.sent_len == (fn_u16)sizeof(VEC1), "send-path check: sent byte count matches vector 1");
        CHECK(m.sent_len == (fn_u16)sizeof(VEC1) && memcmp(m.sent, VEC1, sizeof(VEC1)) == 0,
              "send-path check: sent bytes match fn_encode_request's vector 1 output exactly");
    }
}

/* -------------------------------------------------------------------- */

int main(void)
{
    test_checksum();
    test_slip_roundtrip();
    test_wire_vectors();
    test_mixed_width();
    test_reply_parsing();
    test_length_endianness();
    test_mock_transport();

    printf("%d/%d tests passed\n", g_test_count - g_fail_count, g_test_count);
    return g_fail_count ? 1 : 0;
}
