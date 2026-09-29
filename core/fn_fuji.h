/* fn_fuji.h
 *
 * Client-side API for the FujiBus "Fuji" control device (0x70). This layer
 * sits directly on top of core/fujibus.h's fn_bus_call() transaction
 * primitive and packages up every Fuji command with its exact, firmware-
 * verified parameter list and reply layout.
 *
 * Portability: ANSI C89. No libc calls, no static/global mutable state (all
 * state lives in the caller-supplied FnCtx). See fn_fuji.c for the small
 * static fn_memcpy/fn_memset/fn_strlen helpers used internally.
 */

#ifndef FN_FUJI_H
#define FN_FUJI_H

#include "fujibus.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Fuji control device id on the FujiBus. */
#define FN_FUJI_DEVICE_ID 0x70

#define FN_HOST_SLOT_COUNT 8
#define FN_HOST_SLOT_NAME_LEN 32   /* fixed wire size per host slot name; see fn_fuji_read_host_slots */
#define FN_DEVICE_SLOT_COUNT 8

#define FN_DISK_MODE_READ  0x01
#define FN_DISK_MODE_WRITE 0x02

#define FN_DIR_PATH_LEN 256        /* fixed size of the open-directory "path\0filter" payload */
#define FN_DIR_END 1               /* value written to *end by fn_fuji_read_directory at end-of-directory */

/* Parsed (friendly) AdapterConfig. Wire layout is 140 raw bytes; see fn_fuji.c
 * for the exact field-by-field parse. ssid/hostname/fn_version are always
 * NUL-forced by us at the index shown, even though the firmware's own
 * strlcpy-filled fields are expected to already be terminated within bounds. */
typedef struct {
    char   ssid[34];        /* wire char[33]; ssid[33] is always forced to 0 */
    char   hostname[65];    /* wire char[64]; hostname[64] is always forced to 0 */
    fn_u8  ip[4];
    fn_u8  gateway[4];
    fn_u8  netmask[4];
    fn_u8  dns[4];
    fn_u8  mac[6];
    fn_u8  bssid[6];
    char   fn_version[16];  /* wire char[15]; fn_version[15] is always forced to 0 */
} FnAdapterConfig;

/* Parsed AdapterConfigExtended: the 140 shared bytes above, followed by six
 * string fields (240 bytes total on the wire). Each s_* field's last byte is
 * always forced to 0. */
typedef struct {
    FnAdapterConfig base;
    char s_ip[16];
    char s_gateway[16];
    char s_netmask[16];
    char s_dns[16];
    char s_mac[18];
    char s_bssid[18];
} FnAdapterConfigExt;

/* Parsed device (disk) slot entry: wire order is hostSlot, mode, filename[36]. */
typedef struct {
    fn_u8 host_slot;
    fn_u8 mode;
    char  name[36];   /* wire-exact size; name[35] is always forced to 0 defensively */
} FnDeviceSlot;

/* cmd 0x00: device-ready probe. Reply is 512 bytes that should all be 'A'
 * (0x41) on a healthy device. *count_A receives how many of the received
 * reply bytes were 'A' (only set when fn_bus_call returns FN_OK); a value
 * less than 512 indicates a partial/corrupted reply without being treated
 * as a hard error. Returns whatever fn_bus_call returned. */
FnErr fn_fuji_device_ready(FnCtx *ctx, fn_u16 *count_A);

/* cmd 0xE8: read the 140-byte AdapterConfig, parsed into *out. *out is left
 * untouched unless the call succeeds. */
FnErr fn_fuji_get_adapter_config(FnCtx *ctx, FnAdapterConfig *out);

/* cmd 0xC4: read the 240-byte AdapterConfigExtended, parsed into *out. *out
 * is left untouched unless the call succeeds. */
FnErr fn_fuji_get_adapter_config_ext(FnCtx *ctx, FnAdapterConfigExt *out);

/* cmd 0xFA: read raw wifi status byte (3=connected, 6=disconnected, per
 * firmware comment; passed through as-is). */
FnErr fn_fuji_get_wifi_status(FnCtx *ctx, fn_u8 *status);

/* cmd 0xEA: read wifi-enabled flag (0 or 1). */
FnErr fn_fuji_get_wifi_enabled(FnCtx *ctx, fn_u8 *enabled);

/* cmd 0xFE: read the currently configured SSID/password. ssid must be at
 * least 34 bytes, pass at least 65 bytes; both are always NUL-terminated
 * by us on success. */
FnErr fn_fuji_get_ssid(FnCtx *ctx, char ssid[34], char pass[65]);

/* cmd 0xFB: set SSID/password. No wire params (see fn_fuji.c). ssid/pass are
 * ordinary NUL-terminated C strings; ssid is truncated to 32 bytes and pass
 * to 63 bytes on the wire, per the fixed 33/64-byte wire fields. */
FnErr fn_fuji_set_ssid(FnCtx *ctx, const char *ssid, const char *pass);

/* cmd 0xFD: trigger a wifi scan; *count receives the number of SSIDs found. */
FnErr fn_fuji_scan_networks(FnCtx *ctx, fn_u8 *count);

/* cmd 0xFC: read one scanned SSID result by index. ssid must be at least 34
 * bytes and is always NUL-terminated by us on success. */
FnErr fn_fuji_get_scan_result(FnCtx *ctx, fn_u8 idx, char ssid[34], fn_u8 *rssi);

/* cmd 0xF4: read all 8 host slot names (32 raw bytes each, 256 bytes total)
 * directly into slots. Wire-exact; no extra NUL byte is added. */
FnErr fn_fuji_read_host_slots(FnCtx *ctx, char slots[8][32]);

/* cmd 0xF3: write all 8 host slot names (256 bytes total) from slots as-is. */
FnErr fn_fuji_write_host_slots(FnCtx *ctx, const char slots[8][32]);

/* cmd 0xF2: read all 8 device (disk) slots into slots[8]. Each name[35] is
 * always forced to 0 defensively. */
FnErr fn_fuji_read_device_slots(FnCtx *ctx, FnDeviceSlot slots[8]);

/* cmd 0xF1: write all 8 device (disk) slots from slots[8]. Each name is
 * copied verbatim (all 36 bytes, not strlen-bounded). */
FnErr fn_fuji_write_device_slots(FnCtx *ctx, const FnDeviceSlot slots[8]);

/* cmd 0xF9: mount a TNFS host by slot index. */
FnErr fn_fuji_mount_host(FnCtx *ctx, fn_u8 slot);

/* cmd 0xE6: unmount a TNFS host by slot index. */
FnErr fn_fuji_unmount_host(FnCtx *ctx, fn_u8 slot);

/* cmd 0xF7: open a directory listing on a host. filter may be NULL for no
 * filter/pattern. */
FnErr fn_fuji_open_directory(FnCtx *ctx, fn_u8 host_slot, const char *path, const char *filter);

/* cmd 0xF6: read one directory entry of up to maxlen bytes. `out` must point
 * to a buffer of at least maxlen+1 bytes (caller-owned; this function never
 * allocates). On success, *end is set to FN_DIR_END and out[0]=0 at
 * end-of-directory, otherwise *end is FN_FALSE and out holds the
 * NUL-terminated (at out[maxlen]) raw entry bytes. Neither out nor *end is
 * touched unless fn_bus_call succeeds. */
FnErr fn_fuji_read_directory(FnCtx *ctx, fn_u8 maxlen, fn_u8 flags, char *out, fn_bool *end);

/* cmd 0xF5: close the currently open directory listing. */
FnErr fn_fuji_close_directory(FnCtx *ctx);

/* cmd 0xE4: seek the open directory listing to entry index pos. */
FnErr fn_fuji_set_directory_position(FnCtx *ctx, fn_u16 pos);

/* cmd 0xE5: read the open directory listing's current entry index. */
FnErr fn_fuji_get_directory_position(FnCtx *ctx, fn_u16 *pos);

/* cmd 0xE2: set a device slot's full mount path. */
FnErr fn_fuji_set_device_fullpath(FnCtx *ctx, fn_u8 dev_slot, fn_u8 host_slot, fn_u8 mode, const char *path);

/* cmd 0xDA: read a device slot's full mount path. out_cap is the total size
 * of the out buffer including room for the terminating NUL; out_cap==0
 * returns FN_ERR_PARAM without making a bus call. out is always
 * NUL-terminated on success and never written to at or beyond
 * out[out_cap-1]. */
FnErr fn_fuji_get_device_fullpath(FnCtx *ctx, fn_u8 dev_slot, char *out, fn_u16 out_cap);

/* cmd 0xF8: mount a disk image already assigned to dev_slot, in the given mode. */
FnErr fn_fuji_mount_image(FnCtx *ctx, fn_u8 dev_slot, fn_u8 mode);

/* cmd 0xE9: unmount the disk image in dev_slot. */
FnErr fn_fuji_unmount_image(FnCtx *ctx, fn_u8 dev_slot);

#ifdef __cplusplus
}
#endif

#endif /* FN_FUJI_H */
