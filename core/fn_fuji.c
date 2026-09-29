/* fn_fuji.c
 *
 * Implementation of the Fuji control device (0x70) client API declared in
 * fn_fuji.h. Built strictly on top of fn_bus_call() from fujibus.h.
 *
 * Portability: ANSI C89. No libc calls (tiny static helpers below instead),
 * no static/global mutable state, no local arrays over 64 bytes -- any
 * larger staging buffer uses ctx->scratch. Every multi-byte wire field is
 * parsed/written byte-by-byte, explicitly, as little-endian.
 *
 * Safety note: the firmware's Fuji command dispatcher (fujiDevice.cpp,
 * processCommand) calls each command's handler lambda unconditionally, and
 * several handlers read packet.param(N) via std::vector::at(N) with no
 * bounds check. With C++ exceptions disabled on the real firmware, an
 * out-of-range param access reboots the device. Every function below sends
 * exactly the params the corresponding firmware handler reads -- no more,
 * no fewer -- every time.
 */

#include "fn_fuji.h"
#include <stddef.h> /* for NULL only; <stddef.h> is a freestanding C89
                       header (no libc calls), required even on Palm OS. */

/* ---- tiny local helpers (no libc on Palm OS) ---------------------------- */

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

static fn_u16 fn_strlen(const char *s)
{
    fn_u16 n;

    n = 0;
    while (s[n] != '\0') {
        n++;
    }
    return n;
}

/* Shared parse of the 140-byte AdapterConfig wire layout (also the leading
 * 140 bytes of AdapterConfigExtended). Field order verified against
 * fujiDevice.h's ADAPTER_CONFIG_FIELDS macro:
 * ssid[33], hostname[64], localIP[4], gateway[4], netmask[4], dnsIP[4],
 * macAddress[6], bssid[6], fn_version[15]. No multi-byte integers appear in
 * this layout, so this is a plain byte-for-byte copy plus defensive NUL
 * forcing -- no endianness conversion is needed. */
static void fn_parse_adapter_config(const fn_u8 *w, FnAdapterConfig *out)
{
    fn_u16 off;

    off = 0;
    fn_memcpy((fn_u8 *)out->ssid, w + off, 33);
    out->ssid[33] = '\0';
    off += 33;

    fn_memcpy((fn_u8 *)out->hostname, w + off, 64);
    out->hostname[64] = '\0';
    off += 64;

    fn_memcpy(out->ip, w + off, 4);
    off += 4;

    fn_memcpy(out->gateway, w + off, 4);
    off += 4;

    fn_memcpy(out->netmask, w + off, 4);
    off += 4;

    fn_memcpy(out->dns, w + off, 4);
    off += 4;

    fn_memcpy(out->mac, w + off, 6);
    off += 6;

    fn_memcpy(out->bssid, w + off, 6);
    off += 6;

    fn_memcpy((fn_u8 *)out->fn_version, w + off, 15);
    out->fn_version[15] = '\0';
}

/* ---- Fuji device commands ------------------------------------------------ */

/* rs232Fuji.cpp rs232_test() (CMD::FUJI_DEVICE_READY): no params; replies with
 * exactly 512 bytes that should all be ASCII 'A' (0x41) on a healthy device. */
FnErr fn_fuji_device_ready(FnCtx *ctx, fn_u16 *count_A)
{
    FnErr err;
    fn_u16 reply_len;
    fn_u16 i;
    fn_u16 n;

    reply_len = 0;
    err = fn_bus_call(ctx, FN_FUJI_DEVICE_ID, 0x00, NULL, NULL, 0,
                       ctx->scratch, FN_MAX_DATA, &reply_len);
    if (err == FN_OK) {
        n = 0;
        for (i = 0; i < reply_len; i++) {
            if (ctx->scratch[i] == 0x41) {
                n++;
            }
        }
        *count_A = n;
    }
    return err;
}

/* fujiDevice.cpp:1213-1224: fujicmd_get_adapter_config; no params; replies
 * with exactly sizeof(AdapterConfig) = 140 raw bytes. */
FnErr fn_fuji_get_adapter_config(FnCtx *ctx, FnAdapterConfig *out)
{
    FnErr err;
    fn_u16 reply_len;

    reply_len = 0;
    err = fn_bus_call(ctx, FN_FUJI_DEVICE_ID, 0xE8, NULL, NULL, 0,
                       ctx->scratch, 140, &reply_len);
    if (err != FN_OK) {
        return err;
    }
    fn_parse_adapter_config(ctx->scratch, out);
    return FN_OK;
}

/* fujiDevice.cpp:1263-1271: extended adapter config; no params; replies with
 * exactly 240 raw bytes: the 140-byte AdapterConfig followed by
 * sLocalIP[16], sGateway[16], sNetmask[16], sDnsIP[16], sMacAddress[18],
 * sBssid[18]. */
FnErr fn_fuji_get_adapter_config_ext(FnCtx *ctx, FnAdapterConfigExt *out)
{
    FnErr err;
    fn_u16 reply_len;
    fn_u16 off;

    reply_len = 0;
    err = fn_bus_call(ctx, FN_FUJI_DEVICE_ID, 0xC4, NULL, NULL, 0,
                       ctx->scratch, 240, &reply_len);
    if (err != FN_OK) {
        return err;
    }

    fn_parse_adapter_config(ctx->scratch, &out->base);
    off = 140;

    fn_memcpy((fn_u8 *)out->s_ip, ctx->scratch + off, 16);
    out->s_ip[15] = '\0';
    off += 16;

    fn_memcpy((fn_u8 *)out->s_gateway, ctx->scratch + off, 16);
    out->s_gateway[15] = '\0';
    off += 16;

    fn_memcpy((fn_u8 *)out->s_netmask, ctx->scratch + off, 16);
    out->s_netmask[15] = '\0';
    off += 16;

    fn_memcpy((fn_u8 *)out->s_dns, ctx->scratch + off, 16);
    out->s_dns[15] = '\0';
    off += 16;

    fn_memcpy((fn_u8 *)out->s_mac, ctx->scratch + off, 18);
    out->s_mac[17] = '\0';
    off += 18;

    fn_memcpy((fn_u8 *)out->s_bssid, ctx->scratch + off, 18);
    out->s_bssid[17] = '\0';

    return FN_OK;
}

/* fujiDevice.cpp:1410-1422: no params; replies with exactly 1 byte
 * (3=connected, 6=disconnected per firmware comment; passed through as-is). */
FnErr fn_fuji_get_wifi_status(FnCtx *ctx, fn_u8 *status)
{
    FnErr err;
    fn_u16 reply_len;

    reply_len = 0;
    err = fn_bus_call(ctx, FN_FUJI_DEVICE_ID, 0xFA, NULL, NULL, 0,
                       ctx->scratch, 1, &reply_len);
    if (err != FN_OK) {
        return err;
    }
    *status = ctx->scratch[0];
    return FN_OK;
}

/* fujiDevice.cpp:578-589: no params; replies with exactly 1 byte (0 or 1). */
FnErr fn_fuji_get_wifi_enabled(FnCtx *ctx, fn_u8 *enabled)
{
    FnErr err;
    fn_u16 reply_len;

    reply_len = 0;
    err = fn_bus_call(ctx, FN_FUJI_DEVICE_ID, 0xEA, NULL, NULL, 0,
                       ctx->scratch, 1, &reply_len);
    if (err != FN_OK) {
        return err;
    }
    *enabled = ctx->scratch[0];
    return FN_OK;
}

/* fujiDevice.cpp:414-446: SSIDConfig; no params; replies with exactly 97
 * bytes: char ssid[33]; char password[64]; */
FnErr fn_fuji_get_ssid(FnCtx *ctx, char ssid[34], char pass[65])
{
    FnErr err;
    fn_u16 reply_len;

    reply_len = 0;
    err = fn_bus_call(ctx, FN_FUJI_DEVICE_ID, 0xFE, NULL, NULL, 0,
                       ctx->scratch, 97, &reply_len);
    if (err != FN_OK) {
        return err;
    }
    fn_memcpy((fn_u8 *)ssid, ctx->scratch, 33);
    ssid[33] = '\0';
    fn_memcpy((fn_u8 *)pass, ctx->scratch + 33, 64);
    pass[64] = '\0';
    return FN_OK;
}

/* fujiDevice.cpp:69-71: SET_SSID is handled unconditionally by the base ctor
 * handler map (fujicmd_net_set_ssid_success(), taking no packet argument at
 * all) -- no wire params. Payload is exactly 97 bytes: 33 bytes of ssid
 * (NUL-padded) followed by 64 bytes of pass (NUL-padded). */
FnErr fn_fuji_set_ssid(FnCtx *ctx, const char *ssid, const char *pass)
{
    fn_u16 ssid_len;
    fn_u16 pass_len;

    fn_memset(ctx->scratch, 0, 97);

    ssid_len = fn_strlen(ssid);
    if (ssid_len > 32) {
        ssid_len = 32;
    }
    fn_memcpy(ctx->scratch, (const fn_u8 *)ssid, ssid_len);

    pass_len = fn_strlen(pass);
    if (pass_len > 63) {
        pass_len = 63;
    }
    fn_memcpy(ctx->scratch + 33, (const fn_u8 *)pass, pass_len);

    return fn_bus_call(ctx, FN_FUJI_DEVICE_ID, 0xFB, NULL, ctx->scratch, 97, NULL, 0, NULL);
}

/* fujiDevice.cpp:494-504: no params; replies with exactly 1 byte, the count
 * of scanned SSIDs. */
FnErr fn_fuji_scan_networks(FnCtx *ctx, fn_u8 *count)
{
    FnErr err;
    fn_u16 reply_len;

    reply_len = 0;
    err = fn_bus_call(ctx, FN_FUJI_DEVICE_ID, 0xFD, NULL, NULL, 0,
                       ctx->scratch, 1, &reply_len);
    if (err != FN_OK) {
        return err;
    }
    *count = ctx->scratch[0];
    return FN_OK;
}

/* fujiDevice.cpp:63-65: ctor lambda reads packet.param(0) as scan index.
 * Reply is exactly 34 bytes (SSIDInfo, fujiDevice.cpp:506-517):
 * char ssid[33]; unsigned char rssi; */
FnErr fn_fuji_get_scan_result(FnCtx *ctx, fn_u8 idx, char ssid[34], fn_u8 *rssi)
{
    FnParams p;
    FnErr err;
    fn_u16 reply_len;

    fn_params_none(&p);
    fn_params_add_u8(&p, idx);

    reply_len = 0;
    err = fn_bus_call(ctx, FN_FUJI_DEVICE_ID, 0xFC, &p, NULL, 0,
                       ctx->scratch, 34, &reply_len);
    if (err != FN_OK) {
        return err;
    }
    fn_memcpy((fn_u8 *)ssid, ctx->scratch, 33);
    ssid[33] = '\0';
    *rssi = ctx->scratch[33];
    return FN_OK;
}

/* fujiDevice.cpp:1424-1435: no params; replies with exactly 256 bytes = 8
 * slots of MAX_HOSTNAME_LEN(32) raw bytes each, same layout as slots. */
FnErr fn_fuji_read_host_slots(FnCtx *ctx, char slots[8][32])
{
    FnErr err;
    fn_u16 reply_len;

    reply_len = 0;
    err = fn_bus_call(ctx, FN_FUJI_DEVICE_ID, 0xF4, NULL, NULL, 0,
                       ctx->scratch, 256, &reply_len);
    if (err != FN_OK) {
        return err;
    }
    fn_memcpy((fn_u8 *)slots, ctx->scratch, 256);
    return FN_OK;
}

/* fujiDevice.cpp:1438-1458: reads exactly sizeof(hostSlots) = 256 bytes via
 * transaction_get; no params. slots is already contiguous 256 bytes in the
 * right shape, so it is sent directly with no staging needed. */
FnErr fn_fuji_write_host_slots(FnCtx *ctx, const char slots[8][32])
{
    return fn_bus_call(ctx, FN_FUJI_DEVICE_ID, 0xF3, NULL, slots, 256, NULL, 0, NULL);
}

/* fujiDevice.cpp:1575-1610: disk_slot; no params; replies with exactly 304
 * bytes = 8 * { u8 hostSlot; u8 mode; char filename[36]; } in that field
 * order. */
FnErr fn_fuji_read_device_slots(FnCtx *ctx, FnDeviceSlot slots[8])
{
    FnErr err;
    fn_u16 reply_len;
    fn_u8 i;
    fn_u16 base;

    reply_len = 0;
    err = fn_bus_call(ctx, FN_FUJI_DEVICE_ID, 0xF2, NULL, NULL, 0,
                       ctx->scratch, 304, &reply_len);
    if (err != FN_OK) {
        return err;
    }
    for (i = 0; i < FN_DEVICE_SLOT_COUNT; i++) {
        base = (fn_u16)(i * 38);
        slots[i].host_slot = ctx->scratch[base + 0];
        slots[i].mode = ctx->scratch[base + 1];
        fn_memcpy((fn_u8 *)slots[i].name, ctx->scratch + base + 2, 36);
        slots[i].name[35] = '\0';
    }
    return FN_OK;
}

/* fujiDevice.cpp:1613 onward: reads exactly sizeof(disk_slot)*_totalDiskDevices
 * = 304 bytes via transaction_get; no params. Names are copied verbatim (all
 * 36 bytes), matching the firmware's own tolerance for non-NUL-padded input. */
FnErr fn_fuji_write_device_slots(FnCtx *ctx, const FnDeviceSlot slots[8])
{
    fn_u8 i;
    fn_u16 base;

    for (i = 0; i < FN_DEVICE_SLOT_COUNT; i++) {
        base = (fn_u16)(i * 38);
        ctx->scratch[base + 0] = slots[i].host_slot;
        ctx->scratch[base + 1] = slots[i].mode;
        fn_memcpy(ctx->scratch + base + 2, (const fn_u8 *)slots[i].name, 36);
    }
    return fn_bus_call(ctx, FN_FUJI_DEVICE_ID, 0xF1, NULL, ctx->scratch, 304, NULL, 0, NULL);
}

/* fujiDevice.cpp:93-95: ctor lambda reads packet.param(0) as hostSlot. No
 * payload/reply data. */
FnErr fn_fuji_mount_host(FnCtx *ctx, fn_u8 slot)
{
    FnParams p;

    fn_params_none(&p);
    fn_params_add_u8(&p, slot);
    return fn_bus_call(ctx, FN_FUJI_DEVICE_ID, 0xF9, &p, NULL, 0, NULL, 0, NULL);
}

/* fujiDevice.cpp:119-121: ctor lambda reads packet.param(0) as hostSlot. No
 * payload/reply data. */
FnErr fn_fuji_unmount_host(FnCtx *ctx, fn_u8 slot)
{
    FnParams p;

    fn_params_none(&p);
    fn_params_add_u8(&p, slot);
    return fn_bus_call(ctx, FN_FUJI_DEVICE_ID, 0xE6, &p, NULL, 0, NULL, 0, NULL);
}

/* fujiDevice.cpp:96-98: ctor lambda reads packet.param(0) as hostSlot.
 * fujiDevice.cpp:729-747: fujicore_open_directory_success splits the payload
 * on its first embedded NUL into path and an optional trailing filter/
 * pattern; payload is path bytes, one NUL, then (if filter given) filter
 * bytes and one more NUL. The firmware's receive buffer is zero-initialized
 * so no padding to a fixed size is required. */
FnErr fn_fuji_open_directory(FnCtx *ctx, fn_u8 host_slot, const char *path, const char *filter)
{
    FnParams p;
    fn_u16 len;
    fn_u16 plen;

    fn_params_none(&p);
    fn_params_add_u8(&p, host_slot);

    /* The firmware reads a fixed FN_DIR_PATH_LEN-byte area, "path\0filter\0"
     * zero-padded, and NAKs a shorter payload. */
    fn_memset(ctx->scratch, 0, FN_DIR_PATH_LEN);
    plen = fn_strlen(path);
    if (plen > FN_DIR_PATH_LEN - 2) {
        plen = FN_DIR_PATH_LEN - 2;
    }
    fn_memcpy(ctx->scratch, (const fn_u8 *)path, plen);
    len = (fn_u16)(plen + 1);

    if (filter != NULL) {
        fn_u16 flen;

        flen = fn_strlen(filter);
        if (flen > FN_DIR_PATH_LEN - 1 - len) {
            flen = (fn_u16)(FN_DIR_PATH_LEN - 1 - len);
        }
        fn_memcpy(ctx->scratch + len, (const fn_u8 *)filter, flen);
    }

    return fn_bus_call(ctx, FN_FUJI_DEVICE_ID, 0xF7, &p, ctx->scratch, FN_DIR_PATH_LEN, NULL, 0, NULL);
}

/* fujiDevice.cpp:102-104: ctor lambda reads packet.param(0)=maxlen (as u8),
 * packet.param(1)=flags -- both are sent every time or the firmware reboots.
 * Reply is exactly maxlen bytes, NUL-padded by the firmware if the entry was
 * shorter (fujiDevice.cpp:1028-1060). End-of-directory is signalled by the
 * first two reply bytes both being 0x7F (fujiDevice.cpp:990-991). Caller
 * must supply an out buffer of at least maxlen+1 bytes. */
FnErr fn_fuji_read_directory(FnCtx *ctx, fn_u8 maxlen, fn_u8 flags, char *out, fn_bool *end)
{
    FnParams p;
    FnErr err;
    fn_u16 reply_len;

    fn_params_none(&p);
    fn_params_add_u8(&p, maxlen);
    fn_params_add_u8(&p, flags);

    reply_len = 0;
    err = fn_bus_call(ctx, FN_FUJI_DEVICE_ID, 0xF6, &p, NULL, 0,
                       ctx->scratch, maxlen, &reply_len);
    if (err != FN_OK) {
        return err;
    }

    if (maxlen >= 2 && ctx->scratch[0] == 0x7F && ctx->scratch[1] == 0x7F) {
        *end = FN_DIR_END;
        out[0] = '\0';
        return FN_OK;
    }

    *end = FN_FALSE;
    fn_memcpy((fn_u8 *)out, ctx->scratch, maxlen);
    out[maxlen] = '\0';
    return FN_OK;
}

/* fujiDevice.cpp:99-101: ctor lambda takes no params; no payload/reply. */
FnErr fn_fuji_close_directory(FnCtx *ctx)
{
    return fn_bus_call(ctx, FN_FUJI_DEVICE_ID, 0xF5, NULL, NULL, 0, NULL, 0, NULL);
}

/* fujiDevice.cpp:105-107: fujicmd_set_directory_position(uint16_t pos) --
 * ONE u16 param. */
FnErr fn_fuji_set_directory_position(FnCtx *ctx, fn_u16 pos)
{
    FnParams p;

    fn_params_none(&p);
    fn_params_add_u16(&p, pos);
    return fn_bus_call(ctx, FN_FUJI_DEVICE_ID, 0xE4, &p, NULL, 0, NULL, 0, NULL);
}

/* fujiDevice.cpp:149-151: fujicore_get_directory_position returns uint16_t,
 * sent raw via transaction_send (little-endian on this protocol); no params.
 * Reply is 2 bytes, parsed explicitly rather than cast/overlaid. */
FnErr fn_fuji_get_directory_position(FnCtx *ctx, fn_u16 *pos)
{
    FnErr err;
    fn_u16 reply_len;

    reply_len = 0;
    err = fn_bus_call(ctx, FN_FUJI_DEVICE_ID, 0xE5, NULL, NULL, 0,
                       ctx->scratch, 2, &reply_len);
    if (err != FN_OK) {
        return err;
    }
    *pos = (fn_u16)(ctx->scratch[0] | (ctx->scratch[1] << 8));
    return FN_OK;
}

/* fujiDevice.h:203-207: fujidev_set_device_fullpath reads packet.param(0)=
 * dev_slot, packet.param(1)=host_slot, packet.param(2)=mode, in that order.
 * Payload is path bytes plus a trailing NUL (harmless and simpler than
 * omitting it; the firmware's receive buffer is zero-initialized either way). */
FnErr fn_fuji_set_device_fullpath(FnCtx *ctx, fn_u8 dev_slot, fn_u8 host_slot, fn_u8 mode, const char *path)
{
    FnParams p;
    fn_u16 plen;

    fn_params_none(&p);
    fn_params_add_u8(&p, dev_slot);
    fn_params_add_u8(&p, host_slot);
    fn_params_add_u8(&p, mode);

    plen = fn_strlen(path);
    fn_memcpy(ctx->scratch, (const fn_u8 *)path, plen);
    ctx->scratch[plen] = 0;

    return fn_bus_call(ctx, FN_FUJI_DEVICE_ID, 0xE2, &p, ctx->scratch, (fn_u16)(plen + 1), NULL, 0, NULL);
}

/* fujiDevice.cpp:111-113: ctor lambda reads packet.param(0) as dev_slot.
 * Reply is up to 256 bytes (MAX_FILENAME_LEN), NUL-terminated within that by
 * the firmware's zero-initialized buffer (fujiDevice.cpp:1282-1295). The
 * receive length is capped so it can never exceed out_cap-1, so the
 * terminator always fits inside out. */
FnErr fn_fuji_get_device_fullpath(FnCtx *ctx, fn_u8 dev_slot, char *out, fn_u16 out_cap)
{
    FnParams p;
    FnErr err;
    fn_u16 reply_max;
    fn_u16 reply_len;

    if (out_cap == 0) {
        return FN_ERR_PARAM;
    }

    fn_params_none(&p);
    fn_params_add_u8(&p, dev_slot);

    reply_max = (fn_u16)(out_cap - 1);
    if (reply_max > 256) {
        reply_max = 256;
    }

    reply_len = 0;
    err = fn_bus_call(ctx, FN_FUJI_DEVICE_ID, 0xDA, &p, NULL, 0,
                       ctx->scratch, reply_max, &reply_len);
    if (err != FN_OK) {
        return err;
    }
    fn_memcpy((fn_u8 *)out, ctx->scratch, reply_len);
    out[reply_len] = '\0';
    return FN_OK;
}

/* fujiDevice.cpp:114-118: ctor lambda reads packet.param(0)=dev_slot,
 * packet.param(1)=mode, in that order. No payload/reply data. */
FnErr fn_fuji_mount_image(FnCtx *ctx, fn_u8 dev_slot, fn_u8 mode)
{
    FnParams p;

    fn_params_none(&p);
    fn_params_add_u8(&p, dev_slot);
    fn_params_add_u8(&p, mode);
    return fn_bus_call(ctx, FN_FUJI_DEVICE_ID, 0xF8, &p, NULL, 0, NULL, 0, NULL);
}

/* fujiDevice.cpp:122-124: ctor lambda reads packet.param(0) as dev_slot. No
 * payload/reply data. */
FnErr fn_fuji_unmount_image(FnCtx *ctx, fn_u8 dev_slot)
{
    FnParams p;

    fn_params_none(&p);
    fn_params_add_u8(&p, dev_slot);
    return fn_bus_call(ctx, FN_FUJI_DEVICE_ID, 0xE9, &p, NULL, 0, NULL, 0, NULL);
}
