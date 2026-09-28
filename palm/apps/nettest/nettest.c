/* palm/apps/nettest/nettest.c
 *
 * Test harness for the FujiNet NetLib shim (palm/netshim, fnnetlib.prc).
 * Install/Remove/Status drive the shim's own control calls; DNS and HTTP
 * use ONLY the standard NetLib API, exactly as an existing third-party
 * network app would, so they exercise whatever is behind "Net.lib" (the
 * shim when installed, the ROM PPP stack otherwise).
 */
#include <PalmOS.h>
#include <NetMgr.h>
#include "fnnetlib.h"
#include "nettest_rsc.h"

#define LOG_MAX   2000   /* trim the log from the front past this many chars */
#define LOG_KEEP  1400

#define HTTP_HOST "example.com"

static char gRecvBuf[512];

static void *GetObj(UInt16 id)
{
    FormType *frm = FrmGetActiveForm();
    return FrmGetObjectPtr(frm, FrmGetObjectIndex(frm, id));
}

static void UpdateScroll(void)
{
    FieldType *fld = GetObj(LogField);
    ScrollBarType *bar = GetObj(LogScroll);
    UInt16 pos, textH, fieldH, max;

    FldGetScrollValues(fld, &pos, &textH, &fieldH);
    max = (textH > fieldH) ? (UInt16)(textH - fieldH) : 0;
    SclSetScrollBar(bar, pos, 0, max, (fieldH > 1) ? (UInt16)(fieldH - 1) : 1);
}

static void ScrollLines(Int16 lines)
{
    FieldType *fld = GetObj(LogField);

    if (lines < 0)
        FldScrollField(fld, (UInt16)-lines, winUp);
    else if (lines > 0)
        FldScrollField(fld, (UInt16)lines, winDown);
    UpdateScroll();
}

static void Log(const char *s)
{
    FieldType *fld = GetObj(LogField);
    MemHandle h = FldGetTextHandle(fld);
    char *p;
    UInt16 len, add = (UInt16)(StrLen(s) + 1);
    UInt16 pos, textH, fieldH;

    FldSetTextHandle(fld, NULL);
    if (h == NULL) {
        h = MemHandleNew(1);
        if (h == NULL)
            return;
        p = MemHandleLock(h);
        p[0] = '\0';
        MemHandleUnlock(h);
    }

    p = MemHandleLock(h);
    len = (UInt16)StrLen(p);
    if (len + add > LOG_MAX && len > LOG_KEEP) {
        UInt16 cut = (UInt16)(len - LOG_KEEP);
        while (cut < len && p[cut - 1] != '\n')
            cut++;
        MemMove(p, p + cut, len - cut + 1);
        len = (UInt16)(len - cut);
    }
    MemHandleUnlock(h);

    if (MemHandleResize(h, len + add + 1) == errNone) {
        p = MemHandleLock(h);
        StrCat(p, s);
        StrCat(p, "\n");
        MemHandleUnlock(h);
    }

    FldSetTextHandle(fld, h);
    FldGetScrollValues(fld, &pos, &textH, &fieldH);
    if (textH > fieldH && pos < textH - fieldH)
        FldScrollField(fld, (UInt16)(textH - fieldH - pos), winDown);
    FldDrawField(fld);
    UpdateScroll();
}

static void LogErr(const char *what, Err err)
{
    char buf[64];

    if (err == 0)
        StrPrintF(buf, "%s: ok", what);
    else
        StrPrintF(buf, "%s: err 0x%x", what, err);
    Log(buf);
}

static void ClearLog(void)
{
    FieldType *fld = GetObj(LogField);
    MemHandle h = FldGetTextHandle(fld);

    FldSetTextHandle(fld, NULL);
    if (h != NULL)
        MemHandleFree(h);
    FldDrawField(fld);
    UpdateScroll();
}

/* ---- shim control ------------------------------------------------------ */

static Boolean FindShim(UInt16 *refP, Boolean load)
{
    Err err = SysLibFind(fnNLLibName, refP);

    if (err != errNone && load) {
        err = SysLibLoad(fnNLDbType, fnNLCreator, refP);
        LogErr("SysLibLoad FnNL", err);
    }
    return (Boolean)(err == errNone);
}

static void DoInstall(void)
{
    UInt16 ref;

    if (!FindShim(&ref, true)) {
        Log("FujiNet NetLib not installed?");
        return;
    }
    LogErr("FnNLInstall", FnNLInstall(ref));
}

static void DoRemove(void)
{
    UInt16 ref;

    if (!FindShim(&ref, false)) {
        Log("shim not loaded");
        return;
    }
    LogErr("FnNLRemove", FnNLRemove(ref));
}

static void DoStatus(void)
{
    UInt16 ref;
    FnNLStatusType st;
    char buf[80];
    Err err;

    if (!FindShim(&ref, false)) {
        Log("shim not loaded");
        return;
    }
    err = FnNLGetStatus(ref, &st);
    if (err != errNone) {
        LogErr("FnNLGetStatus", err);
        return;
    }
    StrPrintF(buf, "inst=%d open=%d link=%d socks=%d hosts=%d",
              st.installed, st.openCount, st.linkOpen, st.socketsInUse, st.fakeHostCount);
    Log(buf);
    StrPrintF(buf, "lib=\"%s\" lastErr=0x%x", st.linkLibName, st.lastErr);
    Log(buf);
}

/* ---- standard NetLib client, as a third-party app would do it --------- */

static Boolean OpenNet(UInt16 *netRefP)
{
    UInt16 ifErrs = 0;
    Err err;
    char buf[48];

    err = SysLibFind("Net.lib", netRefP);
    if (err != errNone) {
        LogErr("SysLibFind Net.lib", err);
        return false;
    }
    err = NetLibOpen(*netRefP, &ifErrs);
    if (err == netErrAlreadyOpen)
        err = errNone;
    StrPrintF(buf, "NetLibOpen: err 0x%x ifErrs 0x%x", err, ifErrs);
    Log(buf);
    if (err != errNone)
        return false;
    if (ifErrs != 0) {
        NetLibClose(*netRefP, true);
        return false;
    }
    return true;
}

static Boolean Resolve(UInt16 netRef, const char *name, NetIPAddr *addrP)
{
    NetHostInfoBufType *hb;
    NetHostInfoPtr hi;
    Err err = 0;
    char buf[64], ip[16];

    hb = MemPtrNew(sizeof(NetHostInfoBufType));
    if (hb == NULL) {
        Log("out of memory");
        return false;
    }
    hi = NetLibGetHostByName(netRef, (Char *)name, hb, 10 * SysTicksPerSecond(), &err);
    if (hi == NULL || hi->addrListP == NULL || hi->addrListP[0] == NULL) {
        LogErr("GetHostByName", err ? err : netErrInternal);
        MemPtrFree(hb);
        return false;
    }
    *addrP = *(NetIPAddr *)hi->addrListP[0];
    NetLibAddrINToA(netRef, *addrP, ip);
    StrPrintF(buf, "%s -> %s", hi->nameP, ip);
    Log(buf);
    MemPtrFree(hb);
    return true;
}

static void DoDns(void)
{
    UInt16 netRef;
    NetIPAddr addr;

    if (!OpenNet(&netRef))
        return;
    Resolve(netRef, HTTP_HOST, &addr);
    LogErr("NetLibClose", NetLibClose(netRef, false));
}

static void DoHttp(void)
{
    static const char req[] = "GET / HTTP/1.0\r\nHost: " HTTP_HOST "\r\n\r\n";
    UInt16 netRef;
    NetIPAddr addr;
    NetSocketRef sock;
    NetSocketAddrINType sa;
    NetFDSetType rd;
    Err err = 0;
    Int16 n, total = 0;
    Boolean first = true;
    char buf[64];

    if (!OpenNet(&netRef))
        return;
    if (!Resolve(netRef, HTTP_HOST, &addr))
        goto close_lib;

    sock = NetLibSocketOpen(netRef, netSocketAddrINET, netSocketTypeStream,
                            netSocketProtoIPTCP, 10 * SysTicksPerSecond(), &err);
    StrPrintF(buf, "SocketOpen: %d err 0x%x", sock, err);
    Log(buf);
    if (sock < 0)
        goto close_lib;

    MemSet(&sa, sizeof(sa), 0);
    sa.family = netSocketAddrINET;
    sa.port = 80;       /* network order == host order on m68k */
    sa.addr = addr;
    n = NetLibSocketConnect(netRef, sock, (NetSocketAddrType *)&sa, sizeof(sa),
                            15 * SysTicksPerSecond(), &err);
    StrPrintF(buf, "Connect: %d err 0x%x", n, err);
    Log(buf);
    if (n < 0)
        goto close_sock;

    n = NetLibSend(netRef, sock, (void *)req, (UInt16)StrLen(req), 0, NULL, 0,
                   10 * SysTicksPerSecond(), &err);
    StrPrintF(buf, "Send: %d err 0x%x", n, err);
    Log(buf);
    if (n < 0)
        goto close_sock;

    netFDZero(&rd);
    netFDSet(sock, &rd);
    netFDSet(sysFileDescStdIn, &rd);
    n = NetLibSelect(netRef, (UInt16)(sock + 1), &rd, NULL, NULL,
                     5 * SysTicksPerSecond(), &err);
    StrPrintF(buf, "Select: %d sock=%d stdin=%d err 0x%x", n,
              netFDIsSet(sock, &rd) ? 1 : 0,
              netFDIsSet(sysFileDescStdIn, &rd) ? 1 : 0, err);
    Log(buf);

    while (total < 2048) {
        n = NetLibReceive(netRef, sock, gRecvBuf, sizeof(gRecvBuf) - 1, 0, NULL, NULL,
                          5 * SysTicksPerSecond(), &err);
        if (n <= 0)
            break;
        if (first) {
            Int16 i;

            gRecvBuf[n] = '\0';
            for (i = 0; i < n && gRecvBuf[i] != '\r' && gRecvBuf[i] != '\n'; i++)
                ;
            gRecvBuf[i] = '\0';
            Log(gRecvBuf);
            first = false;
        }
        total += n;
    }
    StrPrintF(buf, "Received %d bytes, last %d err 0x%x", total, n, err);
    Log(buf);

close_sock:
    LogErr("SocketClose", NetLibSocketClose(netRef, sock, 5 * SysTicksPerSecond(), &err) < 0 ? err : 0);
close_lib:
    LogErr("NetLibClose", NetLibClose(netRef, false));
}

/* ---- UI ------------------------------------------------------------------ */

static Boolean MainHandleEvent(EventType *e)
{
    switch (e->eType) {
    case frmOpenEvent:
        FrmDrawForm(FrmGetActiveForm());
        Log("Install the shim, then try DNS/HTTP.");
        return true;
    case ctlSelectEvent:
        switch (e->data.ctlSelect.controlID) {
        case InstallButton: DoInstall(); return true;
        case RemoveButton:  DoRemove();  return true;
        case StatusButton:  DoStatus();  return true;
        case DnsButton:     DoDns();     return true;
        case HttpButton:    DoHttp();    return true;
        case ClearButton:   ClearLog();  return true;
        }
        break;
    case sclRepeatEvent:
        ScrollLines((Int16)(e->data.sclRepeat.newValue - e->data.sclRepeat.value));
        return false;
    case keyDownEvent:
        if (e->data.keyDown.chr == pageUpChr) {
            ScrollLines(-10);
            return true;
        }
        if (e->data.keyDown.chr == pageDownChr) {
            ScrollLines(10);
            return true;
        }
        break;
    case frmCloseEvent: {
        FieldType *fld = GetObj(LogField);
        MemHandle h = FldGetTextHandle(fld);

        FldSetTextHandle(fld, NULL);
        if (h != NULL)
            MemHandleFree(h);
        break;
    }
    default:
        break;
    }
    return false;
}

static Boolean AppHandleEvent(EventType *e)
{
    if (e->eType == frmLoadEvent) {
        FormType *frm = FrmInitForm(e->data.frmLoad.formID);

        FrmSetActiveForm(frm);
        FrmSetEventHandler(frm, MainHandleEvent);
        return true;
    }
    return false;
}

UInt32 PilotMain(UInt16 cmd, void *cmdPBP, UInt16 launchFlags)
{
    EventType e;
    Err err;

    (void)cmdPBP;
    (void)launchFlags;

    if (cmd != sysAppLaunchCmdNormalLaunch)
        return 0;

    FrmGotoForm(MainForm);
    do {
        EvtGetEvent(&e, evtWaitForever);
        if (SysHandleEvent(&e))
            continue;
        if (MenuHandleEvent(NULL, &e, &err))
            continue;
        if (AppHandleEvent(&e))
            continue;
        FrmDispatchEvent(&e);
    } while (e.eType != appStopEvent);
    FrmCloseAllForms();
    return 0;
}
