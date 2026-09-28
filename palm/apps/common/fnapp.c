/* palm/apps/common/fnapp.c -- small helpers shared by the ISS Tracker,
 * Weather, and News FujiNet client apps (see fnapp.h for the full
 * contract of every function here). Compiled into each app separately
 * from its own Makefile; portable C89, Palm SDK types/calls only (no
 * libc string.h -- this codebase uses the Str-family/Mem-family calls
 * throughout).
 */
#include "fnapp.h"

/* ------------------------------------------------------------------ */
/* JSON fetch                                                          */
/* ------------------------------------------------------------------ */

FN_ERR fnapp_json_open(const char *devicespec)
{
    FN_ERR rc;

    rc = network_open(devicespec, OPEN_MODE_HTTP_GET, OPEN_TRANS_NONE);
    if (rc != FN_ERR_OK) {
        return rc;
    }

    rc = network_json_parse(devicespec);
    if (rc != FN_ERR_OK) {
        network_close(devicespec);
        return rc;
    }

    return FN_ERR_OK;
}

void fnapp_json_close(const char *devicespec)
{
    network_close(devicespec);
}

Int16 fnapp_json_query(const char *devicespec, const char *path, char *buf, UInt16 cap)
{
    Int16 n;

    if (cap == 0) {
        return 0;
    }
    buf[0] = '\0';

    /* network_json_query() (fujinet-network.h) takes NO length parameter
     * -- it writes exactly as many bytes as the field is long, straight
     * into buf, with no cap of its own. The forced NUL below only
     * guarantees termination for the expected case (the field was in
     * fact shorter than cap); it cannot undo an overflow that already
     * happened. Callers must size buf generously -- see fnapp.h. */
    n = network_json_query(devicespec, path, buf);
    if (n >= 0) {
        buf[cap - 1] = '\0';
    }
    return n;
}

/* ------------------------------------------------------------------ */
/* Plain body read-all                                                 */
/* ------------------------------------------------------------------ */

#define FNAPP_CHUNK 4096uL /* matches fujiconfig.c's InstallReadProc */

Err fnapp_http_get_all(const char *devicespec, MemHandle *outH, UInt32 *outLen, UInt32 cap)
{
    FN_ERR rc;
    MemHandle h;
    UInt8 *p;
    UInt32 total = 0;

    if (cap < 2) {
        return memErrInvalidParam;
    }

    rc = network_open(devicespec, OPEN_MODE_HTTP_GET, OPEN_TRANS_NONE);
    if (rc != FN_ERR_OK) {
        return (Err)(1000 + rc); /* distinguishable nonzero Err; callers only test zero/nonzero */
    }

    h = MemHandleNew(cap);
    if (!h) {
        network_close(devicespec);
        return memErrNotEnoughSpace;
    }
    p = (UInt8 *)MemHandleLock(h);

    for (;;) {
        UInt32 room32 = cap - 1 - total;
        UInt16 chunk;
        int16_t n;

        if (room32 == 0) {
            break;
        }
        chunk = (UInt16)((room32 > FNAPP_CHUNK) ? FNAPP_CHUNK : room32);

        n = network_read(devicespec, p + total, chunk);
        if (n <= 0) {
            /* 0 = EOF (success); negative = a read error. Either way,
             * accept whatever was read so far rather than discarding it
             * -- matches this codebase's "accept truncation" philosophy
             * (see mastodon.c's FetchQuery). */
            break;
        }
        total += (UInt32)n;
        EvtResetAutoOffTimer();
    }

    p[total] = 0;
    MemHandleUnlock(h);
    network_close(devicespec);

    *outH = h;
    *outLen = total;
    return errNone;
}

/* ------------------------------------------------------------------ */
/* Decimal-string parsing                                              */
/* ------------------------------------------------------------------ */

void fnapp_parse_fixed(const char *s, FixedPt *out)
{
    Boolean neg = false;
    UInt32 whole = 0;
    UInt16 frac = 0;
    UInt8 fracDigits = 0;

    out->neg = false;
    out->whole = 0;
    out->frac = 0;

    if (!s) {
        return;
    }

    while (*s == ' ' || *s == '\t') {
        s++;
    }
    if (*s == '-') {
        neg = true;
        s++;
    } else if (*s == '+') {
        s++;
    }

    while (*s >= '0' && *s <= '9') {
        whole = whole * 10 + (UInt32)(*s - '0');
        s++;
    }

    if (*s == '.') {
        s++;
        while (*s >= '0' && *s <= '9' && fracDigits < 2) {
            frac = (UInt16)(frac * 10 + (UInt16)(*s - '0'));
            fracDigits++;
            s++;
        }
        if (fracDigits == 1) {
            frac = (UInt16)(frac * 10);
        }
    }

    out->neg = neg;
    out->whole = whole;
    out->frac = frac;
}

void fnapp_format_fixed(char *buf, UInt16 cap, const FixedPt *v)
{
    char num[12];

    if (cap == 0) {
        return;
    }
    buf[0] = '\0';

    if (v->neg) {
        StrNCopy(buf, "-", (Int16)(cap - 1));
    }
    StrIToA(num, (Int32)v->whole);
    StrNCat(buf, num, (Int16)(cap - 1 - StrLen(buf)));
    StrNCat(buf, ".", (Int16)(cap - 1 - StrLen(buf)));
    if (v->frac < 10) {
        StrNCat(buf, "0", (Int16)(cap - 1 - StrLen(buf)));
    }
    StrIToA(num, (Int32)v->frac);
    StrNCat(buf, num, (Int16)(cap - 1 - StrLen(buf)));
}

Int32 fnapp_atol(const char *s)
{
    Int32 v = 0;
    Boolean neg = false;

    if (!s) {
        return 0;
    }
    while (*s == ' ' || *s == '\t') {
        s++;
    }
    if (*s == '-') {
        neg = true;
        s++;
    } else if (*s == '+') {
        s++;
    }
    while (*s >= '0' && *s <= '9') {
        v = v * 10 + (Int32)(*s - '0');
        s++;
    }
    return neg ? -v : v;
}

void fnapp_epoch_to_hms_utc(UInt32 unixSecs, UInt16 *hh, UInt16 *mm, UInt16 *ss)
{
    UInt32 sod = unixSecs % 86400uL;

    *hh = (UInt16)(sod / 3600uL);
    *mm = (UInt16)((sod % 3600uL) / 60uL);
    *ss = (UInt16)(sod % 60uL);
}
