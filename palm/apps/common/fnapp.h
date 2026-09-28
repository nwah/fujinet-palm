/* palm/apps/common/fnapp.h -- small helpers shared by the ISS Tracker,
 * Weather, and News FujiNet client apps (palm/apps/isstracker,
 * palm/apps/weather, palm/apps/news). Portable C89, compiled into each app
 * from its own Makefile (see e.g. palm/apps/isstracker/Makefile) -- there
 * is no common library archive, just a shared .c/.h pair.
 *
 * Header order: like every fujinet-lib caller in this tree, fujinet-*.h
 * headers must be included BEFORE PalmOS.h (fujinet-int.h pulls in the
 * compiler's stdbool.h, which must be seen before PalmOS's PalmTypes.h
 * defines its own true/false). This header pulls in fujinet-network.h and
 * PalmOS.h itself, so each app's .c should include ITS OWN fujinet-*.h
 * headers first, then this header, exactly like fujiconfig.c:
 *
 *   #include "fujinet-fuji.h"       (if the app needs fuji_* calls)
 *   #include "fujinet-network.h"
 *   #include "fujinet-palmos.h"
 *   #include <PalmOS.h>
 *   #include "fnapp.h"
 *   #include "yourapp_rsc.h"
 *
 * fnapp.h itself is safe to include stand-alone (it includes
 * fujinet-network.h/fujinet-palmos.h/PalmOS.h in the right order), but if
 * an app's .c also needs fujinet-fuji.h, that must still come first in the
 * .c, before anything else.
 *
 * Apps do NOT need to call fuji_palmos_open(): if none of them ever do,
 * the first fuji_bus_call() opens the link lazily using the "FujiNet
 * link" preference FujiConfig saves (see fujinet-palmos.h). Every app
 * MUST still call fuji_palmos_close() from PilotMain's appStopEvent path
 * (see fujiconfig.c) so HotSync etc. can use the port afterward.
 */
#ifndef FNAPP_H
#define FNAPP_H

#include "fujinet-network.h"
#include "fujinet-palmos.h"
#include <PalmOS.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ------------------------------------------------------------------ */
/* JSON fetch (mirrors the network_open/network_json_parse/
 * network_json_query/network_close sequence used by fujiconfig.c and the
 * fujinet-iss-tracker apple2/c64 originals -- see fujinet-network.h).
 * ------------------------------------------------------------------ */

/* Opens devicespec as an HTTP GET and turns on the JSON parser. On success
 * (FN_ERR_OK) the caller may issue any number of fnapp_json_query() calls
 * and MUST eventually call fnapp_json_close(). On failure, nothing needs
 * closing. */
FN_ERR fnapp_json_open(const char *devicespec);

/* Closes devicespec (network_close). Always safe to call after a
 * successful fnapp_json_open(), on every path (success or later failure). */
void fnapp_json_close(const char *devicespec);

/* Queries `path` (a JSON pointer like "/iss_position/longitude") into buf,
 * NUL-terminating within cap bytes. IMPORTANT: network_json_query() itself
 * (see fujinet-network.h) takes NO length parameter and writes exactly as
 * many bytes as the field is long, straight into `buf` -- there is no
 * device-side cap. This wrapper cannot retroactively bound that write; it
 * only truncates (with a warning-free NUL at cap-1) what it *reports* as
 * the string, by writing into buf directly and trusting the caller to
 * have sized buf generously for the field being queried (a handful of
 * bytes for a status code, ~80 for a headline, etc.) -- never pass a
 * buffer smaller than about 32 bytes even for short fields, and size up
 * for anything free-text. Returns the byte count network_json_query()
 * reported (>=0, may be 0 for an empty/missing field), or a negative
 * FN_ERR on failure. */
Int16 fnapp_json_query(const char *devicespec, const char *path, char *buf, UInt16 cap);

/* ------------------------------------------------------------------ */
/* Plain body read-all: opens devicespec as HTTP GET (no JSON parser),
 * reads the whole response into a newly allocated MemHandle (caller frees
 * it with MemHandleFree), stopping early at cap-1 bytes if the body is
 * longer (never overruns cap). *outLen is the number of bytes actually
 * read (NOT including the trailing NUL this always adds at outH[*outLen]).
 * Always closes devicespec itself, on every path. Returns errNone on
 * success; a nonzero Palm/FN error code otherwise (*outH and *outLen are
 * unspecified on failure). */
Err fnapp_http_get_all(const char *devicespec, MemHandle *outH, UInt32 *outLen, UInt32 cap);

/* ------------------------------------------------------------------ */
/* Decimal-string parsing -- this target has NO floating point, so numeric
 * JSON fields that look like "-48.2919" must be split by hand. */

typedef struct {
    Boolean neg;    /* true if the input started with '-' */
    UInt32  whole;  /* integer part, magnitude only (sign is `neg`) */
    UInt16  frac;   /* first two fractional digits, 0-99, truncated (not
                      * rounded); 0 if the input had no '.' */
} FixedPt;

/* Parses s (optionally leading/trailing whitespace, optional '+'/'-',
 * digits, optional '.' and more digits) into *out. Never reads past a
 * NUL. Malformed/empty input yields {false, 0, 0}. */
void fnapp_parse_fixed(const char *s, FixedPt *out);

/* Formats *v as "[-]W.FF" into buf (cap must be >= 14 to be safe for any
 * UInt32 whole part; 8 is enough for anything in this app's actual data,
 * e.g. lat/lon/temperatures). */
void fnapp_format_fixed(char *buf, UInt16 cap, const FixedPt *v);

/* Int32 decimal parse, stops at the first non-digit (so it also reads the
 * whole-number part of "134.4221" as a convenience, like atoi/atol would).
 * Handles an optional leading '-'. This is this app family's replacement
 * for atol() -- avoid plain int/StrAToI's 16-bit range for anything that
 * can exceed +/-32767 (Unix epoch seconds in particular). */
Int32 fnapp_atol(const char *s);

/* Unix epoch seconds (e.g. from fnapp_atol() on a "/timestamp" JSON field)
 * -> UTC time-of-day, by plain integer math (mod/div on the Unix count
 * directly -- deliberately NOT via TimSecondsToDateTime, which uses
 * Palm's 1904 epoch and would need the environment's documented
 * 2082844800 offset; going through it is unnecessary when only
 * time-of-day, not a date, is wanted). */
void fnapp_epoch_to_hms_utc(UInt32 unixSecs, UInt16 *hh, UInt16 *mm, UInt16 *ss);

#ifdef __cplusplus
}
#endif

#endif /* FNAPP_H */
