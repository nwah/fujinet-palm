/* palm/apps/fnlink/fnlink.c
 *
 * FnLink -- serial/USB link test app for the FujiNet Palm OS transport.
 * Picks one of the three shared serial libraries a Handspring Visor
 * exposes, opens it through fujinet-lib's own serial transport
 * (bus/palmos/transport_ser, the one every fujinet-lib app uses) and runs
 * an echo test against a host-side bridge (tools/visorbridge.js --echo).
 * The Fuji button talks to FujiNet through fujinet-lib's public API.
 *
 * Targets Palm OS 3.1 / DragonBall EZ (Handspring Visor Deluxe). Built with
 * -palmos3.5 against the 3.5 SDK's <PalmOS.h> plus an explicit
 * <SerialMgrOld.h> include (see ../../transport_ser.c) to get the old
 * Serial Manager API that is the only one this device's ROM has; the app
 * itself never calls Ser- or Srm-prefixed functions directly, only through
 * the FnTransport vtable, so it doesn't care which serial manager is
 * underneath.
 */
/* fujinet headers before PalmOS.h, see ../common/fnapp.h */
#include "fujinet-fuji.h"
#include "fujinet-palmos.h"
#include <PalmOS.h>
#include "transport_ser.h" /* fujinet-lib bus/palmos: the raw serial transport */
#include "fnlink_rsc.h"

#define FN_ECHO_TIMEOUT_MS 3000uL
#define FN_ECHO_SIZE       256

#define LOG_LINES   8
#define LOG_LINE_LEN 48
#define LOG_X   4
#define LOG_Y   46
#define LOG_W   152
#define LOG_H   112

static FnSerPort  gPort;
static FnTransport gTransport;
static UInt16     gSelectedLib = LibUsbButton;

static char gLog[LOG_LINES][LOG_LINE_LEN];
static UInt8 gLogUsed = 0;

typedef enum {
    ECHO_OK,
    ECHO_SEND_ERR,
    ECHO_RECV_ERR,
    ECHO_TIMEOUT,
    ECHO_MISMATCH
} EchoResult;

/* --------------------------------------------------------------------- */
/* Small helpers                                                         */
/* --------------------------------------------------------------------- */

static UInt32 TicksToMs(UInt32 ticks)
{
    UInt32 tps = SysTicksPerSecond();
    if (tps == 0)
        return 0;
    return (ticks * 1000uL) / tps;
}

static const char *LibNameForId(UInt16 id)
{
    switch (id) {
    case LibUartButton:   return "BuiltIn SerLib";
    case LibSerialButton: return "Serial Library";
    case LibUsbButton:
    default:              return "USB Library";
    }
}

static const char *LibShortName(UInt16 id)
{
    switch (id) {
    case LibUartButton:   return "UART";
    case LibSerialButton: return "SER";
    case LibUsbButton:
    default:              return "USB";
    }
}

/* --------------------------------------------------------------------- */
/* Log area: a fixed-size ring of short lines, redrawn with WinDrawChars. */
/* No Field/Gadget object -- this keeps the log free of MemHandle/scroll  */
/* edge cases on an old ROM.                                             */
/* --------------------------------------------------------------------- */

static void LogDraw(void)
{
    RectangleType r;
    UInt8 i;
    FontID oldFont;
    Int16 lineH;

    r.topLeft.x = LOG_X;
    r.topLeft.y = LOG_Y;
    r.extent.x = LOG_W;
    r.extent.y = LOG_H;
    WinEraseRectangle(&r, 0);

    oldFont = FntSetFont(stdFont);
    lineH = FntLineHeight();

    for (i = 0; i < gLogUsed; i++)
        WinDrawChars(gLog[i], StrLen(gLog[i]), LOG_X, LOG_Y + i * lineH);

    FntSetFont(oldFont);
}

static void LogAppend(const char *msg)
{
    UInt8 i;

    if (gLogUsed < LOG_LINES) {
        StrNCopy(gLog[gLogUsed], msg, LOG_LINE_LEN - 1);
        gLog[gLogUsed][LOG_LINE_LEN - 1] = '\0';
        gLogUsed++;
    } else {
        for (i = 1; i < LOG_LINES; i++)
            StrCopy(gLog[i - 1], gLog[i]);
        StrNCopy(gLog[LOG_LINES - 1], msg, LOG_LINE_LEN - 1);
        gLog[LOG_LINES - 1][LOG_LINE_LEN - 1] = '\0';
    }

    LogDraw();
}

/* --------------------------------------------------------------------- */
/* Serial link actions                                                   */
/* --------------------------------------------------------------------- */

static void DoOpen(void)
{
    const char *libName;
    const char *shortName;
    Err err;
    char msg[LOG_LINE_LEN];
    char num[12];

    if (gPort.open) {
        LogAppend("OPEN: already open");
        return;
    }

    libName = LibNameForId(gSelectedLib);
    shortName = LibShortName(gSelectedLib);

    err = fn_ser_open(&gPort, libName, 115200uL);

    if (gPort.open) {
        fn_ser_transport(&gPort, &gTransport);

        StrCopy(msg, "OPEN OK ");
        StrCat(msg, shortName);
        StrCat(msg, " ref=");
        StrIToA(num, (Int32)gPort.refNum);
        StrCat(msg, num);
    } else {
        char hex[9];

        StrCopy(msg, "OPEN FAIL ");
        StrCat(msg, shortName);
        StrCat(msg, " err=");
        StrIToH(hex, (UInt32)(UInt16)err);
        StrCat(msg, hex + 4); /* low 4 hex digits are enough for a 16-bit Err */
    }

    LogAppend(msg);
}

static void DoClose(void)
{
    if (!gPort.open) {
        LogAppend("CLOSE: not open");
        return;
    }

    fn_ser_close(&gPort);
    LogAppend("CLOSE ok");
}

/* Runs one 256-byte send/receive round trip. Returns the outcome; *outGot
 * and *outMismatchIdx are only meaningful for ECHO_TIMEOUT/ECHO_MISMATCH,
 * *outElapsedMs is always set. */
static EchoResult EchoOnePass(UInt16 *outGot, UInt16 *outMismatchIdx, UInt32 *outElapsedMs)
{
    static UInt8 txbuf[FN_ECHO_SIZE];
    static UInt8 rxbuf[FN_ECHO_SIZE];
    UInt16 i;
    UInt32 startTicks;
    UInt16 gotTotal = 0;
    fn_i16 rc;

    for (i = 0; i < FN_ECHO_SIZE; i++)
        txbuf[i] = (UInt8)i;

    gTransport.flush_rx(gTransport.ctx);

    startTicks = TimGetTicks();

    rc = gTransport.send(gTransport.ctx, txbuf, FN_ECHO_SIZE);
    if (rc < 0) {
        *outGot = 0;
        *outElapsedMs = TicksToMs(TimGetTicks() - startTicks);
        return ECHO_SEND_ERR;
    }

    while (gotTotal < FN_ECHO_SIZE) {
        UInt32 elapsedMs = TicksToMs(TimGetTicks() - startTicks);
        UInt32 remainMs;

        if (elapsedMs >= FN_ECHO_TIMEOUT_MS)
            break;
        remainMs = FN_ECHO_TIMEOUT_MS - elapsedMs;

        rc = gTransport.recv(gTransport.ctx, rxbuf + gotTotal,
                              (fn_u16)(FN_ECHO_SIZE - gotTotal), remainMs);
        if (rc < 0) {
            *outGot = gotTotal;
            *outElapsedMs = TicksToMs(TimGetTicks() - startTicks);
            return ECHO_RECV_ERR;
        }
        if (rc == 0)
            break; /* timeout */

        gotTotal += (UInt16)rc;
        EvtResetAutoOffTimer();
    }

    *outGot = gotTotal;
    *outElapsedMs = TicksToMs(TimGetTicks() - startTicks);

    if (gotTotal < FN_ECHO_SIZE)
        return ECHO_TIMEOUT;

    for (i = 0; i < FN_ECHO_SIZE; i++) {
        if (rxbuf[i] != txbuf[i]) {
            *outMismatchIdx = i;
            return ECHO_MISMATCH;
        }
    }

    return ECHO_OK;
}

static void AppendEchoFailure(char *msg, const char *prefix, EchoResult r,
                               UInt16 got, UInt16 mismatchIdx)
{
    char num[12];

    StrCopy(msg, prefix);

    switch (r) {
    case ECHO_SEND_ERR:
        StrCat(msg, " send err=");
        {
            char hex[9];
            StrIToH(hex, (UInt32)(UInt16)gPort.lastErr);
            StrCat(msg, hex + 4);
        }
        break;
    case ECHO_RECV_ERR:
        StrCat(msg, " recv err=");
        {
            char hex[9];
            StrIToH(hex, (UInt32)(UInt16)gPort.lastErr);
            StrCat(msg, hex + 4);
        }
        break;
    case ECHO_TIMEOUT:
        StrIToA(num, (Int32)got);
        StrCat(msg, " ");
        StrCat(msg, num);
        StrCat(msg, "/256 timeout");
        break;
    case ECHO_MISMATCH:
        StrCat(msg, " mismatch @");
        StrIToA(num, (Int32)mismatchIdx);
        StrCat(msg, num);
        break;
    default:
        break;
    }
}

static void DoEcho(void)
{
    UInt16 got = 0, mismatchIdx = 0;
    UInt32 elapsedMs = 0;
    EchoResult r;
    char msg[LOG_LINE_LEN];

    if (!gPort.open) {
        LogAppend("ECHO: not open");
        return;
    }

    r = EchoOnePass(&got, &mismatchIdx, &elapsedMs);

    if (r == ECHO_OK) {
        char num[12];
        StrCopy(msg, "echo 256/256 OK ");
        StrIToA(num, (Int32)elapsedMs);
        StrCat(msg, num);
        StrCat(msg, "ms");
    } else {
        AppendEchoFailure(msg, "echo", r, got, mismatchIdx);
    }

    LogAppend(msg);
}

/* FujiBus smoke test through fujinet-lib: open the link on the selected
 * library, read the adapter config, close. The raw echo port is closed
 * first -- both would want the same serial library. */
static void LogLinkErr(const char *prefix)
{
    char msg[LOG_LINE_LEN];
    char num[12];

    StrCopy(msg, prefix);
    StrCat(msg, " err=");
    StrIToH(num, (UInt32)fuji_palmos_last_error());
    StrCat(msg, num + 4);
    LogAppend(msg);
}

static void DoFuji(void)
{
    static AdapterConfigExtended cfg;
    char msg[LOG_LINE_LEN];
    char num[12];
    UInt32 t0;

    if (gPort.open) {
        fn_ser_close(&gPort);
        LogAppend("(echo port closed)");
    }

    if (!fuji_palmos_open(LibNameForId(gSelectedLib), 115200uL)) {
        LogLinkErr("LINK");
        return;
    }

    t0 = TimGetTicks();
    if (!fuji_get_adapter_config_extended(&cfg)) {
        LogLinkErr("CONFIG");
        fuji_palmos_close();
        return;
    }
    StrCopy(msg, "CONFIG ok ");
    StrIToA(num, (Int32)TicksToMs(TimGetTicks() - t0));
    StrCat(msg, num);
    StrCat(msg, "ms");
    LogAppend(msg);

    StrCopy(msg, "SSID ");
    StrNCat(msg, cfg.ssid, LOG_LINE_LEN);
    LogAppend(msg);
    StrCopy(msg, "HOST ");
    StrNCat(msg, cfg.hostname, LOG_LINE_LEN);
    LogAppend(msg);
    StrCopy(msg, "FN ");
    StrNCat(msg, cfg.fn_version, LOG_LINE_LEN);
    LogAppend(msg);

    fuji_palmos_close();
}

static void DoX20(void)
{
    UInt16 pass;
    UInt16 okCount = 0;
    UInt32 totalBytes = 0;
    UInt32 totalElapsedMs = 0;
    char msg[LOG_LINE_LEN];

    if (!gPort.open) {
        LogAppend("X20: not open");
        return;
    }

    for (pass = 0; pass < 20; pass++) {
        UInt16 got = 0, mismatchIdx = 0;
        UInt32 elapsedMs = 0;
        EchoResult r = EchoOnePass(&got, &mismatchIdx, &elapsedMs);

        totalElapsedMs += elapsedMs;

        if (r == ECHO_OK) {
            okCount++;
            totalBytes += FN_ECHO_SIZE;
        } else {
            char prefix[16];
            char num[12];
            StrCopy(prefix, "x20#");
            StrIToA(num, (Int32)(pass + 1));
            StrCat(prefix, num);
            AppendEchoFailure(msg, prefix, r, got, mismatchIdx);
            LogAppend(msg);
        }
    }

    {
        UInt32 kbTimesTen = 0;
        char num[12];

        if (totalElapsedMs > 0) {
            UInt32 bytesPerSec = (totalBytes * 1000uL) / totalElapsedMs;
            kbTimesTen = (bytesPerSec * 10uL) / 1024uL;
        }

        StrCopy(msg, "x20 ");
        StrIToA(num, (Int32)okCount);
        StrCat(msg, num);
        StrCat(msg, "/20 ");
        StrIToA(num, (Int32)totalBytes);
        StrCat(msg, num);
        StrCat(msg, "B ");
        StrIToA(num, (Int32)totalElapsedMs);
        StrCat(msg, num);
        StrCat(msg, "ms ");
        StrIToA(num, (Int32)(kbTimesTen / 10));
        StrCat(msg, num);
        StrCat(msg, ".");
        StrIToA(num, (Int32)(kbTimesTen % 10));
        StrCat(msg, num);
        StrCat(msg, "kB/s");
    }

    LogAppend(msg);
}

/* --------------------------------------------------------------------- */
/* Form / event handling                                                 */
/* --------------------------------------------------------------------- */

static void SelectLibButton(FormType *frm, UInt16 selectedId)
{
    static const UInt16 ids[3] = { LibUsbButton, LibUartButton, LibSerialButton };
    UInt16 i;

    for (i = 0; i < 3; i++) {
        UInt16 idx = FrmGetObjectIndex(frm, ids[i]);
        ControlType *ctl = (ControlType *)FrmGetObjectPtr(frm, idx);
        CtlSetValue(ctl, (ids[i] == selectedId) ? 1 : 0);
    }
}

static Boolean MainFormHandleEvent(EventType *e)
{
    switch (e->eType) {
    case frmOpenEvent: {
        FormType *frm = FrmGetActiveForm();
        FrmDrawForm(frm);
        SelectLibButton(frm, gSelectedLib);
        LogDraw();
        return true;
    }

    case frmUpdateEvent:
        FrmDrawForm(FrmGetActiveForm());
        LogDraw();
        return true;

    case ctlSelectEvent:
        switch (e->data.ctlSelect.controlID) {
        case LibUsbButton:
        case LibUartButton:
        case LibSerialButton:
            gSelectedLib = e->data.ctlSelect.controlID;
            return true;
        case OpenButton:
            DoOpen();
            return true;
        case CloseButton:
            DoClose();
            return true;
        case EchoButton:
            DoEcho();
            return true;
        case X20Button:
            DoX20();
            return true;
        case FujiButton:
            DoFuji();
            return true;
        default:
            return false;
        }

    default:
        return false;
    }
}

static Boolean AppHandleEvent(EventType *e)
{
    if (e->eType == frmLoadEvent) {
        FormType *frm = FrmInitForm(e->data.frmLoad.formID);
        FrmSetActiveForm(frm);
        FrmSetEventHandler(frm, MainFormHandleEvent);
        return true;
    }
    return false;
}

UInt32 PilotMain(UInt16 cmd, MemPtr cmdPBP, UInt16 launchFlags)
{
    EventType e;
    UInt16 err;

    if (cmd != sysAppLaunchCmdNormalLaunch)
        return 0;

    FrmGotoForm(MainForm);
    do {
        EvtGetEvent(&e, evtWaitForever);
        if (SysHandleEvent(&e)) continue;
        if (MenuHandleEvent(0, &e, &err)) continue;
        if (AppHandleEvent(&e)) continue;
        FrmDispatchEvent(&e);
    } while (e.eType != appStopEvent);

    if (gPort.open)
        fn_ser_close(&gPort);

    FrmCloseAllForms();
    return 0;
}
