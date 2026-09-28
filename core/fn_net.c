/*
 * fn_net.c -- FujiBus "N" network device API implementation.
 *
 * ANSI C89. No libc calls; tiny local helpers are defined below in place of
 * any string.h functions actually needed by this file. No static/global
 * mutable state -- all state lives in the caller's FnCtx or on the stack.
 */
#include "fn_net.h"

/* ---- tiny local helper (no libc calls; see portability rules) ---- */

static fn_u16 fn_strlen(const char *s)
{
    fn_u16 n = 0;

    while (s[n] != '\0') {
        n++;
    }
    return n;
}

/*
 * Open a network unit. Cmd 'O' (0x4F); param0=mode, param1=trans
 * (NDevice.cpp:138-141, fujidev_open requires both params or the firmware
 * crashes). Payload = devicespec bytes as-is (NDevice.cpp:143-151).
 */
FnErr fn_net_open(FnCtx *ctx, fn_u8 unit, const char *devicespec,
                   fn_u8 mode, fn_u8 trans)
{
    FnParams p;
    fn_u8 dev;
    fn_u16 len;

    if (unit < 1 || unit > 8) {
        return FN_ERR_PARAM;
    }

    dev = (fn_u8)(FN_NET_DEVICE_BASE + (unit - 1));
    len = (devicespec != 0) ? fn_strlen(devicespec) : 0;

    fn_params_none(&p);
    fn_params_add_u8(&p, mode);
    fn_params_add_u8(&p, trans);

    return fn_bus_call(ctx, dev, (fn_u8)'O', &p, devicespec, len, 0, 0, 0);
}

/*
 * Close a network unit. Cmd 'C' (0x43); no params, no payload
 * (NDevice.cpp:196-209, fujidev_close ignores the packet entirely).
 */
FnErr fn_net_close(FnCtx *ctx, fn_u8 unit)
{
    FnParams p;
    fn_u8 dev;

    if (unit < 1 || unit > 8) {
        return FN_ERR_PARAM;
    }

    dev = (fn_u8)(FN_NET_DEVICE_BASE + (unit - 1));
    fn_params_none(&p);

    return fn_bus_call(ctx, dev, (fn_u8)'C', &p, 0, 0, 0, 0, 0);
}

/*
 * Query status of a network unit. Cmd 'S' (0x53); no params. Reply is
 * exactly 4 raw bytes: avail (u16 LE), connected (u8), err (u8)
 * (NDevice.cpp:333-339 + NDeviceStatus struct, sizeof==4, sent raw so
 * native/LE byte order).
 */
FnErr fn_net_status(FnCtx *ctx, fn_u8 unit, fn_u16 *avail,
                     fn_u8 *connected, fn_u8 *err)
{
    FnParams p;
    fn_u8 dev;
    FnErr rc;

    if (unit < 1 || unit > 8) {
        return FN_ERR_PARAM;
    }

    dev = (fn_u8)(FN_NET_DEVICE_BASE + (unit - 1));
    fn_params_none(&p);

    rc = fn_bus_call(ctx, dev, (fn_u8)'S', &p, 0, 0, ctx->scratch, 4, 0);
    if (rc != FN_OK) {
        return rc;
    }

    if (avail != 0) {
        *avail = (fn_u16)(ctx->scratch[0] | (ctx->scratch[1] << 8));
    }
    if (connected != 0) {
        *connected = ctx->scratch[2];
    }
    if (err != 0) {
        *err = ctx->scratch[3];
    }

    return FN_OK;
}

/*
 * Read up to len bytes from a network unit. Cmd 'R' (0x52); param0=len as
 * u16 (NDevice.cpp:222-242, fujidev_read reads packet.param(0) as uint16_t
 * num_bytes). Caller must never call this on an unopened unit or request
 * more than the last known avail -- see fn_net.h for the full hazard note.
 */
FnErr fn_net_read(FnCtx *ctx, fn_u8 unit, void *buf, fn_u16 len)
{
    FnParams p;
    fn_u8 dev;

    if (unit < 1 || unit > 8) {
        return FN_ERR_PARAM;
    }
    if (len == 0 || len > FN_MAX_DATA) {
        return FN_ERR_PARAM;
    }

    dev = (fn_u8)(FN_NET_DEVICE_BASE + (unit - 1));
    fn_params_none(&p);
    fn_params_add_u16(&p, len);

    return fn_bus_call(ctx, dev, (fn_u8)'R', &p, 0, 0, buf, len, 0);
}

/*
 * Composite helper: fn_net_status() then fn_net_read() for at most avail
 * bytes, so it never violates fn_net_read()'s "never request more than
 * avail" rule by construction.
 */
FnErr fn_net_read_avail(FnCtx *ctx, fn_u8 unit, void *buf, fn_u16 max,
                         fn_u16 *got, fn_u8 *err)
{
    FnErr rc;
    fn_u16 avail;
    fn_u8 status_err;
    fn_u16 n;

    if (unit < 1 || unit > 8) {
        return FN_ERR_PARAM;
    }

    rc = fn_net_status(ctx, unit, &avail, 0, &status_err);
    if (rc != FN_OK) {
        return rc;
    }

    n = avail;
    if (n > max) {
        n = max;
    }
    if (n > FN_MAX_DATA) {
        n = FN_MAX_DATA;
    }

    if (n == 0) {
        if (got != 0) {
            *got = 0;
        }
        if (err != 0) {
            *err = status_err;
        }
        return FN_OK;
    }

    rc = fn_net_read(ctx, unit, buf, n);
    if (rc != FN_OK) {
        return rc;
    }

    if (got != 0) {
        *got = n;
    }
    if (err != 0) {
        *err = status_err;
    }

    return FN_OK;
}

/*
 * Write len bytes to a network unit. Cmd 'W' (0x57); param0=len as u16
 * (NDevice.cpp:255-290, fujidev_write reads packet.param(0) as uint16_t
 * num_bytes).
 */
FnErr fn_net_write(FnCtx *ctx, fn_u8 unit, const void *buf, fn_u16 len)
{
    FnParams p;
    fn_u8 dev;

    if (unit < 1 || unit > 8) {
        return FN_ERR_PARAM;
    }
    if (len == 0 || len > FN_MAX_DATA) {
        return FN_ERR_PARAM;
    }

    dev = (fn_u8)(FN_NET_DEVICE_BASE + (unit - 1));
    fn_params_none(&p);
    fn_params_add_u16(&p, len);

    return fn_bus_call(ctx, dev, (fn_u8)'W', &p, buf, len, 0, 0, 0);
}

/*
 * Set the response parser mode. Cmd 0xFC; param0=0 (dummy), param1=mode
 * (NDevice.cpp:530-539, fujidev_set_parser reads
 * param_cast<parserMode_t>(packet,1) -- index 1, so both params are
 * required or the firmware's .at(1) throws).
 */
FnErr fn_net_set_parser(FnCtx *ctx, fn_u8 unit, fn_u8 mode)
{
    FnParams p;
    fn_u8 dev;

    if (unit < 1 || unit > 8) {
        return FN_ERR_PARAM;
    }

    dev = (fn_u8)(FN_NET_DEVICE_BASE + (unit - 1));
    fn_params_none(&p);
    fn_params_add_u8(&p, 0);
    fn_params_add_u8(&p, mode);

    return fn_bus_call(ctx, dev, (fn_u8)0xFC, &p, 0, 0, 0, 0, 0);
}

/*
 * Parse the last response with the previously-set parser. Cmd 'P' (0x50);
 * no params (NDevice.cpp:569-588, fujidev_do_parse takes no packet params;
 * a NAK from the firmware if no parser was set propagates as FN_ERR_NAK).
 */
FnErr fn_net_parse(FnCtx *ctx, fn_u8 unit)
{
    FnParams p;
    fn_u8 dev;

    if (unit < 1 || unit > 8) {
        return FN_ERR_PARAM;
    }

    dev = (fn_u8)(FN_NET_DEVICE_BASE + (unit - 1));
    fn_params_none(&p);

    return fn_bus_call(ctx, dev, (fn_u8)'P', &p, 0, 0, 0, 0, 0);
}

/*
 * Send a query string. Cmd 'Q' (0x51); no params. The RS232 transport
 * overrides this handler to read the whole payload verbatim
 * (rs232Network.cpp:6-11, fujidev_set_query uses packet.dataAsString()), so
 * payload = query bytes. The trailing NUL is sent too: older firmware
 * (lib/device/rs232/network.cpp rs232_set_json_query, e.g. fujinet-pc
 * v1.6-69dd4cc55) copies the payload into an uninitialised 256-byte buffer
 * and never terminates it, so short queries picked up stack garbage. Newer
 * firmware stops at the NUL when it calls c_str().
 */
FnErr fn_net_query(FnCtx *ctx, fn_u8 unit, const char *query)
{
    FnParams p;
    fn_u8 dev;
    fn_u16 len;

    if (unit < 1 || unit > 8) {
        return FN_ERR_PARAM;
    }

    dev = (fn_u8)(FN_NET_DEVICE_BASE + (unit - 1));
    len = (query != 0) ? (fn_u16)(fn_strlen(query) + 1) : 0;
    fn_params_none(&p);

    return fn_bus_call(ctx, dev, (fn_u8)'Q', &p, query, len, 0, 0, 0);
}

/*
 * Composite helper: fn_net_query(), then fn_net_status(), then read
 * min(avail, max-1, FN_MAX_DATA) bytes into out and NUL-terminate it.
 */
FnErr fn_net_json_query(FnCtx *ctx, fn_u8 unit, const char *query,
                         char *out, fn_u16 max, fn_u16 *got)
{
    FnErr rc;
    fn_u16 avail;
    fn_u16 n;

    if (unit < 1 || unit > 8) {
        return FN_ERR_PARAM;
    }
    if (max == 0) {
        return FN_ERR_PARAM;
    }

    rc = fn_net_query(ctx, unit, query);
    if (rc != FN_OK) {
        return rc;
    }

    rc = fn_net_status(ctx, unit, &avail, 0, 0);
    if (rc != FN_OK) {
        return rc;
    }

    n = avail;
    if (n > (fn_u16)(max - 1)) {
        n = (fn_u16)(max - 1);
    }
    if (n > FN_MAX_DATA) {
        n = FN_MAX_DATA;
    }

    if (n > 0) {
        rc = fn_net_read(ctx, unit, out, n);
        if (rc != FN_OK) {
            return rc;
        }
    }

    out[n] = 0;
    if (got != 0) {
        *got = n;
    }

    return FN_OK;
}

/*
 * Set HTTP channel mode. Cmd 'M' (0x4D); param0=0 (dummy), param1=mode
 * (NDevice.cpp:805-821, fujidev_http_set_channel_mode reads
 * param_cast<...>(packet,1)).
 */
FnErr fn_net_set_http_mode(FnCtx *ctx, fn_u8 unit, fn_u8 mode)
{
    FnParams p;
    fn_u8 dev;

    if (unit < 1 || unit > 8) {
        return FN_ERR_PARAM;
    }

    dev = (fn_u8)(FN_NET_DEVICE_BASE + (unit - 1));
    fn_params_none(&p);
    fn_params_add_u8(&p, 0);
    fn_params_add_u8(&p, mode);

    return fn_bus_call(ctx, dev, (fn_u8)'M', &p, 0, 0, 0, 0, 0);
}

/*
 * Set translation mode. Cmd 'T' (0x54); param0=0 (dummy), param1=mode
 * (NDevice.h:198-202, fujidev_set_translation reads packet.param(1)).
 */
FnErr fn_net_set_translation(FnCtx *ctx, fn_u8 unit, fn_u8 mode)
{
    FnParams p;
    fn_u8 dev;

    if (unit < 1 || unit > 8) {
        return FN_ERR_PARAM;
    }

    dev = (fn_u8)(FN_NET_DEVICE_BASE + (unit - 1));
    fn_params_none(&p);
    fn_params_add_u8(&p, 0);
    fn_params_add_u8(&p, mode);

    return fn_bus_call(ctx, dev, (fn_u8)'T', &p, 0, 0, 0, 0, 0);
}
