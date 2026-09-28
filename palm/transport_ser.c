/* palm/transport_ser.c
 *
 * See transport_ser.h for the design. Nothing here touches globals: every
 * callback receives its FnSerPort via the FnTransport ctx pointer, so this
 * code can later be relocated into a shared library without change.
 */
#include "transport_ser.h"
#include <SerialMgrOld.h>

/* HsExtKeyboardEnable(): Handspring AN-09. In a serial (non-USB) cradle, the
 * keyboard daemon holds "BuiltIn SerLib"; calling this with `enable` false
 * before SerOpen releases it (no need to re-enable afterwards).
 *
 * The PalmOne SDK's 68K/System/HsExt.h declares the sibling Hs* trap calls
 * with this SYS_SEL_TRAP(sysTrapHsSelector, hsSelXxx) pattern -- an ordinary
 * numbered system trap (sysTrapHsSelector, aka sysTrapOEMDispatch, 0xA349)
 * whose sub-function is chosen by a 16-bit selector word the caller pushes
 * on the stack first -- but it doesn't declare this one selector, so the
 * macro (from HsExtTraps.h/HsExt.h, gcc >= 2.95 form) and the two constants
 * (from HsExtTraps.h) are reproduced locally instead of including those
 * headers. sysDispatchTrapNum (15, the fixed "trap #15" that dispatches
 * every numbered Palm OS system trap by a following inline trap-number
 * word) comes from PalmTypes.h via <PalmOS.h>, already included above. */
#ifndef _Str
#define _Str(X) #X
#endif
#define HSEXT_SYS_TRAP_HS_SELECTOR      0xA349  /* sysTrapHsSelector == sysTrapOEMDispatch */
#define HSEXT_SEL_EXT_KEYBOARD_ENABLE   0x5     /* hsSelExtKeyboardEnable */

/* Two macro levels, exactly like HsExt.h's own
 * _HS_OS_CALL_WITH_UNPOPPED_16BIT_SELECTOR/SYS_SEL_TRAP pair: `table`
 * (sysDispatchTrapNum) must be macro-expanded to "15" as an ORDINARY
 * argument of HSEXT_CALLSEQ (whose own body only hands it to _Str(), never
 * stringizing it directly) before _Str() stringizes it -- collapsing this
 * into one macro that calls _Str(sysDispatchTrapNum) directly would
 * stringize the literal identifier instead, since _Str's own parameter is
 * `#`-prefixed and so never pre-expands its argument. */
#define HSEXT_CALLSEQ(table, vector, selector) \
    __attribute__ ((__callseq__ ( \
        "move.w #" _Str(selector) ",-(%%sp); " \
        "trap #" _Str(table) "; dc.w " _Str(vector))))

#define HSEXT_SYS_SEL_TRAP(trapNum, selector) \
    HSEXT_CALLSEQ(sysDispatchTrapNum, trapNum, selector)

Err HsExtKeyboardEnable(Boolean enable)
    HSEXT_SYS_SEL_TRAP(HSEXT_SYS_TRAP_HS_SELECTOR, HSEXT_SEL_EXT_KEYBOARD_ENABLE);

#define FN_SER_RX_BUF_SIZE 4096uL

/* Known shared-library names and, where verified (Handspring AN-09), the
 * creator code to try with SysLibLoad('libr', creator, &refNum) if
 * SysLibFind fails. A creator of 0 means "no verified fallback creator";
 * for those we rely on SysLibFind alone rather than guess a code. */
static const struct {
    const char *name;
    UInt32      creator;
} kSerLibs[] = {
    { "USB Library",    'HsUs' },
    { "BuiltIn SerLib",  0     }, /* creator unknown; SysLibFind by name only */
    { "Serial Library",  0     }  /* creator unknown; SysLibFind by name only */
};

#define kNumSerLibs (sizeof(kSerLibs) / sizeof(kSerLibs[0]))

/* Converts a millisecond timeout to Palm OS system ticks, rounding up and
 * clamping to a minimum of 1 tick so a caller-specified nonzero timeout
 * never collapses to "wait forever" (timeout 0 has that meaning to some
 * Ser* calls). Correct for timeouts up to several hours at typical tick
 * rates; this transport is only ever asked for timeouts of a few seconds. */
static Int32 SerMsToTicks(fn_u32 timeoutMs)
{
    UInt32 tps;
    UInt32 ticks;

    if (timeoutMs == 0)
        return 0;

    tps = SysTicksPerSecond();
    ticks = (((UInt32)timeoutMs * tps) + 999uL) / 1000uL;
    if (ticks < 1uL)
        ticks = 1uL;

    return (Int32)ticks;
}

/* Returns true and clears the line error if err is the old Serial Manager's
 * line-error indication, per fn_ser_open()/callback contract: "on serLineErr
 * errors call SerClearErr and return -1". */
static Boolean SerHandleLineErr(FnSerPort *p, Err err)
{
    if (err == serErrLineErr) {
        SerClearErr(p->refNum);
        return true;
    }
    return false;
}

Err fn_ser_open(FnSerPort *p, const char *libName, UInt32 baud)
{
    Err     err;
    UInt16  refNum;
    UInt32  creator = 0;
    UInt16  i;
    SerSettingsType settings;

    if (p == NULL || libName == NULL)
        return sysErrParamErr;

    MemSet(p, sizeof(FnSerPort), 0);
    p->baud = baud;

    for (i = 0; i < kNumSerLibs; i++) {
        if (StrCompare(libName, kSerLibs[i].name) == 0) {
            creator = kSerLibs[i].creator;
            break;
        }
    }

    /* Handspring AN-09: in a serial cradle, "BuiltIn SerLib" (and anything
     * redirected onto it, e.g. "Serial Library") is held by the keyboard
     * daemon until this is called with false. Not needed for "USB Library"
     * (a different bridge, not backed by the UART the daemon watches), and
     * harmless to skip if this ROM has no Handspring extensions at all. No
     * need to re-enable after close (see HsExtKeyboardEnable comment
     * above). */
    if (StrCompare(libName, "USB Library") != 0) {
        UInt32 hsExtVersion;

        if (FtrGet('hsEx', 0, &hsExtVersion) == errNone)
            HsExtKeyboardEnable(false);
    }

    err = SysLibFind(libName, &refNum);
    if (err != errNone && creator != 0)
        err = SysLibLoad(sysFileTLibrary, creator, &refNum);
    if (err != errNone) {
        p->lastErr = err;
        return err;
    }

    /* `port` is meaningless once we've already picked a specific library by
     * name; pass 0. */
    err = SerOpen(refNum, 0, baud);
    if (err != errNone) {
        p->lastErr = err;
        return err;
    }

    p->refNum = refNum;
    p->open = true;
    StrNCopy(p->libName, libName, sizeof(p->libName) - 1);
    p->libName[sizeof(p->libName) - 1] = '\0';

    /* 115200 8N1, no flow control (no RTS/CTS). */
    err = SerGetSettings(refNum, &settings);
    if (err == errNone) {
        settings.baudRate = baud;
        settings.flags = serSettingsFlagBitsPerChar8 | serSettingsFlagStopBits1;
        err = SerSetSettings(refNum, &settings);
    }
    if (err != errNone) {
        p->lastErr = err;
        SerClose(refNum);
        p->open = false;
        p->refNum = 0;
        return err;
    }

    /* Bigger receive buffer; not fatal if it can't be allocated -- the port
     * still works with the library's default buffer. */
    p->rxBuf = MemPtrNew(FN_SER_RX_BUF_SIZE);
    if (p->rxBuf != NULL) {
        err = SerSetReceiveBuffer(refNum, p->rxBuf, (UInt16)FN_SER_RX_BUF_SIZE);
        if (err != errNone) {
            MemPtrFree(p->rxBuf);
            p->rxBuf = NULL;
        }
    }

    return errNone;
}

void fn_ser_close(FnSerPort *p)
{
    if (p == NULL)
        return;

    if (p->open) {
        if (p->rxBuf != NULL) {
            /* Must restore the default receive buffer before SerClose and
             * before freeing the memory we handed to SerSetReceiveBuffer. */
            SerSetReceiveBuffer(p->refNum, NULL, 0);
        }
        SerClose(p->refNum);
        p->open = false;
        p->refNum = 0;
    }

    if (p->rxBuf != NULL) {
        MemPtrFree(p->rxBuf);
        p->rxBuf = NULL;
    }
}

static fn_i16 SerTransportSend(void *ctx, const fn_u8 *buf, fn_u16 len)
{
    FnSerPort *p = (FnSerPort *)ctx;
    UInt32 totalSent = 0;
    Err err;

    if (p == NULL || !p->open)
        return -1;

    while (totalSent < (UInt32)len) {
        UInt32 n = SerSend(p->refNum, (void *)(buf + totalSent),
                            (UInt32)len - totalSent, &err);
        if (err != errNone) {
            p->lastErr = err;
            SerHandleLineErr(p, err);
            return -1;
        }
        if (n == 0) {
            /* No forward progress without an error reported; bail out
             * rather than spin forever. */
            return -1;
        }
        totalSent += n;
    }

    return 0;
}

static fn_i16 SerTransportRecv(void *ctx, fn_u8 *buf, fn_u16 max, fn_u32 timeoutMs)
{
    FnSerPort *p = (FnSerPort *)ctx;
    Err    err;
    UInt32 avail = 0;
    UInt32 want;
    UInt32 got;
    Int32  ticks;

    if (p == NULL || !p->open || buf == NULL || max == 0)
        return -1;

    ticks = SerMsToTicks(timeoutMs);

    err = SerReceiveCheck(p->refNum, &avail);
    if (err != errNone) {
        p->lastErr = err;
        SerHandleLineErr(p, err);
        return -1;
    }

    if (avail == 0) {
        /* Nothing queued yet; wait for at least one byte or the timeout. */
        err = SerReceiveWait(p->refNum, 1, ticks);
        if (err == serErrTimeOut)
            return 0;
        if (err != errNone) {
            p->lastErr = err;
            SerHandleLineErr(p, err);
            return -1;
        }

        err = SerReceiveCheck(p->refNum, &avail);
        if (err != errNone) {
            p->lastErr = err;
            SerHandleLineErr(p, err);
            return -1;
        }
        if (avail == 0)
            return 0; /* woken with nothing to show for it; treat as timeout */
    }

    want = (avail < (UInt32)max) ? avail : (UInt32)max;
    if (want > 32767uL)
        want = 32767uL; /* keep the byte count representable in fn_i16 */

    got = SerReceive(p->refNum, buf, want, 0, &err);
    if (err == serErrTimeOut) {
        /* Bytes were already known to be queued, so a partial read here
         * still counts as data; only report a timeout if nothing came. */
        return (got > 0) ? (fn_i16)got : 0;
    }
    if (err != errNone) {
        p->lastErr = err;
        SerHandleLineErr(p, err);
        return -1;
    }

    return (fn_i16)got;
}

static void SerTransportFlushRx(void *ctx)
{
    FnSerPort *p = (FnSerPort *)ctx;

    if (p == NULL || !p->open)
        return;

    SerReceiveFlush(p->refNum, 0);
}

void fn_ser_transport(FnSerPort *p, FnTransport *t)
{
    if (t == NULL)
        return;

    t->ctx = p;
    t->send = SerTransportSend;
    t->recv = SerTransportRecv;
    t->flush_rx = SerTransportFlushRx;
}
