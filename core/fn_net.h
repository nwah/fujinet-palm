/*
 * fn_net.h -- FujiBus "N" network device API (devices 0x71-0x78, units 1-8).
 *
 * ANSI C89. Portable client for FujiNet's network device over the FujiBus
 * serial protocol. See fn_net.c for implementation notes.
 */
#ifndef FN_NET_H
#define FN_NET_H

#include "fujibus.h"

/* Device byte for network unit u (1-8) is FN_NET_DEVICE_BASE + (u-1),
   i.e. units 1-8 map to devices 0x71-0x78. */
#define FN_NET_DEVICE_BASE 0x71

/* fn_net_open() access mode values (task spec, fixed values). */
#define FN_OPEN_READ         4
#define FN_OPEN_HTTP_DELETE  5
#define FN_OPEN_DIR          6
#define FN_OPEN_WRITE        8
#define FN_OPEN_APPEND       9
#define FN_OPEN_RW          12
#define FN_OPEN_HTTP_POST   13
#define FN_OPEN_HTTP_PUT    14

/* Translation mode values; only NONE is individually named by the spec.
   Values 1-4 exist on the wire but are passed through as plain integers. */
#define FN_TRANS_NONE 0

/* fn_net_set_parser() mode values (NDevice.h PARSER enum). */
#define FN_PARSER_NONE 0
#define FN_PARSER_JSON 1
#define FN_PARSER_HTML 2
#define FN_PARSER_XML  3

/*
 * Open a network unit (unit 1-8) on devicespec with the given access mode
 * and translation mode. Cmd 'O' (0x4F): sends mode as param0, trans as
 * param1 (NDevice.cpp:138-141, fujidev_open reads param_cast<...>(packet,0)
 * and param_cast<...>(packet,1) -- both are required or the firmware
 * crashes). Payload is devicespec's bytes as-is, no trailing NUL needed
 * (NDevice.cpp:143-151).
 *
 * IMPORTANT: fn_net_read() must never be called on a unit that has not been
 * successfully opened by this function (nor after a successful
 * fn_net_close() until reopened) -- the firmware's read handler
 * dereferences a null parser pointer with no server-side guard and will
 * crash the device.
 */
FnErr fn_net_open(FnCtx *ctx, fn_u8 unit, const char *devicespec,
                   fn_u8 mode, fn_u8 trans);

/*
 * Close a network unit. Cmd 'C' (0x43): no params, no payload
 * (NDevice.cpp:196-209, fujidev_close ignores the packet entirely).
 *
 * IMPORTANT: once closed, the unit is no longer open -- do not call
 * fn_net_read() on it again until it has been successfully reopened via
 * fn_net_open() (see the crash hazard documented there).
 */
FnErr fn_net_close(FnCtx *ctx, fn_u8 unit);

/*
 * Query status of a network unit. Cmd 'S' (0x53): no params. Reply is
 * exactly 4 raw bytes: avail as little-endian u16 (bytes 0-1), connected
 * (byte 2), err (byte 3) (NDevice.cpp:333-339 + NDeviceStatus struct,
 * static_assert(sizeof==4), sent raw via transaction_send so native/LE byte
 * order). Any of avail/connected/err may be NULL if not needed.
 */
FnErr fn_net_status(FnCtx *ctx, fn_u8 unit, fn_u16 *avail,
                     fn_u8 *connected, fn_u8 *err);

/*
 * Read up to len bytes from a network unit into buf. Cmd 'R' (0x52): sends
 * len as a single u16 param0 (NDevice.cpp:222-242, fujidev_read reads
 * packet.param(0) as uint16_t num_bytes). len must be 1..FN_MAX_DATA;
 * returns FN_ERR_PARAM immediately (without calling the bus) otherwise.
 *
 * CRITICAL, caller's responsibility, cannot be detected or enforced here:
 *   1. Never call this on a unit that has not been successfully opened via
 *      fn_net_open() -- the firmware's read handler dereferences a null
 *      parser pointer and crashes the device; there is no server-side
 *      guard.
 *   2. Never request more bytes than fn_net_status() last reported as
 *      available -- this is undefined/bad behavior server-side.
 * See fn_net_read_avail() for a helper that enforces rule 2 by construction.
 */
FnErr fn_net_read(FnCtx *ctx, fn_u8 unit, void *buf, fn_u16 len);

/*
 * Composite helper (not a single wire command): calls fn_net_status(), then
 * reads exactly min(avail, max, FN_MAX_DATA) bytes via fn_net_read() -- by
 * construction this never requests more than the last known avail, so it is
 * always safe to call on an open unit. If avail is 0, returns FN_OK with
 * *got==0 (if got non-NULL) and *err set from the status reply (if err
 * non-NULL), without calling fn_net_read() at all. got and err may be NULL.
 */
FnErr fn_net_read_avail(FnCtx *ctx, fn_u8 unit, void *buf, fn_u16 max,
                         fn_u16 *got, fn_u8 *err);

/*
 * Write len bytes from buf to a network unit. Cmd 'W' (0x57): sends len as
 * a single u16 param0 (NDevice.cpp:255-290, fujidev_write reads
 * packet.param(0) as uint16_t num_bytes). len must be 1..FN_MAX_DATA.
 */
FnErr fn_net_write(FnCtx *ctx, fn_u8 unit, const void *buf, fn_u16 len);

/*
 * Set the response parser mode for a network unit (FN_PARSER_*). Cmd 0xFC:
 * sends a dummy param0 (0) then mode as param1 (NDevice.cpp:530-539,
 * fujidev_set_parser reads param_cast<parserMode_t>(packet,1) -- index 1,
 * not 0 -- so a param0 placeholder must still be sent or the firmware's
 * .at(1) throws with only one param present).
 */
FnErr fn_net_set_parser(FnCtx *ctx, fn_u8 unit, fn_u8 mode);

/*
 * Ask the firmware to parse the last response using the previously-set
 * parser. Cmd 'P' (0x50): no params (NDevice.cpp:569-588, fujidev_do_parse
 * reads no packet params; it requires a parser to already be set via
 * fn_net_set_parser(), else the firmware itself returns a NAK, which
 * propagates here as FN_ERR_NAK).
 */
FnErr fn_net_parse(FnCtx *ctx, fn_u8 unit);

/*
 * Send a query string to a network unit. Cmd 'Q' (0x51): no params. The
 * RS232 transport overrides this handler to read the entire payload
 * verbatim (rs232Network.cpp:6-11, fujidev_set_query uses
 * packet.dataAsString()) rather than the generic fixed-size read other
 * transports use, so the payload is exactly query's bytes with NO trailing
 * NUL byte.
 */
FnErr fn_net_query(FnCtx *ctx, fn_u8 unit, const char *query);

/*
 * Composite helper (not a single wire command): fn_net_query(), then
 * fn_net_status(), then reads min(avail, max-1, FN_MAX_DATA) bytes into
 * out, NUL-terminating it. max must be at least 1 (room reserved for the
 * terminator); returns FN_ERR_PARAM immediately if max==0. got may be NULL.
 */
FnErr fn_net_json_query(FnCtx *ctx, fn_u8 unit, const char *query,
                         char *out, fn_u16 max, fn_u16 *got);

/*
 * Set HTTP channel mode for a network unit. Cmd 'M' (0x4D): sends a dummy
 * param0 (0) then mode as param1 (NDevice.cpp:805-821,
 * fujidev_http_set_channel_mode reads param_cast<...>(packet,1) -- same
 * index-1 pattern as fn_net_set_parser).
 */
FnErr fn_net_set_http_mode(FnCtx *ctx, fn_u8 unit, fn_u8 mode);

/*
 * Set translation mode for a network unit (FN_TRANS_* / raw values 1-4).
 * Cmd 'T' (0x54): sends a dummy param0 (0) then mode as param1
 * (NDevice.h:198-202, fujidev_set_translation reads packet.param(1)).
 */
FnErr fn_net_set_translation(FnCtx *ctx, fn_u8 unit, fn_u8 mode);

#endif /* FN_NET_H */
