/* fnhost.c
 *
 * Command-line client for exercising a FujiBus device (real adapter over
 * serial, or a TCP bridge/emulator) from macOS. Thin CLI glue over the
 * portable core library (fujibus.h/fn_fuji.h/fn_net.h) plus the two host
 * FnTransport backends in transport_tcp.c / transport_serial.c.
 *
 * Host-only code: ordinary C99 + POSIX.
 */

#define _POSIX_C_SOURCE 200809L
#define _DARWIN_C_SOURCE

#include "fn_types.h"
#include "fn_transport.h"
#include "fujibus.h"
#include "fn_fuji.h"
#include "fn_net.h"
#include "transport_tcp.h"
#include "transport_serial.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

/* ------------------------------------------------------------------ */
/* small helpers                                                       */
/* ------------------------------------------------------------------ */

static const char *fnerr_name(FnErr e)
{
    switch (e) {
        case FN_OK:            return "FN_OK";
        case FN_ERR_TIMEOUT:   return "FN_ERR_TIMEOUT";
        case FN_ERR_NAK:       return "FN_ERR_NAK";
        case FN_ERR_CHECKSUM:  return "FN_ERR_CHECKSUM";
        case FN_ERR_LENGTH:    return "FN_ERR_LENGTH";
        case FN_ERR_DEVICE:    return "FN_ERR_DEVICE";
        case FN_ERR_IO:        return "FN_ERR_IO";
        case FN_ERR_OVERFLOW:  return "FN_ERR_OVERFLOW";
        case FN_ERR_PARAM:     return "FN_ERR_PARAM";
        default:                return "FN_ERR_UNKNOWN";
    }
}

/* Prints "error: <cmd>: <FnErr name>" to stderr and returns 1, for use as
 * `rc = fail("cmd", err);`. */
static int fail(const char *cmdname, FnErr e)
{
    fprintf(stderr, "error: %s: %s\n", cmdname, fnerr_name(e));
    return 1;
}

/* Builds the devicespec fn_net_open actually needs to send.
 *
 * Verified against the real firmware (lib/utils/utils.cpp:913
 * util_devicespec_fix_for_parsing, called from NDevice.cpp:160
 * fujidev_open): a devicespec with no "N:"/"N<digits>:" unit prefix is
 * rejected outright as "Invalid devicespec" before the URL is even parsed
 * -- confirmed live: `HTTP://example.com/` and `http://example.com/` (no
 * prefix) both NAK with that exact log message, while `N:HTTP://example.com/`
 * (or any `N<digits>:` form) succeeds; the prefix itself is stripped by
 * util_remove_n_prefix() before the URL parser ever sees it, and scheme case
 * does not matter once the prefix is present. So a plain URL typed by a CLI
 * user needs an "N:" prepended here for the CLI to "just work"; this is a
 * host/fnhost.c convenience, not a core-library concern -- fn_net_open
 * itself remains a faithful thin wrapper that sends exactly the devicespec
 * it's given. Returns a pointer into a static buffer, valid until the next
 * call (fine for this single-threaded, one-shot-per-command CLI). */
static const char *fn_devicespec(const char *url)
{
    static char buf[600];

    if ((url[0] == 'N' || url[0] == 'n') && strchr(url, ':') != NULL) {
        /* Already has some "N..." prefix ending in ':' -- assume the
         * caller knows what they're doing and pass it through as-is. */
        return url;
    }
    snprintf(buf, sizeof(buf), "N:%s", url);
    return buf;
}

static void usage(const char *prog)
{
    fprintf(stderr,
        "usage: %s [-t host:port | -s /dev/cu.X[@baud]] [-v] <command> [args...]\n"
        "\n"
        "transport (default: -t localhost:1985):\n"
        "  -t host:port           connect over TCP\n"
        "  -s device[@baud]       connect over a serial device (default 115200 baud)\n"
        "  -v                     verbose hex-dump tracing of the wire protocol\n"
        "\n"
        "commands:\n"
        "  ready\n"
        "  config\n"
        "  configx\n"
        "  wifi\n"
        "  scan\n"
        "  hosts\n"
        "  devices\n"
        "  mount-host N\n"
        "  ls SLOT [PATH]\n"
        "  get URL\n"
        "  json URL QUERY\n"
        "  raw DEV CMD [hexpayload]\n"
        "  tcp HOST:PORT [TEXT]\n",
        prog);
}

static void hexdump_stdout(const unsigned char *buf, fn_u16 len)
{
    fn_u16 i, j;

    for (i = 0; i < len; i += 16) {
        fn_u16 linelen = (fn_u16)((len - i < 16) ? (len - i) : 16);
        printf("%04x: ", (unsigned)i);
        for (j = 0; j < linelen; j++) {
            printf("%02x ", buf[i + j]);
        }
        printf("\n");
    }
}

static const char *wifi_status_desc(fn_u8 status, char *buf, size_t buflen)
{
    if (status == 3) {
        return "connected";
    }
    if (status == 6) {
        return "disconnected";
    }
    snprintf(buf, buflen, "unknown(%u)", (unsigned)status);
    return buf;
}

static void print_adapter_config(const FnAdapterConfig *cfg)
{
    printf("ssid: %s\n", cfg->ssid);
    printf("hostname: %s\n", cfg->hostname);
    printf("ip: %u.%u.%u.%u\n", cfg->ip[0], cfg->ip[1], cfg->ip[2], cfg->ip[3]);
    printf("gateway: %u.%u.%u.%u\n", cfg->gateway[0], cfg->gateway[1], cfg->gateway[2], cfg->gateway[3]);
    printf("netmask: %u.%u.%u.%u\n", cfg->netmask[0], cfg->netmask[1], cfg->netmask[2], cfg->netmask[3]);
    printf("dns: %u.%u.%u.%u\n", cfg->dns[0], cfg->dns[1], cfg->dns[2], cfg->dns[3]);
    printf("mac: %02x:%02x:%02x:%02x:%02x:%02x\n",
           cfg->mac[0], cfg->mac[1], cfg->mac[2], cfg->mac[3], cfg->mac[4], cfg->mac[5]);
    printf("bssid: %02x:%02x:%02x:%02x:%02x:%02x\n",
           cfg->bssid[0], cfg->bssid[1], cfg->bssid[2], cfg->bssid[3], cfg->bssid[4], cfg->bssid[5]);
    printf("fn_version: %s\n", cfg->fn_version);
}

/* Parses "N" as an unsigned/hex/octal-prefixed integer (via strtoul's base
 * 0), returning it as an int. No range validation beyond what strtoul does;
 * callers pass these straight to fn_u8/fn_u16 parameters. */
static long parse_int(const char *s)
{
    return strtoul(s, NULL, 0);
}

/* ------------------------------------------------------------------ */
/* commands                                                             */
/* ------------------------------------------------------------------ */

static int cmd_ready(FnCtx *ctx)
{
    fn_u16 count = 0;
    FnErr e = fn_fuji_device_ready(ctx, &count);
    if (e != FN_OK) {
        return fail("ready", e);
    }
    printf("device ready: %u/512 'A' bytes\n", (unsigned)count);
    return 0;
}

static int cmd_config(FnCtx *ctx)
{
    FnAdapterConfig cfg;
    FnErr e = fn_fuji_get_adapter_config(ctx, &cfg);
    if (e != FN_OK) {
        return fail("config", e);
    }
    print_adapter_config(&cfg);
    return 0;
}

static int cmd_configx(FnCtx *ctx)
{
    FnAdapterConfigExt cfg;
    FnErr e = fn_fuji_get_adapter_config_ext(ctx, &cfg);
    if (e != FN_OK) {
        return fail("configx", e);
    }
    print_adapter_config(&cfg.base);
    printf("s_ip: %s\n", cfg.s_ip);
    printf("s_gateway: %s\n", cfg.s_gateway);
    printf("s_netmask: %s\n", cfg.s_netmask);
    printf("s_dns: %s\n", cfg.s_dns);
    printf("s_mac: %s\n", cfg.s_mac);
    printf("s_bssid: %s\n", cfg.s_bssid);
    return 0;
}

static int cmd_wifi(FnCtx *ctx)
{
    fn_u8 status = 0;
    char ssid[34];
    char pass[65];
    char statusbuf[32];
    FnErr e = fn_fuji_get_wifi_status(ctx, &status);
    if (e != FN_OK) {
        return fail("wifi", e);
    }
    e = fn_fuji_get_ssid(ctx, ssid, pass);
    if (e != FN_OK) {
        return fail("wifi", e);
    }
    printf("status: %u (%s)\n", (unsigned)status, wifi_status_desc(status, statusbuf, sizeof(statusbuf)));
    printf("ssid: %s\n", ssid);
    return 0;
}

static int cmd_scan(FnCtx *ctx)
{
    fn_u8 count = 0;
    fn_u8 idx;
    FnErr e = fn_fuji_scan_networks(ctx, &count);
    if (e != FN_OK) {
        return fail("scan", e);
    }
    for (idx = 0; idx < count; idx++) {
        char ssid[34];
        fn_u8 rssi = 0;
        e = fn_fuji_get_scan_result(ctx, idx, ssid, &rssi);
        if (e != FN_OK) {
            return fail("scan", e);
        }
        printf("%u: %s (%d dBm)\n", (unsigned)idx, ssid, (int)(signed char)rssi);
    }
    return 0;
}

static int cmd_hosts(FnCtx *ctx)
{
    char slots[8][32];
    int i;
    FnErr e = fn_fuji_read_host_slots(ctx, slots);
    if (e != FN_OK) {
        return fail("hosts", e);
    }
    for (i = 0; i < 8; i++) {
        if (slots[i][0] == 0) {
            printf("%d: (empty)\n", i);
        } else {
            char buf[33];
            memcpy(buf, slots[i], 32);
            buf[32] = 0;
            printf("%d: %s\n", i, buf);
        }
    }
    return 0;
}

static int cmd_devices(FnCtx *ctx)
{
    FnDeviceSlot slots[8];
    int i;
    FnErr e = fn_fuji_read_device_slots(ctx, slots);
    if (e != FN_OK) {
        return fail("devices", e);
    }
    for (i = 0; i < 8; i++) {
        printf("%d: host_slot=%u mode=%u name=%s\n",
               i, (unsigned)slots[i].host_slot, (unsigned)slots[i].mode, slots[i].name);
    }
    return 0;
}

static int cmd_mount_host(FnCtx *ctx, int argc, char **argv)
{
    fn_u8 slot;
    FnErr e;
    if (argc < 2) {
        fprintf(stderr, "usage: mount-host N\n");
        return 2;
    }
    slot = (fn_u8)parse_int(argv[1]);
    e = fn_fuji_mount_host(ctx, slot);
    if (e != FN_OK) {
        return fail("mount-host", e);
    }
    printf("mounted host slot %u\n", (unsigned)slot);
    return 0;
}

static int cmd_ls(FnCtx *ctx, int argc, char **argv)
{
    fn_u8 slot;
    const char *path;
    FnErr e;
    int rc = 0;
    int cap;
    int have_dir = 0;
    int ext = getenv("FN_LS_EXT") != NULL;   /* extended entries: date, size, flags */

    if (argc < 2) {
        fprintf(stderr, "usage: ls SLOT [PATH]\n");
        return 2;
    }
    slot = (fn_u8)parse_int(argv[1]);
    path = (argc >= 3) ? argv[2] : "/";

    e = fn_fuji_mount_host(ctx, slot);
    if (e != FN_OK) {
        fprintf(stderr, "warning: mount-host %u: %s\n", (unsigned)slot, fnerr_name(e));
    }

    e = fn_fuji_open_directory(ctx, slot, path, NULL);
    if (e != FN_OK) {
        return fail("ls", e);
    }
    have_dir = 1;

    for (cap = 0; cap < 10000; cap++) {
        char buf[256];
        fn_bool end = FN_FALSE;
        e = fn_fuji_read_directory(ctx, 255, ext ? 0x80 : 0, buf, &end);
        if (e != FN_OK) {
            rc = fail("ls", e);
            break;
        }
        if (end) {
            break;
        }
        if (ext) {
            /* 12-byte details (rs232Fuji::set_additional_direntry_details):
             * date y-70,m,d,h,m,s; size u32 LE; flags; mediatype. */
            const unsigned char *d = (const unsigned char *)buf;
            unsigned long size = d[6] | (d[7] << 8) | ((unsigned long)d[8] << 16) | ((unsigned long)d[9] << 24);
            printf("%04d-%02d-%02d %8lu flags=%02x media=%02x  %s\n", d[0] + 1970, d[1], d[2],
                   size, d[10], d[11], buf + 12);
        } else {
            printf("%s\n", buf);
        }
    }

    if (have_dir) {
        fn_fuji_close_directory(ctx);
    }
    return rc;
}

static int cmd_get(FnCtx *ctx, int argc, char **argv)
{
    const char *url;
    FnErr e;
    unsigned char buf[512];
    fn_u16 avail, got;
    fn_u8 connected, err, err2;
    long idle_ms = 0;
    int hard_error = 0;
    int timed_out = 0;
    int rc;

    if (argc < 2) {
        fprintf(stderr, "usage: get URL\n");
        return 2;
    }
    url = argv[1];

    e = fn_net_open(ctx, 1, fn_devicespec(url), FN_OPEN_READ, FN_TRANS_NONE);
    if (e != FN_OK) {
        return fail("get", e);
    }

    for (;;) {
        e = fn_net_status(ctx, 1, &avail, &connected, &err);
        if (e != FN_OK) {
            fprintf(stderr, "error: get: %s\n", fnerr_name(e));
            hard_error = 1;
            break;
        }
        if (err == 136 || (connected == 0 && avail == 0)) {
            break; /* clean EOF */
        }
        if (avail == 0) {
            usleep(20000);
            idle_ms += 20;
            if (idle_ms >= 15000) {
                timed_out = 1;
                break;
            }
            continue;
        }
        idle_ms = 0;
        e = fn_net_read_avail(ctx, 1, buf, sizeof(buf), &got, &err2);
        if (e != FN_OK) {
            fprintf(stderr, "error: get: %s\n", fnerr_name(e));
            hard_error = 1;
            break;
        }
        fwrite(buf, 1, got, stdout);
    }

    fn_net_close(ctx, 1);

    if (hard_error) {
        rc = 1;
    } else if (timed_out) {
        fprintf(stderr, "error: get: timed out waiting for data (15s idle)\n");
        rc = 1;
    } else {
        rc = 0;
    }
    return rc;
}

static int cmd_json(FnCtx *ctx, int argc, char **argv)
{
    const char *url;
    const char *query;
    FnErr e;
    int opened = 0;
    int rc = 0;
    char outbuf[4096];
    fn_u16 got = 0;

    if (argc < 3) {
        fprintf(stderr, "usage: json URL QUERY\n");
        return 2;
    }
    url = argv[1];
    query = argv[2];

    e = fn_net_open(ctx, 1, fn_devicespec(url), FN_OPEN_READ, FN_TRANS_NONE);
    if (e != FN_OK) {
        return fail("json", e);
    }
    opened = 1;

    /* The underlying HTTP fetch streams in asynchronously (same as `get`
     * sees avail==0 for a few polls right after open): give it a moment to
     * have SOME body bytes buffered before asking the JSON parser to parse
     * and query the document, or the parser sees an empty/partial buffer
     * and every query comes back with avail==0. A few short polls is enough
     * in practice; if nothing shows up we proceed anyway and let parse/query
     * fail normally rather than hanging. */
    {
        int i;
        fn_u16 avail = 0;
        fn_u8 connected = 0, st_err = 0;
        for (i = 0; i < 100; i++) {
            e = fn_net_status(ctx, 1, &avail, &connected, &st_err);
            if (e != FN_OK) {
                rc = fail("json", e);
                goto out;
            }
            if (avail > 0 || connected == 0 || st_err != 0) {
                break;
            }
            usleep(20000);
        }
    }

    e = fn_net_set_parser(ctx, 1, FN_PARSER_JSON);
    if (e != FN_OK) {
        rc = fail("json", e);
        goto out;
    }

    e = fn_net_parse(ctx, 1);
    if (e != FN_OK) {
        rc = fail("json", e);
        goto out;
    }

    e = fn_net_json_query(ctx, 1, query, outbuf, sizeof(outbuf), &got);
    if (e != FN_OK) {
        rc = fail("json", e);
        goto out;
    }

    printf("%s\n", outbuf);

out:
    if (opened) {
        fn_net_close(ctx, 1);
    }
    return rc;
}

/* Step 0 of the netshim spec: confirms the firmware's STATUS semantics for
 * a TCP devicespec ("N:TCP://HOST:PORT/") -- what avail/connected/err report
 * before data arrives, while it's pending, and after the peer closes. Opens
 * unit 1 read/write (mode 12, FN_OPEN_RW), optionally writes TEXT (with
 * literal "\r\n" two-char escapes unescaped to real CR/LF, since shells
 * don't pass real control chars easily), then polls fn_net_status/reads
 * whatever is available for up to ~5s, printing every poll, stopping early
 * once connected==0 and avail==0 (server closed and drained). */
static int cmd_tcp(FnCtx *ctx, int argc, char **argv)
{
    const char *hostport;
    const char *text = NULL;
    char devspec[300];
    char wbuf[1024];
    unsigned char buf[FN_MAX_DATA];
    FnErr e;
    int rc = 0;
    long elapsed_ms = 0;

    if (argc < 2) {
        fprintf(stderr, "usage: tcp HOST:PORT [TEXT]\n");
        return 2;
    }
    hostport = argv[1];
    if (argc >= 3) {
        text = argv[2];
    }

    snprintf(devspec, sizeof(devspec), "N:TCP://%s/", hostport);

    e = fn_net_open(ctx, 1, devspec, FN_OPEN_RW, FN_TRANS_NONE);
    printf("open %s: %s\n", devspec, fnerr_name(e));
    if (e != FN_OK) {
        return fail("tcp", e);
    }

    if (text != NULL) {
        size_t wi = 0, ri;
        size_t tl = strlen(text);
        for (ri = 0; ri < tl && wi < sizeof(wbuf) - 1; ri++) {
            if (text[ri] == '\\' && ri + 1 < tl && text[ri + 1] == 'r') {
                wbuf[wi++] = '\r';
                ri++;
            } else if (text[ri] == '\\' && ri + 1 < tl && text[ri + 1] == 'n') {
                wbuf[wi++] = '\n';
                ri++;
            } else {
                wbuf[wi++] = text[ri];
            }
        }
        e = fn_net_write(ctx, 1, wbuf, (fn_u16)wi);
        printf("write %lu bytes: %s\n", (unsigned long)wi, fnerr_name(e));
        if (e != FN_OK) {
            fn_net_close(ctx, 1);
            return fail("tcp", e);
        }
    }

    for (;;) {
        fn_u16 avail = 0, got = 0;
        fn_u8 connected = 0, err = 0, err2 = 0;

        e = fn_net_status(ctx, 1, &avail, &connected, &err);
        if (e != FN_OK) {
            fprintf(stderr, "error: tcp: status: %s\n", fnerr_name(e));
            rc = 1;
            break;
        }
        printf("status: avail=%u connected=%u err=%u\n",
               (unsigned)avail, (unsigned)connected, (unsigned)err);

        if (avail > 0) {
            e = fn_net_read_avail(ctx, 1, buf, sizeof(buf), &got, &err2);
            if (e != FN_OK) {
                fprintf(stderr, "error: tcp: read: %s\n", fnerr_name(e));
                rc = 1;
                break;
            }
            printf("read %u bytes:\n", (unsigned)got);
            hexdump_stdout(buf, got);
            fwrite(buf, 1, got, stdout);
            printf("\n");
        }

        if (connected == 0 && avail == 0) {
            printf("done: connected=0 avail=0\n");
            break;
        }

        usleep(100000);
        elapsed_ms += 100;
        if (elapsed_ms >= 5000) {
            printf("stopping: 5s poll budget exhausted\n");
            break;
        }
    }

    fn_net_close(ctx, 1);
    return rc;
}

static int cmd_raw(FnCtx *ctx, int argc, char **argv)
{
    fn_u8 dev, cmdb;
    unsigned char payload[FN_MAX_DATA];
    fn_u16 payload_len = 0;
    unsigned char replybuf[FN_MAX_DATA];
    fn_u16 reply_len = 0;
    FnErr e;

    if (argc < 3) {
        fprintf(stderr, "usage: raw DEV CMD [hexpayload]\n");
        return 2;
    }
    dev = (fn_u8)parse_int(argv[1]);
    cmdb = (fn_u8)parse_int(argv[2]);

    if (argc >= 4) {
        const char *hex = argv[3];
        size_t hexlen = strlen(hex);
        size_t i;
        if ((hexlen % 2) != 0) {
            fprintf(stderr, "error: raw: hex payload must have an even number of digits\n");
            return 2;
        }
        payload_len = (fn_u16)(hexlen / 2);
        if (payload_len > sizeof(payload)) {
            fprintf(stderr, "error: raw: payload too large\n");
            return 2;
        }
        for (i = 0; i < payload_len; i++) {
            unsigned int byte = 0;
            if (sscanf(hex + i * 2, "%2x", &byte) != 1) {
                fprintf(stderr, "error: raw: invalid hex digit in payload\n");
                return 2;
            }
            payload[i] = (unsigned char)byte;
        }
    }

    e = fn_bus_call(ctx, dev, cmdb, NULL,
                     (payload_len > 0) ? payload : NULL, payload_len,
                     replybuf, sizeof(replybuf), &reply_len);

    hexdump_stdout(replybuf, reply_len);
    fprintf(stderr, "%s\n", fnerr_name(e));

    return (e == FN_OK) ? 0 : 1;
}

/* ------------------------------------------------------------------ */
/* main / argument parsing                                             */
/* ------------------------------------------------------------------ */

int main(int argc, char **argv)
{
    int i = 1;
    int verbose = 0;
    const char *tcp_host = NULL;
    int tcp_port = 0;
    const char *serial_device = NULL;
    long serial_baud = 0;
    char hostbuf[256];
    char devbuf[256];
    FnTransport *transport;
    FnCtx ctx;
    int rc;
    const char *cmd;
    int cmd_argc;
    char **cmd_argv;

    for (; i < argc; i++) {
        if (strcmp(argv[i], "-t") == 0) {
            char *colon;
            if (i + 1 >= argc) {
                usage(argv[0]);
                return 2;
            }
            i++;
            colon = strrchr(argv[i], ':');
            if (colon == NULL) {
                fprintf(stderr, "error: -t expects host:port\n");
                return 2;
            }
            {
                size_t hostlen = (size_t)(colon - argv[i]);
                if (hostlen >= sizeof(hostbuf)) {
                    hostlen = sizeof(hostbuf) - 1;
                }
                memcpy(hostbuf, argv[i], hostlen);
                hostbuf[hostlen] = 0;
            }
            tcp_host = hostbuf;
            tcp_port = atoi(colon + 1);
        } else if (strcmp(argv[i], "-s") == 0) {
            char *at;
            if (i + 1 >= argc) {
                usage(argv[0]);
                return 2;
            }
            i++;
            at = strrchr(argv[i], '@');
            if (at != NULL) {
                size_t devlen = (size_t)(at - argv[i]);
                if (devlen >= sizeof(devbuf)) {
                    devlen = sizeof(devbuf) - 1;
                }
                memcpy(devbuf, argv[i], devlen);
                devbuf[devlen] = 0;
                serial_device = devbuf;
                serial_baud = strtol(at + 1, NULL, 10);
            } else {
                size_t devlen = strlen(argv[i]);
                if (devlen >= sizeof(devbuf)) {
                    devlen = sizeof(devbuf) - 1;
                }
                memcpy(devbuf, argv[i], devlen);
                devbuf[devlen] = 0;
                serial_device = devbuf;
                serial_baud = 0;
            }
        } else if (strcmp(argv[i], "-v") == 0) {
            verbose = 1;
        } else {
            break;
        }
    }

    if (i >= argc) {
        usage(argv[0]);
        return 2;
    }

    cmd = argv[i];
    cmd_argc = argc - i;
    cmd_argv = &argv[i];

    if (serial_device != NULL) {
        transport = fn_transport_serial_open(serial_device, serial_baud, verbose);
    } else {
        const char *host = (tcp_host != NULL) ? tcp_host : "localhost";
        int port = (tcp_host != NULL) ? tcp_port : 1985;
        transport = fn_transport_tcp_open(host, port, verbose);
    }

    if (transport == NULL) {
        fprintf(stderr, "error: could not open transport\n");
        return 1;
    }

    fn_init(&ctx, transport);

    if (strcmp(cmd, "ready") == 0) {
        rc = cmd_ready(&ctx);
    } else if (strcmp(cmd, "config") == 0) {
        rc = cmd_config(&ctx);
    } else if (strcmp(cmd, "configx") == 0) {
        rc = cmd_configx(&ctx);
    } else if (strcmp(cmd, "wifi") == 0) {
        rc = cmd_wifi(&ctx);
    } else if (strcmp(cmd, "scan") == 0) {
        rc = cmd_scan(&ctx);
    } else if (strcmp(cmd, "hosts") == 0) {
        rc = cmd_hosts(&ctx);
    } else if (strcmp(cmd, "devices") == 0) {
        rc = cmd_devices(&ctx);
    } else if (strcmp(cmd, "mount-host") == 0) {
        rc = cmd_mount_host(&ctx, cmd_argc, cmd_argv);
    } else if (strcmp(cmd, "ls") == 0) {
        rc = cmd_ls(&ctx, cmd_argc, cmd_argv);
    } else if (strcmp(cmd, "get") == 0) {
        rc = cmd_get(&ctx, cmd_argc, cmd_argv);
    } else if (strcmp(cmd, "json") == 0) {
        rc = cmd_json(&ctx, cmd_argc, cmd_argv);
    } else if (strcmp(cmd, "raw") == 0) {
        rc = cmd_raw(&ctx, cmd_argc, cmd_argv);
    } else if (strcmp(cmd, "tcp") == 0) {
        rc = cmd_tcp(&ctx, cmd_argc, cmd_argv);
    } else {
        fprintf(stderr, "error: unrecognized command '%s'\n", cmd);
        usage(argv[0]);
        rc = 2;
    }

    if (serial_device != NULL) {
        fn_transport_serial_close(transport);
    } else {
        fn_transport_tcp_close(transport);
    }

    return rc;
}
