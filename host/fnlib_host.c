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
#include <sys/stat.h>

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

/* ------------------------------------------------------------------ */
/* FujiConfig call-sequence exercises: every fujinet-lib call FujiConfig
 * (palm/apps/fujiconfig) makes, run against the real library code the app
 * links (common/ sources + bus/palmos/fujinet-bus-palmos.c) and a live
 * fujinet-pc, before trusting any of it on the Palm OS side. Uses host
 * slot 0 ("SD", see run/fnconfig.ini) and the test file this repo's
 * docs/protocol.md-adjacent setup places at run/SD/palm/hello.prc (copied
 * from palm/apps/hello/hello.prc -- see host/Makefile's fnlib-test-run or
 * the FujiConfig build notes for how it gets there; if absent,
 * test_install_read() reports it and fails rather than hanging).
 * ------------------------------------------------------------------ */

/* run/SD/palm/hello.prc's path relative to this binary's cwd (host/, per
 * host/Makefile's fnlib-test-run target: `./$(FNLIB_BUILD)/fnlib-test`). */
#define TEST_PRC_DISK_PATH "../run/SD/palm/hello.prc"
#define TEST_PRC_DEVICESPEC "N:SD:/palm/hello.prc"

static int test_host_slots(void)
{
  HostSlot slots[8];
  int i;

  memset(slots, 0, sizeof(slots));
  if (!fuji_get_host_slots(slots, 8)) {
    fprintf(stderr, "fuji_get_host_slots failed (last_error=%u)\n",
            (unsigned) fuji_palmos_last_error());
    return 1;
  }

  printf("host slots:\n");
  for (i = 0; i < 8; i++) {
    if (slots[i][0] == 0) {
      printf("  %d: (empty)\n", i);
    } else {
      char buf[33];
      memcpy(buf, slots[i], 32);
      buf[32] = '\0';
      printf("  %d: %s\n", i, buf);
    }
  }

  /* Slot 0 must be "SD" for test_directory_listing()/test_install_read()
   * below to mean anything -- confirmed against run/fnconfig.ini's
   * [Host1] type=SD name=SD. */
  if (slots[0][0] == 0 || strncmp((char *) slots[0], "SD", 2) != 0) {
    fprintf(stderr, "expected host slot 0 == \"SD\", got \"%.32s\"\n", (char *) slots[0]);
    return 1;
  }

  /* Idempotent round-trip of fuji_put_host_slots(): FujiConfig's Hosts form
   * writes all 8 slots back on every edit (per the firmware "send fixed
   * structs at full size" hazard), so this must work and must not disturb
   * slots the user didn't touch. Write the exact bytes just read back, then
   * re-read and compare -- never write anything actually different here. */
  if (!fuji_put_host_slots(slots, 8)) {
    fprintf(stderr, "fuji_put_host_slots (round-trip, unchanged) failed (last_error=%u)\n",
            (unsigned) fuji_palmos_last_error());
    return 1;
  }
  {
    HostSlot verify[8];
    memset(verify, 0, sizeof(verify));
    if (!fuji_get_host_slots(verify, 8)) {
      fprintf(stderr, "fuji_get_host_slots (post-write verify) failed (last_error=%u)\n",
              (unsigned) fuji_palmos_last_error());
      return 1;
    }
    if (memcmp(slots, verify, sizeof(slots)) != 0) {
      fprintf(stderr, "fuji_put_host_slots round-trip mismatch: slots changed\n");
      return 1;
    }
  }
  printf("fuji_put_host_slots round-trip: OK (8 slots written and re-verified unchanged)\n");
  return 0;
}

/* Lists one directory to completion via fuji_open_directory_filter() /
 * fuji_read_directory() / fuji_close_directory(), the exact sequence
 * FujiConfig's Browse form uses. Detects end-of-listing the same way
 * core/fn_fuji.c's fn_fuji_read_directory() does -- fujinet-lib's own
 * fuji_read_directory() wrapper does NOT decode this, it is a firmware
 * convention the caller must check itself (fujiDevice.cpp:990-991,
 * confirmed live below): the reply's first two bytes are both 0x7F.
 * Prints each entry (trailing '/' on directories, confirmed live) and
 * returns the number of entries listed, or -1 on error. */
static int list_directory(uint8_t hostSlot, const char *path)
{
  int count = 0;

  /* NOT fuji_open_directory_filter(hostSlot, path, "") -- confirmed live
   * (see FujiConfig build notes) that it reads MEMORY GARBAGE and sends it
   * to the firmware as a bogus filter pattern whenever `filter` is empty.
   * Root cause: fuji_open_directory_filter() (common/
   * fuji_open_directory_filter.c) only builds its padded, correctly-sized
   * static buffer[256] when filter[0] is true; with an empty filter it
   * instead forwards `path` (here, a short string literal) STRAIGHT to
   * fuji_open_directory(), whose FUJICALL_A1_D(..., MAX_FILENAME_LEN)
   * unconditionally reads a FIXED 256 bytes starting at that pointer
   * (include/fujinet-fuji.h) -- an out-of-bounds read on any path shorter
   * than 256 bytes, which is every real path. The firmware then reads that
   * garbage tail as an fnmatch-style filter pattern (fujiDevice.cpp
   * fujicore_open_directory_success), which can silently drop real
   * entries. Confirmed live: listing "/palm" this way returned zero
   * entries even though hello.prc is present, because whatever heap/rodata
   * bytes trailed the "/palm" literal didn't match it.
   *
   * Workaround (no fujinet-lib-palmos source changes): build our own
   * zero-padded MAX_FILENAME_LEN buffer and call the public
   * fuji_open_directory() macro directly. This is the exact byte layout
   * fuji_open_directory_filter()'s own non-empty-filter branch produces,
   * just done for the empty-filter case too. */
  {
    char pathbuf[MAX_FILENAME_LEN];
    size_t plen = strlen(path);

    if (plen >= sizeof(pathbuf))
      plen = sizeof(pathbuf) - 1;
    memset(pathbuf, 0, sizeof(pathbuf));
    memcpy(pathbuf, path, plen);

    if (!fuji_mount_host_slot(hostSlot)) {
      fprintf(stderr, "fuji_mount_host_slot(%u) failed (last_error=%u)\n",
              (unsigned) hostSlot, (unsigned) fuji_palmos_last_error());
      return -1;
    }

    if (!fuji_open_directory(hostSlot, pathbuf)) {
      fprintf(stderr, "fuji_open_directory(%u, \"%s\") failed (last_error=%u)\n",
              (unsigned) hostSlot, path, (unsigned) fuji_palmos_last_error());
      return -1;
    }
  }

  printf("directory listing of \"%s\" on host slot %u:\n", path, (unsigned) hostSlot);
  for (;;) {
    unsigned char buf[256];
    size_t namelen;

    memset(buf, 0xAA, sizeof(buf));
    if (!fuji_read_directory(255, 0, buf)) {
      fprintf(stderr, "fuji_read_directory failed (last_error=%u)\n",
              (unsigned) fuji_palmos_last_error());
      fuji_close_directory();
      return -1;
    }

    if (buf[0] == 0x7F && buf[1] == 0x7F) {
      break; /* end-of-directory marker */
    }

    /* Reply is exactly 255 bytes, NUL-padded after the entry name. */
    namelen = strnlen((char *) buf, 255);
    printf("  [%3d] %.*s\n", count, (int) namelen, (char *) buf);
    count++;

    if (count > 500) {
      fprintf(stderr, "list_directory: giving up after 500 entries (no end marker seen)\n");
      fuji_close_directory();
      return -1;
    }
  }

  fuji_close_directory();
  return count;
}

static int test_directory_listing(void)
{
  int rootCount = list_directory(0, "/");
  int subCount;

  if (rootCount < 0)
    return 1;

  subCount = list_directory(0, "/palm");
  if (subCount < 0)
    return 1;

  if (subCount < 1) {
    fprintf(stderr, "expected at least one entry under /palm (hello.prc) -- "
                     "is " TEST_PRC_DISK_PATH " present?\n");
    return 1;
  }

  return 0;
}

/* N: open + full read of the test .prc, the exact sequence FujiConfig's
 * Install form uses for its ExgDBRead readProc. network_read() (common/
 * network_read.c, via network_read_nb.c) already loops internally until it
 * has filled the requested length or hit real EOF/error -- confirmed by
 * reading that source -- so a single bounded-size call per chunk is enough;
 * no manual network_status() polling is required in the app. */
static int test_install_read(void)
{
  char buf[512];
  size_t total = 0;
  int16_t n;
  struct stat st;

  if (stat(TEST_PRC_DISK_PATH, &st) != 0) {
    fprintf(stderr, "stat(%s) failed -- copy palm/apps/hello/hello.prc there first "
                     "(see palm/apps/fujiconfig build notes)\n", TEST_PRC_DISK_PATH);
    return 1;
  }

  if (network_open(TEST_PRC_DEVICESPEC, OPEN_MODE_READ, OPEN_TRANS_NONE) != FN_ERR_OK) {
    fprintf(stderr, "network_open(%s) failed\n", TEST_PRC_DEVICESPEC);
    return 1;
  }

  for (;;) {
    n = network_read(TEST_PRC_DEVICESPEC, buf, sizeof(buf));
    if (n < 0) {
      fprintf(stderr, "network_read failed (n=%d)\n", (int) n);
      network_close(TEST_PRC_DEVICESPEC);
      return 1;
    }
    if (n == 0)
      break; /* EOF */
    total += (size_t) n;
    if (total > 10u * 1024u * 1024u) {
      fprintf(stderr, "test_install_read: giving up after 10MB (no EOF seen)\n");
      network_close(TEST_PRC_DEVICESPEC);
      return 1;
    }
  }

  network_close(TEST_PRC_DEVICESPEC);

  printf("network_read %s: %lu bytes (disk file is %lld bytes)\n",
         TEST_PRC_DEVICESPEC, (unsigned long) total, (long long) st.st_size);

  if (total != (size_t) st.st_size) {
    fprintf(stderr, "byte count mismatch: read %lu, disk file is %lld\n",
            (unsigned long) total, (long long) st.st_size);
    return 1;
  }
  return 0;
}

static int test_scan(void)
{
  uint8_t count = 0;

  if (!fuji_scan_for_networks(&count)) {
    fprintf(stderr, "fuji_scan_for_networks failed (last_error=%u)\n",
            (unsigned) fuji_palmos_last_error());
    return 1;
  }

  printf("scan_for_networks: %u result(s)\n", (unsigned) count);

  if (count > 0) {
    SSIDInfo info;

    memset(&info, 0, sizeof(info));
    if (!fuji_get_scan_result(0, &info)) {
      fprintf(stderr, "fuji_get_scan_result(0) failed (last_error=%u)\n",
              (unsigned) fuji_palmos_last_error());
      return 1;
    }
    printf("  [0] ssid=\"%.*s\" rssi=%d\n",
           (int) sizeof(info.ssid), info.ssid, (int) info.rssi);
  }

  /* fujinet-pc fakes WiFi (see FujiConfig build notes) -- a 0-result scan
   * is not a failure here, only a malformed call would be. */
  return 0;
}

static int run_fujiconfig_tests(void)
{
  int rc = 0;

  if (test_host_slots() != 0)
    rc = 1;
  if (test_directory_listing() != 0)
    rc = 1;
  if (test_install_read() != 0)
    rc = 1;
  if (test_scan() != 0)
    rc = 1;

  return rc;
}

/* ------------------------------------------------------------------ */
/* ISS Tracker call-sequence exercises: the exact fujinet-lib calls
 * palm/apps/isstracker/isstracker.c makes (via ../../palm/apps/common/
 * fnapp.c's fnapp_json_open/fnapp_json_query/fnapp_json_close, which are
 * thin wrappers over network_open/network_json_parse/network_json_query/
 * network_close -- exercised directly here since fnapp.c itself is Palm-
 * app code, not part of fujinet-lib proper).
 * ------------------------------------------------------------------ */

static int test_iss(void)
{
  const char *spec = "N:http://api.open-notify.org/iss-now.json";
  const char *astros = "N:http://api.open-notify.org/astros.json";
  char buf[64];
  int16_t n;
  int rc = 0;

  if (network_open(spec, OPEN_MODE_HTTP_GET, OPEN_TRANS_NONE) != FN_ERR_OK) {
    fprintf(stderr, "test_iss: network_open(%s) failed\n", spec);
    return 1;
  }
  if (network_json_parse(spec) != FN_ERR_OK) {
    fprintf(stderr, "test_iss: network_json_parse failed\n");
    network_close(spec);
    return 1;
  }

  n = network_json_query(spec, "/timestamp", buf);
  if (n < 0) { fprintf(stderr, "test_iss: /timestamp failed (n=%d)\n", (int) n); rc = 1; }
  else printf("iss timestamp: %.*s\n", (int) n, buf);

  n = network_json_query(spec, "/iss_position/latitude", buf);
  if (n < 0) { fprintf(stderr, "test_iss: /iss_position/latitude failed (n=%d)\n", (int) n); rc = 1; }
  else printf("iss latitude: %.*s\n", (int) n, buf);

  n = network_json_query(spec, "/iss_position/longitude", buf);
  if (n < 0) { fprintf(stderr, "test_iss: /iss_position/longitude failed (n=%d)\n", (int) n); rc = 1; }
  else printf("iss longitude: %.*s\n", (int) n, buf);

  network_close(spec);

  if (network_open(astros, OPEN_MODE_HTTP_GET, OPEN_TRANS_NONE) != FN_ERR_OK) {
    fprintf(stderr, "test_iss: network_open(%s) failed\n", astros);
    return 1;
  }
  if (network_json_parse(astros) != FN_ERR_OK) {
    fprintf(stderr, "test_iss: astros network_json_parse failed\n");
    network_close(astros);
    return 1;
  }

  n = network_json_query(astros, "/number", buf);
  if (n < 0) {
    fprintf(stderr, "test_iss: /number failed (n=%d)\n", (int) n);
    rc = 1;
  } else {
    int count = atoi(buf);
    char path[32];

    printf("astros /number: %.*s\n", (int) n, buf);
    if (count > 0) {
      n = network_json_query(astros, "/people/0/name", buf);
      if (n < 0) { fprintf(stderr, "test_iss: /people/0/name failed\n"); rc = 1; }
      else printf("astros /people/0/name: %.*s\n", (int) n, buf);

      snprintf(path, sizeof(path), "/people/%d/craft", 0);
      n = network_json_query(astros, path, buf);
      if (n < 0) { fprintf(stderr, "test_iss: /people/0/craft failed\n"); rc = 1; }
      else printf("astros /people/0/craft: %.*s\n", (int) n, buf);
    }
  }

  network_close(astros);
  return rc;
}

/* ------------------------------------------------------------------ */
/* Weather call-sequence exercises: the exact fujinet-lib calls
 * palm/apps/weather/weather.c makes (ip-api geolocation, Open-Meteo
 * geocoding, and the combined current+daily forecast request).
 * ------------------------------------------------------------------ */

static int test_weather(void)
{
  const char *ip_url = "N:http://ip-api.com/json/?fields=status,city,regionName,countryCode,lon,lat";
  char lat[20], lon[20], city[32];
  char buf[64];
  char url[256];
  int16_t n;
  int rc = 0;

  if (network_open(ip_url, OPEN_MODE_HTTP_GET, OPEN_TRANS_NONE) != FN_ERR_OK) {
    fprintf(stderr, "test_weather: network_open(ip-api) failed\n");
    return 1;
  }
  if (network_json_parse(ip_url) != FN_ERR_OK) {
    fprintf(stderr, "test_weather: ip-api network_json_parse failed\n");
    network_close(ip_url);
    return 1;
  }

  n = network_json_query(ip_url, "/status", buf);
  printf("ip-api /status: %.*s\n", (int) (n < 0 ? 0 : n), buf);
  n = network_json_query(ip_url, "/city", city);
  printf("ip-api /city: %.*s\n", (int) (n < 0 ? 0 : n), city);
  n = network_json_query(ip_url, "/lat", lat);
  printf("ip-api /lat: %.*s\n", (int) (n < 0 ? 0 : n), lat);
  n = network_json_query(ip_url, "/lon", lon);
  printf("ip-api /lon: %.*s\n", (int) (n < 0 ? 0 : n), lon);
  network_close(ip_url);

  if (lat[0] == '\0' || lon[0] == '\0') {
    fprintf(stderr, "test_weather: no lat/lon from ip-api, skipping forecast query\n");
    return 1;
  }

  /* CRITICAL FINDING (confirmed live 2026-09-28): fujinet-pc/the firmware
   * silently truncates a devicespec (the whole "N:..." string, INCLUDING
   * the "N:" prefix) at exactly 256 bytes, regardless of this platform's
   * FUJI_VARIABLE_LEN_PACKETS=1 (which only affects what the CLIENT
   * calculates as NETWORK_OPEN_LEN, not what the server/firmware actually
   * honors). A single combined Open-Meteo request listing every field
   * (357 bytes) got cut mid-parameter -- "weather_code" truncated to
   * "weather" -- producing a small JSON *error* body ({"error":true,...})
   * with no open/parse-level failure at all: network_open and
   * network_json_parse both report FN_ERR_OK, so this is invisible unless
   * you inspect the actual query results (which all come back empty).
   * Binary-searched the exact cutoff: bodies came back correct through a
   * 204-byte devicespec, and the truncation-induced error appeared at
   * 261 bytes (cut at byte 256 precisely). Also, https:// specifically to
   * api.open-meteo.com/v1/forecast was separately confirmed to return a
   * 0-byte body every time (open/parse both report FN_ERR_OK, but no
   * data ever arrives) while http:// against the identical host+path
   * always worked -- both findings are baked into
   * palm/apps/weather/weather.c's GetWeather(), which issues THREE short
   * http:// requests instead of one long one, each safely under ~230
   * bytes even with 9-byte lat/lon strings. Exercise that exact 3-request
   * sequence here. */
  {
    /* Request 1: current temp/feels-like/wind (unit-dependent). */
    snprintf(url, sizeof(url),
             "N:http://api.open-meteo.com/v1/forecast?latitude=%s&longitude=%s"
             "&current=temperature_2m,apparent_temperature,wind_speed_10m,wind_direction_10m"
             "&temperature_unit=fahrenheit&wind_speed_unit=mph&timeformat=unixtime",
             lat, lon);
    if (network_open(url, OPEN_MODE_HTTP_GET, OPEN_TRANS_NONE) != FN_ERR_OK) {
      fprintf(stderr, "test_weather: network_open(forecast req1) failed\n");
      return 1;
    }
    if (network_json_parse(url) != FN_ERR_OK) {
      fprintf(stderr, "test_weather: forecast req1 network_json_parse failed\n");
      network_close(url);
      return 1;
    }
    n = network_json_query(url, "/current/temperature_2m", buf);
    if (n <= 0) { fprintf(stderr, "test_weather: /current/temperature_2m failed/empty (n=%d)\n", (int) n); rc = 1; }
    else printf("weather /current/temperature_2m: %.*s\n", (int) n, buf);
    n = network_json_query(url, "/current/wind_direction_10m", buf);
    if (n <= 0) { fprintf(stderr, "test_weather: /current/wind_direction_10m failed/empty\n"); rc = 1; }
    else printf("weather /current/wind_direction_10m: %.*s\n", (int) n, buf);
    network_close(url);

    /* Request 2: current humidity/weather code/pressure (unit-independent). */
    snprintf(url, sizeof(url),
             "N:http://api.open-meteo.com/v1/forecast?latitude=%s&longitude=%s"
             "&current=relative_humidity_2m,weather_code,surface_pressure"
             "&daily=sunrise,sunset&forecast_days=1&timezone=auto&timeformat=unixtime",
             lat, lon);
    if (network_open(url, OPEN_MODE_HTTP_GET, OPEN_TRANS_NONE) != FN_ERR_OK) {
      fprintf(stderr, "test_weather: network_open(forecast req2) failed\n");
      return 1;
    }
    if (network_json_parse(url) != FN_ERR_OK) {
      fprintf(stderr, "test_weather: forecast req2 network_json_parse failed\n");
      network_close(url);
      return 1;
    }
    n = network_json_query(url, "/current/weather_code", buf);
    if (n <= 0) { fprintf(stderr, "test_weather: /current/weather_code failed/empty\n"); rc = 1; }
    else printf("weather /current/weather_code: %.*s\n", (int) n, buf);
    n = network_json_query(url, "/utc_offset_seconds", buf);
    if (n <= 0) { fprintf(stderr, "test_weather: /utc_offset_seconds failed/empty\n"); rc = 1; }
    else printf("weather /utc_offset_seconds: %.*s\n", (int) n, buf);
    n = network_json_query(url, "/daily/sunrise/0", buf);
    if (n <= 0) { fprintf(stderr, "test_weather: /daily/sunrise/0 failed/empty\n"); rc = 1; }
    else printf("weather /daily/sunrise/0: %.*s\n", (int) n, buf);
    n = network_json_query(url, "/daily/sunset/0", buf);
    if (n <= 0) { fprintf(stderr, "test_weather: /daily/sunset/0 failed/empty\n"); rc = 1; }
    else printf("weather /daily/sunset/0: %.*s\n", (int) n, buf);
    network_close(url);

    /* Request 3: 5-day forecast. */
    snprintf(url, sizeof(url),
             "N:http://api.open-meteo.com/v1/forecast?latitude=%s&longitude=%s"
             "&forecast_days=6&daily=temperature_2m_max,temperature_2m_min,weather_code"
             "&temperature_unit=fahrenheit&timezone=auto&timeformat=unixtime",
             lat, lon);
    if (network_open(url, OPEN_MODE_HTTP_GET, OPEN_TRANS_NONE) != FN_ERR_OK) {
      fprintf(stderr, "test_weather: network_open(forecast req3) failed\n");
      return 1;
    }
    if (network_json_parse(url) != FN_ERR_OK) {
      fprintf(stderr, "test_weather: forecast req3 network_json_parse failed\n");
      network_close(url);
      return 1;
    }
    n = network_json_query(url, "/daily/temperature_2m_max/1", buf);
    if (n <= 0) { fprintf(stderr, "test_weather: /daily/temperature_2m_max/1 failed/empty\n"); rc = 1; }
    else printf("weather /daily/temperature_2m_max/1: %.*s\n", (int) n, buf);
    network_close(url);
  }

  /* Geocoding endpoint, independent of the location above. http, NOT
   * https, for the same reason as the forecast URL above -- confirmed
   * live: https://geocoding-api.open-meteo.com was flaky (roughly 2/3
   * runs) while http:// was 100% reliable across repeated runs. */
  {
    const char *geoUrl = "N:http://geocoding-api.open-meteo.com/v1/search?name=Chicago&count=1&language=en&format=json";

    if (network_open(geoUrl, OPEN_MODE_HTTP_GET, OPEN_TRANS_NONE) != FN_ERR_OK) {
      fprintf(stderr, "test_weather: network_open(geocoding) failed\n");
      return 1;
    }
    if (network_json_parse(geoUrl) != FN_ERR_OK) {
      fprintf(stderr, "test_weather: geocoding network_json_parse failed\n");
      network_close(geoUrl);
      return 1;
    }
    n = network_json_query(geoUrl, "/results/0/name", buf);
    if (n < 0) { fprintf(stderr, "test_weather: /results/0/name failed\n"); rc = 1; }
    else printf("geocoding /results/0/name: %.*s\n", (int) n, buf);
    network_close(geoUrl);
  }

  return rc;
}

/* ------------------------------------------------------------------ */
/* News call-sequence exercises: the exact fujinet-lib calls
 * palm/apps/news/news.c makes -- plain network_read of the category
 * listing and article endpoints (no JSON parser; the server returns
 * pipe/newline-delimited text, see palm/apps/news/news.c's NextField).
 * ------------------------------------------------------------------ */

static int test_news(void)
{
  const char *listUrl = "N:https://fujinet.online/8bitnews/news.php?t=lf&ps=255x24&l=5&p=1&c=top";
  char buf[4096];
  size_t total = 0;
  int16_t n;
  unsigned long articleId = 0;
  char *firstLine;
  char *idStart;

  if (network_open(listUrl, OPEN_MODE_HTTP_GET, OPEN_TRANS_NONE) != FN_ERR_OK) {
    fprintf(stderr, "test_news: network_open(listing) failed\n");
    return 1;
  }
  for (;;) {
    n = network_read(listUrl, buf + total, (uint16_t) (sizeof(buf) - 1 - total));
    if (n <= 0) break;
    total += (size_t) n;
    if (total >= sizeof(buf) - 1) break;
  }
  buf[total] = '\0';
  network_close(listUrl);

  printf("news listing (%lu bytes):\n%.*s\n", (unsigned long) total, (int) (total > 400 ? 400 : total), buf);

  if (total == 0) {
    fprintf(stderr, "test_news: empty listing response\n");
    return 1;
  }

  /* Parse the first "id|datetime|title" line (after the "page/pages"
   * first line) well enough to get an article id to test the article
   * endpoint with. */
  firstLine = strchr(buf, '\n');
  idStart = firstLine ? firstLine + 1 : buf;
  articleId = strtoul(idStart, NULL, 10);
  if (articleId == 0) {
    fprintf(stderr, "test_news: could not parse an article id from listing\n");
    return 1;
  }
  printf("news: testing article id %lu\n", articleId);

  {
    char artUrl[160];
    char artBuf[8192];
    size_t artTotal = 0;

    snprintf(artUrl, sizeof(artUrl),
             "N:https://fujinet.online/8bitnews/news.php?t=lf&ps=255x60&l=5&p=1&a=%lu", articleId);

    if (network_open(artUrl, OPEN_MODE_HTTP_GET, OPEN_TRANS_NONE) != FN_ERR_OK) {
      fprintf(stderr, "test_news: network_open(article) failed\n");
      return 1;
    }
    for (;;) {
      n = network_read(artUrl, artBuf + artTotal, (uint16_t) (sizeof(artBuf) - 1 - artTotal));
      if (n <= 0) break;
      artTotal += (size_t) n;
      if (artTotal >= sizeof(artBuf) - 1) break;
    }
    artBuf[artTotal] = '\0';
    network_close(artUrl);

    printf("news article (%lu bytes), first 200 chars:\n%.*s\n",
           (unsigned long) artTotal, (int) (artTotal > 200 ? 200 : artTotal), artBuf);

    if (artTotal == 0) {
      fprintf(stderr, "test_news: empty article response\n");
      return 1;
    }
  }

  return 0;
}

int main(int argc, char **argv)
{
  const char *host = "127.0.0.1";
  int port = 1985;
  int verbose = 0;
  /* New 4th arg (mode): "all" (default) runs the original endian/adapter/
   * network/json exercises plus the FujiConfig call sequences below;
   * "fujiconfig" runs only the new ones, for fast iteration; "iss",
   * "weather", "news" each run only that app's call-sequence exercises
   * (test_iss/test_weather/test_news below), for fast iteration on a
   * single app. The existing 3-arg invocation (host port verbose) is
   * unchanged and still runs everything "all" always did. */
  const char *mode = "all";
  int rc = 0;

  if (argc > 1)
    host = argv[1];
  if (argc > 2)
    port = atoi(argv[2]);
  if (argc > 3)
    verbose = atoi(argv[3]);
  if (argc > 4)
    mode = argv[4];

  if (strcmp(mode, "fujiconfig") != 0 && strcmp(mode, "iss") != 0 &&
      strcmp(mode, "weather") != 0 && strcmp(mode, "news") != 0) {
    test_endian_fixup();
    if (g_test_failures > 0)
      rc = 1;
  }

  g_transport = fn_transport_tcp_open(host, port, verbose);
  if (g_transport == NULL) {
    fprintf(stderr, "could not connect to fujinet-pc at %s:%d\n", host, port);
    return 1;
  }
  fn_init(&g_ctx, g_transport);

  if (strcmp(mode, "iss") == 0) {
    rc = test_iss();
  } else if (strcmp(mode, "weather") == 0) {
    rc = test_weather();
  } else if (strcmp(mode, "news") == 0) {
    rc = test_news();
  } else {
    if (strcmp(mode, "fujiconfig") != 0) {
      if (test_adapter_config() != 0)
        rc = 1;
      if (test_network_read() != 0)
        rc = 1;
      if (test_json() != 0)
        rc = 1;
    }

    if (run_fujiconfig_tests() != 0)
      rc = 1;
  }

  fn_transport_tcp_close(g_transport);

  printf(rc == 0 ? "ALL TESTS PASSED\n" : "SOME TESTS FAILED\n");
  return rc;
}
