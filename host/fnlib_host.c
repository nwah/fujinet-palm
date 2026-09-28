/* fnlib_host.c
 *
 * Host integration test for the fujinet-lib "palmos" platform port
 * (../../fujinet-lib-palmos, FNLIB below). Builds the ACTUAL public
 * fujinet-lib API -- common/ sources plus bus/palmos/fujinet-bus-palmos.c,
 * unmodified from the worktree -- with this file's own fuji_palmos_ctx()
 * wired to the TCP transport (transport_tcp.c) instead of link_palmos.c's
 * Palm Serial Manager one, and exercises it against a live fujinet-pc.
 *
 * Host-only code: ordinary C99 + POSIX (via transport_tcp.c). The library
 * code it links against is still compiled as if for palmos (-D__palmos__),
 * see host/Makefile's fnlib-test target.
 */
#define _POSIX_C_SOURCE 200809L
#define _DARWIN_C_SOURCE

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "fujinet-fuji.h"
#include "fujinet-network.h"
#include "fujinet-palmos.h"
#include "fujinet-bus-palmos.h"
#include "transport_tcp.h"

/* ------------------------------------------------------------------ */
/* fuji_palmos_ctx() / fuji_palmos_set_last_error(): the link_palmos.c
 * replacement for this test. Holds one FnCtx bound to a TCP transport
 * instead of a Palm serial port.
 * ------------------------------------------------------------------ */

static FnCtx g_ctx;
static FnTransport *g_transport;
static uint16_t g_last_error;

void fuji_palmos_set_last_error(uint16_t err)
{
  g_last_error = err;
}

uint16_t fuji_palmos_last_error(void)
{
  return g_last_error;
}

FnCtx *fuji_palmos_ctx(void)
{
  return g_transport ? &g_ctx : NULL;
}

/* ------------------------------------------------------------------ */
/* Endian fix-up unit tests.
 *
 * fujinet-bus-palmos.c's palmos_decode_le()/palmos_encode_le() are the
 * primitives its REPLY/DATA fix-up table is built on (see the big comment
 * at the top of that file for the audited call-site table). They are
 * exercised directly here -- rather than round-tripping a whole
 * NetworkStatus reply through fuji_bus_call() on this (little-endian) host,
 * which could not tell "the swap ran and was a no-op on LE" apart from
 * "no swap ran at all" -- by checking VALUE semantics against known wire
 * byte patterns, which is host-endianness independent.
 * ------------------------------------------------------------------ */

extern uint32_t palmos_decode_le(const uint8_t *p, uint8_t width);
extern void palmos_encode_le(uint8_t *p, uint32_t v, uint8_t width);

static int g_test_failures = 0;

#define CHECK(cond, fmt, ...)                                          \
  do {                                                                 \
    if (!(cond)) {                                                     \
      fprintf(stderr, "FAIL %s:%d: " fmt "\n", __FILE__, __LINE__, __VA_ARGS__); \
      g_test_failures++;                                               \
    }                                                                  \
  } while (0)

static void test_endian_fixup(void)
{
  uint8_t wire16[2];
  uint8_t wire32[4];
  uint8_t out[4];
  uint32_t v;

  /* Synthetic little-endian NetworkStatus.avail wire bytes for avail=0x1234
   * (as fujinet-pc would actually send it: LSB first). Confirmed against a
   * live capture in docs/protocol.md: "N1 STATUS ... ACK plus 4 bytes
   * (avail u16 LE, connected, error)". */
  wire16[0] = 0x34;
  wire16[1] = 0x12;
  v = palmos_decode_le(wire16, 2);
  CHECK(v == 0x1234u, "decode_le(u16) = 0x%lx, want 0x1234", (unsigned long) v);

  /* Round trip: encoding the decoded value back must reproduce the same
   * little-endian wire bytes, independent of host endianness -- this is
   * exactly what palmos_fixup_data_field() relies on for outgoing
   * requests (e.g. FNAppKeyID.creator). */
  memset(out, 0xAA, sizeof(out));
  palmos_encode_le(out, v, 2);
  CHECK(out[0] == 0x34 && out[1] == 0x12,
        "encode_le(u16) = %02x %02x, want 34 12", out[0], out[1]);

  /* 32-bit case, e.g. the base64/QRCode "unsigned long" length replies:
   * value 0x89ABCDEF sent LSB-first. */
  wire32[0] = 0xEF;
  wire32[1] = 0xCD;
  wire32[2] = 0xAB;
  wire32[3] = 0x89;
  v = palmos_decode_le(wire32, 4);
  CHECK(v == 0x89ABCDEFu, "decode_le(u32) = 0x%lx, want 0x89abcdef", (unsigned long) v);

  memset(out, 0, sizeof(out));
  palmos_encode_le(out, v, 4);
  CHECK(out[0] == 0xEF && out[1] == 0xCD && out[2] == 0xAB && out[3] == 0x89,
        "encode_le(u32) round trip mismatch: %02x %02x %02x %02x",
        out[0], out[1], out[2], out[3]);

  /* This is the value-semantics claim that matters for m68k: whatever the
   * host's own native byte order is, decode_le() must read wire bytes
   * LSB-first (never native memcpy), so casting wire16 as a big-endian
   * uint16_t (as memcpy-into-a-struct would do on Palm without the
   * fix-up) would give 0x3412, NOT 0x1234. Confirm the two disagree, i.e.
   * this host's test actually distinguishes the fixed from the unfixed
   * behaviour rather than the two coincidentally agreeing. */
  {
    uint16_t naive_be;
    naive_be = (uint16_t) ((wire16[0] << 8) | wire16[1]); /* what a raw
      big-endian memcpy of these wire bytes would read as an int */
    CHECK(naive_be != 0x1234u,
          "test is not discriminating: naive big-endian read (0x%x) "
          "accidentally matches the correct decode", naive_be);
  }

  if (g_test_failures == 0)
    printf("endian fix-up unit tests: PASS\n");
  else
    printf("endian fix-up unit tests: %d FAILURE(S)\n", g_test_failures);
}

/* ------------------------------------------------------------------ */
/* Live fujinet-pc exercises.                                          */
/* ------------------------------------------------------------------ */

static int test_adapter_config(void)
{
  AdapterConfigExtended ac;

  memset(&ac, 0, sizeof(ac));
  if (!fuji_get_adapter_config_extended(&ac)) {
    fprintf(stderr, "fuji_get_adapter_config_extended failed (last_error=%u)\n",
            (unsigned) fuji_palmos_last_error());
    return 1;
  }

  printf("adapter config:\n");
  printf("  ssid:     %.*s\n", (int) sizeof(ac.ssid), ac.ssid);
  printf("  hostname: %.*s\n", (int) sizeof(ac.hostname), ac.hostname);
  printf("  ip:       %s\n", ac.sLocalIP);
  printf("  version:  %.*s\n", (int) sizeof(ac.fn_version), ac.fn_version);
  return 0;
}

static int test_network_read(void)
{
  const char *spec = "N:http://example.com/";
  char buf[8192];
  size_t total = 0;
  uint16_t avail;
  uint8_t status, errcode;
  FN_ERR err;
  int spins = 0;

  if (network_open(spec, OPEN_MODE_HTTP_GET, OPEN_TRANS_NONE) != FN_ERR_OK) {
    fprintf(stderr, "network_open(%s) failed\n", spec);
    return 1;
  }

  for (;;) {
    err = network_status(spec, &avail, &status, &errcode);
    if (err != FN_ERR_OK) {
      fprintf(stderr, "network_status failed mid-read (err=%u)\n", (unsigned) err);
      break;
    }

    if (avail > 0) {
      uint16_t chunk = avail;
      size_t room = sizeof(buf) - 1 - total;

      if (chunk > room)
        chunk = (uint16_t) room;
      if (chunk == 0)
        break; /* buffer full */

      {
        int16_t n = network_read(spec, buf + total, chunk);
        if (n < 0) {
          fprintf(stderr, "network_read failed (n=%d)\n", (int) n);
          break;
        }
        total += (size_t) n;
      }
    } else if (status == 0) {
      break; /* nothing waiting, and the resource read has completed */
    }

    if (++spins > 10000) {
      fprintf(stderr, "network_read: giving up after %d status polls\n", spins);
      break;
    }
  }

  buf[total] = '\0';
  network_close(spec);

  printf("network_read: %lu bytes\n", (unsigned long) total);
  printf("  first 80 chars: %.80s\n", buf);
  return (total > 0) ? 0 : 1;
}

static int test_json(void)
{
  const char *spec = "N:https://oldbytes.space/api/v1/timelines/public?limit=1";
  /* network_json_query() has no caller-supplied length limit (see
   * include/fujinet-network.h) -- it reads exactly as many bytes as
   * network_status() reports available, straight into `buf`. A live toot's
   * "content" field (raw HTML) can run several KB, so this needs real
   * headroom, not just MAX_JSON_QUERY_LEN. */
  char buf[16384];
  int16_t n;
  int rc = 0;

  if (network_open(spec, OPEN_MODE_HTTP_GET, OPEN_TRANS_NONE) != FN_ERR_OK) {
    fprintf(stderr, "network_open(%s) failed\n", spec);
    return 1;
  }

  if (network_json_parse(spec) != FN_ERR_OK) {
    fprintf(stderr, "network_json_parse failed\n");
    network_close(spec);
    return 1;
  }

  n = network_json_query(spec, "/0/account/display_name", buf);
  if (n < 0) {
    fprintf(stderr, "network_json_query(display_name) failed (n=%d)\n", (int) n);
    rc = 1;
  } else {
    printf("json display_name: %.*s\n", (int) n, buf);
  }

  n = network_json_query(spec, "/0/replies_count", buf);
  if (n < 0) {
    fprintf(stderr, "network_json_query(replies_count) failed (n=%d)\n", (int) n);
    rc = 1;
  } else {
    printf("json replies_count: %.*s\n", (int) n, buf);
  }

  n = network_json_query(spec, "/0/content", buf);
  if (n < 0) {
    fprintf(stderr, "network_json_query(content) failed (n=%d)\n", (int) n);
    rc = 1;
  } else {
    printf("json content: %.*s\n", n > 200 ? 200 : (int) n, buf);
  }

  network_close(spec);
  return rc;
}

int main(int argc, char **argv)
{
  const char *host = "127.0.0.1";
  int port = 1985;
  int verbose = 0;
  int rc = 0;

  if (argc > 1)
    host = argv[1];
  if (argc > 2)
    port = atoi(argv[2]);
  if (argc > 3)
    verbose = atoi(argv[3]);

  test_endian_fixup();
  if (g_test_failures > 0)
    rc = 1;

  g_transport = fn_transport_tcp_open(host, port, verbose);
  if (g_transport == NULL) {
    fprintf(stderr, "could not connect to fujinet-pc at %s:%d\n", host, port);
    return 1;
  }
  fn_init(&g_ctx, g_transport);

  if (test_adapter_config() != 0)
    rc = 1;
  if (test_network_read() != 0)
    rc = 1;
  if (test_json() != 0)
    rc = 1;

  fn_transport_tcp_close(g_transport);

  printf(rc == 0 ? "ALL TESTS PASSED\n" : "SOME TESTS FAILED\n");
  return rc;
}
