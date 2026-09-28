/* palm/apps/fujiconfig/fujiconfig.c
 *
 * FujiConfig -- configures a FujiNet adapter (WiFi, host slots) and
 * installs/launches Palm apps from it. Targets Palm OS 3.1 / DragonBall EZ
 * (Handspring Visor Deluxe), 160x160 mono/4-gray.
 *
 * Unlike ../mastodon and ../fnlink, this app talks to the adapter through
 * the standard fujinet-lib-palmos client library (the fuji_ and network_
 * functions), not palmos-rs232 own core/ FujiBus API. See
 * fujinet-lib-palmos/include/fujinet-fuji.h, fujinet-network.h and
 * fujinet-palmos.h.
 *
 * Header order matters: fujinet-int.h pulls in the compiler stdbool.h,
 * which must be seen before PalmOS.h PalmTypes.h defines its own
 * true/false, so every fujinet header here comes before PalmOS.h
 * (see fujinet-lib-palmos/bus/palmos/link_palmos.c top comment).
 *
 * Single top-level event loop, same shape as mastodon.c/fnlink.c: no
 * FrmDoDialog anywhere. Small modal dialogs (WiFi password entry, host
 * slot editing, install confirmation) are popup forms layered on their
 * parent via FrmPopupForm/FrmReturnToForm, handled through the same loop.
 */
#include "fujinet-fuji.h"
#include "fujinet-network.h"
#include "fujinet-palmos.h"
#include <PalmOS.h>
#include "fujiconfig_rsc.h"

#define NUMBUF_CAP 16

/* ------------------------------------------------------------------ */
/* Link preference (Link/Baud popups on MainForm)                      */
/* ------------------------------------------------------------------ */

static const char *const kLibNames[3]   = { "USB Library", "BuiltIn SerLib", "Serial Library" };
static const char *const kBaudLabels[5] = { "115200", "57600", "38400", "19200", "9600" };
static const UInt32 kBauds[5] = { 115200uL, 57600uL, 38400uL, 19200uL, 9600uL };

static UInt16 gLinkIdx = 0; /* index into kLibNames; default USB Library */
static UInt16 gBaudIdx = 0; /* index into kBauds; default 115200 */

/* ------------------------------------------------------------------ */
/* MainForm status                                                     */
/* ------------------------------------------------------------------ */

static AdapterConfigExtended gAdapterExt;
static char gSsid[34];
static char gHost[65];
static char gIp[20];
static char gFw[16];
static char gConnLine[24] = "Not connected";

/* ------------------------------------------------------------------ */
/* WiFi form                                                            */
/* ------------------------------------------------------------------ */

#define WIFI_MAX_ENTRIES 64
static SSIDInfo gWifiEntries[WIFI_MAX_ENTRIES];
static UInt16 gWifiCount = 0;
static char gWifiSelectedSsid[SSID_MAXLEN];
static char gWifiPasswordBuf[MAX_PASSWORD_LEN];

/* ------------------------------------------------------------------ */
/* Hosts form                                                           */
/* ------------------------------------------------------------------ */

#define HOST_SLOT_COUNT 8
static HostSlot gHostSlots[HOST_SLOT_COUNT];
static Int16 gHostsSelectedIdx = noListSelection;
static UInt16 gEditSlotIdx;
static char gEditBuf[32];

/* ------------------------------------------------------------------ */
/* Browse form                                                          */
/* ------------------------------------------------------------------ */

#define BROWSE_MAX_ENTRIES 200
#define BROWSE_NAME_CAP    40

typedef struct {
    char name[BROWSE_NAME_CAP];
    UInt8 isDir;
} BrowseEntry;

static UInt8 gBrowseHostSlot;
static char gBrowseHostValue[32];
static char gBrowsePath[256];
static char gBrowseTitle[48];         /* Browse form title; see BrowseRelist */
static MemHandle gBrowseEntriesH = 0;
static UInt16 gBrowseCount = 0;

/* Scroll offset shared by whichever variable-length List is on screen
 * (WiFi results or Browse entries -- never both at once). */
static Int16 gListTop = 0;

/* ------------------------------------------------------------------ */
/* Install confirm / progress                                          */
/* ------------------------------------------------------------------ */

static char gInstallFilename[BROWSE_NAME_CAP];
static char gInstallPath[300];       /* full remote path, e.g. "/palm/x.prc" */
static char gInstallDeviceSpec[300]; /* "N:SD:/..." or "N:TNFS://host/..." */
static Boolean gInstallRun = false;
static Boolean gInstallCanRun = false;
static UInt32 gInstallTotalBytes = 0;
static char gInstallStatusLine[48] = "";

/* ------------------------------------------------------------------ */
/* Small generic helpers                                               */
/* ------------------------------------------------------------------ */

static void *GetObj(FormType *frm, UInt16 id)
{
    return FrmGetObjectPtr(frm, FrmGetObjectIndex(frm, id));
}

/* Copies up to (srcCap) bytes from src into dst, stopping early at an
 * embedded NUL, and always NUL-terminates dst within dstCap. Used for any
 * fixed-size field from the device that isn't guaranteed to be
 * NUL-terminated within its own array bounds (AdapterConfigExtended's
 * char[] fields in particular). */
static void ClampedCopy(char *dst, UInt16 dstCap, const char *src, UInt16 srcCap)
{
    UInt16 n = 0;

    while (n < srcCap && n < (UInt16)(dstCap - 1) && src[n] != '\0') {
        n++;
    }
    MemMove(dst, (void *)src, n);
    dst[n] = '\0';
}

/* Single shared error-reporting helper -- every failure site in this app
 * routes through here rather than calling FrmCustomAlert directly. `what`
 * is a short human description of what failed; `reason` an optional short
 * sub-step tag (may be "" or 0). The low 4 hex digits of
 * fuji_palmos_last_error() are shown, matching fnlink.c's StrIToH pattern. */
static void ShowError(const char *what, const char *reason)
{
    char hex[9];
    char err4[5];
    UInt16 e = fuji_palmos_last_error();

    StrIToH(hex, (UInt32)e);
    StrNCopy(err4, hex + 4, 4);
    err4[4] = '\0';
    FrmCustomAlert(GeneralAlert, what, reason ? reason : "", err4);
}

static void ShowInfo(const char *msg)
{
    FrmCustomAlert(InfoAlert, msg, "", "");
}

/* Draws one line of text in the active form's window, clearing the rest of
 * the row first. Used for the dialogs' variable text: CtlSetLabel only works
 * on controls, not LABEL objects. */
static void DrawTextLine(const char *text, Coord x, Coord y, Coord width)
{
    RectangleType r;

    r.topLeft.x = x;
    r.topLeft.y = y;
    r.extent.x = width;
    r.extent.y = FntLineHeight();
    WinEraseRectangle(&r, 0);
    WinDrawChars(text, StrLen(text), x, y);
}

/* Gives an editable field its own text handle (FldSetTextPtr is only for
 * non-editable fields). The form frees the handle when it is deleted. */
static void FieldSetEditText(FieldType *fld, const char *init, UInt16 cap)
{
    MemHandle h = MemHandleNew(cap);
    char *p;

    if (!h) {
        return;
    }
    p = (char *)MemHandleLock(h);
    StrNCopy(p, init, (Int16)(cap - 1));
    p[cap - 1] = '\0';
    MemHandleUnlock(h);
    FldSetTextHandle(fld, h);
    FldDrawField(fld);
}

static void FieldGetEditText(FieldType *fld, char *out, UInt16 cap)
{
    const char *p = FldGetTextPtr(fld);

    StrNCopy(out, p ? p : "", (Int16)(cap - 1));
    out[cap - 1] = '\0';
}

/* Shared by the WiFi-results and Browse-entries Lists: clamps gListTop to
 * the current item count and syncs the List/ScrollBar to it. */
static void ListScrollSetup(ListType *lst, ScrollBarType *bar, Int16 count)
{
    Int16 visible = LstGetVisibleItems(lst);
    Int16 maxTop = (count > visible) ? (Int16)(count - visible) : 0;

    if (gListTop > maxTop) {
        gListTop = maxTop;
    }
    if (gListTop < 0) {
        gListTop = 0;
    }
    LstSetTopItem(lst, gListTop);
    SclSetScrollBar(bar, gListTop, 0, maxTop, (visible > 0) ? visible : 1);
}

static Boolean HandleListScroll(FormType *frm, UInt16 listId, EventType *e)
{
    ListType *lst = (ListType *)GetObj(frm, listId);

    gListTop = e->data.sclRepeat.newValue;
    LstSetTopItem(lst, gListTop);
    LstDrawList(lst);
    return true;
}

/* path/PathPop/PathAppend maintain a "/"-rooted directory path with no
 * trailing slash except at the root itself. */
static void PathAppend(char *path, UInt16 cap, const char *seg)
{
    UInt16 len = StrLen(path);

    if (len > 0 && path[len - 1] != '/') {
        StrNCat(path, "/", (Int16)(cap - 1 - len));
        len = StrLen(path);
    }
    StrNCat(path, seg, (Int16)(cap - 1 - len));
}

static void PathPop(char *path)
{
    UInt16 len = StrLen(path);

    if (len <= 1) {
        return; /* already at root */
    }
    if (path[len - 1] == '/') {
        path[len - 1] = '\0';
        len--;
    }
    while (len > 0 && path[len - 1] != '/') {
        len--;
    }
    if (len == 0) {
        len = 1;
    }
    path[len] = '\0';
}

/* Case-insensitive suffix compare -- there is no strcasecmp here. */
static Boolean HasExtCI(const char *name, const char *ext)
{
    UInt16 nlen = StrLen(name);
    UInt16 elen = StrLen(ext);

    if (nlen < elen) {
        return false;
    }
    return (Boolean)(StrNCaselessCompare(name + (nlen - elen), ext, elen) == 0);
}

/* Builds the network devicespec for a file under a given host slot value.
 * "SD" (case-insensitive) is FujiConfig's own heuristic for the on-device
 * SD card -- host slots only carry a display string, no machine-readable
 * type -- and uses the confirmed-live "N:SD:/path" form. Any other value
 * is treated as a TNFS hostname; that branch is NOT live-tested (no
 * reachable TNFS host in the verification environment), built per
 * fujinet-network.h's documented "N:PROTO://[HOSTNAME]:PORT/PATH" form. */
static void BuildDeviceSpec(const char *hostValue, const char *path, char *out, UInt16 outCap)
{
    if (StrCaselessCompare(hostValue, "SD") == 0) {
        StrNCopy(out, "N:SD:", (Int16)(outCap - 1));
        out[outCap - 1] = '\0';
        StrNCat(out, path, (Int16)(outCap - 1 - StrLen(out)));
    } else {
        StrNCopy(out, "N:TNFS://", (Int16)(outCap - 1));
        out[outCap - 1] = '\0';
        StrNCat(out, hostValue, (Int16)(outCap - 1 - StrLen(out)));
        StrNCat(out, "/", (Int16)(outCap - 1 - StrLen(out)));
        StrNCat(out, (path[0] == '/') ? path + 1 : path, (Int16)(outCap - 1 - StrLen(out)));
    }
}

/* ------------------------------------------------------------------ */
/* Link preference                                                     */
/* ------------------------------------------------------------------ */

static void LoadLinkPref(void)
{
    FujiPalmosLinkPref pref;
    UInt16 size = sizeof(pref);
    Int16 rc;
    UInt16 i;

    gLinkIdx = 0;
    gBaudIdx = 0;

    rc = PrefGetAppPreferences(FUJI_PALMOS_PREF_CREATOR, FUJI_PALMOS_PREF_ID, &pref, &size, false);
    if (rc != FUJI_PALMOS_PREF_VERSION || size != sizeof(pref)) {
        return;
    }
    for (i = 0; i < 3; i++) {
        if (StrCompare(pref.lib_name, (char *)kLibNames[i]) == 0) {
            gLinkIdx = i;
            break;
        }
    }
    for (i = 0; i < 5; i++) {
        if (pref.baud == kBauds[i]) {
            gBaudIdx = i;
            break;
        }
    }
}

static void SaveLinkPref(void)
{
    FujiPalmosLinkPref pref;

    pref.version = FUJI_PALMOS_PREF_VERSION;
    pref.baud = kBauds[gBaudIdx];
    MemSet(pref.lib_name, sizeof(pref.lib_name), 0);
    StrNCopy(pref.lib_name, (char *)kLibNames[gLinkIdx], sizeof(pref.lib_name) - 1);
    PrefSetAppPreferences(FUJI_PALMOS_PREF_CREATOR, FUJI_PALMOS_PREF_ID,
                           FUJI_PALMOS_PREF_VERSION, &pref, sizeof(pref), false);
}

static void SyncLinkPopups(FormType *frm)
{
    ControlType *linkTrig = (ControlType *)GetObj(frm, LinkPopTrigger);
    ListType *linkList    = (ListType *)GetObj(frm, LinkList);
    ControlType *baudTrig = (ControlType *)GetObj(frm, BaudPopTrigger);
    ListType *baudList    = (ListType *)GetObj(frm, BaudList);

    CtlSetLabel(linkTrig, (char *)kLibNames[gLinkIdx]);
    LstSetSelection(linkList, (Int16)gLinkIdx);
    CtlSetLabel(baudTrig, (char *)kBaudLabels[gBaudIdx]);
    LstSetSelection(baudList, (Int16)gBaudIdx);
}

/* ------------------------------------------------------------------ */
/* Browse entry point shared by MainForm's Browse button (defaults to    */
/* slot 0) and HostsForm's Browse button (uses the selected slot).       */
/* ------------------------------------------------------------------ */

static void StartBrowse(UInt8 hostSlot)
{
    if (!fuji_get_host_slots(gHostSlots, HOST_SLOT_COUNT)) {
        ShowError("Could not read host slots", "");
        return;
    }
    gBrowseHostSlot = hostSlot;
    ClampedCopy(gBrowseHostValue, sizeof(gBrowseHostValue),
                (char *)gHostSlots[hostSlot], sizeof(gHostSlots[hostSlot]));
    if (gBrowseHostValue[0] == '\0') {
        ShowInfo("That host slot is empty.");
        return;
    }
    StrCopy(gBrowsePath, "/");
    FrmGotoForm(BrowseForm);
}

/* ------------------------------------------------------------------ */
/* MainForm                                                             */
/* ------------------------------------------------------------------ */

static void DrawMainStatus(FormType *frm)
{
    RectangleType r;
    char line[96];

    if (!frm) {
        return;
    }

    r.topLeft.x = 4;
    r.topLeft.y = 64;
    r.extent.x = 152;
    r.extent.y = 68;
    WinEraseRectangle(&r, 0);

    StrPrintF(line, "SSID: %s", gSsid);
    WinDrawChars(line, StrLen(line), 4, 64);
    StrPrintF(line, "Host: %s", gHost);
    WinDrawChars(line, StrLen(line), 4, 76);
    StrPrintF(line, "IP: %s", gIp);
    WinDrawChars(line, StrLen(line), 4, 88);
    StrPrintF(line, "FW: %s", gFw);
    WinDrawChars(line, StrLen(line), 4, 100);
    WinDrawChars(gConnLine, StrLen(gConnLine), 4, 116);
}

static Boolean RequireConnected(void)
{
    if (!fuji_palmos_is_open()) {
        ShowInfo("Connect first.");
        return false;
    }
    return true;
}

static void DoConnect(FormType *frm)
{
    if (fuji_palmos_is_open()) {
        fuji_palmos_close();
    }

    /* Save the link on every Connect, not only when a popup changes: the
     * other fujinet-lib apps open whatever the preference says, and a
     * never-saved preference falls back to "Serial Library". */
    SaveLinkPref();
    if (!fuji_palmos_open((char *)kLibNames[gLinkIdx], kBauds[gBaudIdx])) {
        ShowError("Connect failed", "");
        gSsid[0] = '\0';
        gHost[0] = '\0';
        gIp[0] = '\0';
        gFw[0] = '\0';
        StrCopy(gConnLine, "Not connected");
        DrawMainStatus(frm);
        return;
    }

    if (fuji_get_adapter_config_extended(&gAdapterExt)) {
        ClampedCopy(gSsid, sizeof(gSsid), gAdapterExt.ssid, sizeof(gAdapterExt.ssid));
        ClampedCopy(gHost, sizeof(gHost), gAdapterExt.hostname, sizeof(gAdapterExt.hostname));
        ClampedCopy(gIp, sizeof(gIp), gAdapterExt.sLocalIP, sizeof(gAdapterExt.sLocalIP));
        ClampedCopy(gFw, sizeof(gFw), gAdapterExt.fn_version, sizeof(gAdapterExt.fn_version));
    } else {
        AdapterConfig ac;

        if (fuji_get_adapter_config(&ac)) {
            ClampedCopy(gSsid, sizeof(gSsid), ac.ssid, sizeof(ac.ssid));
            ClampedCopy(gHost, sizeof(gHost), ac.hostname, sizeof(ac.hostname));
            StrPrintF(gIp, "%d.%d.%d.%d", ac.localIP[0], ac.localIP[1], ac.localIP[2], ac.localIP[3]);
            ClampedCopy(gFw, sizeof(gFw), ac.fn_version, sizeof(ac.fn_version));
        } else {
            ShowError("Could not read adapter config", "");
            gSsid[0] = '\0';
            gHost[0] = '\0';
            gIp[0] = '\0';
            gFw[0] = '\0';
        }
    }

    StrCopy(gConnLine, "Connected");
    DrawMainStatus(frm);
}

static Boolean MainFormHandleEvent(EventType *e)
{
    switch (e->eType) {
    case frmOpenEvent: {
        FormType *frm = FrmGetActiveForm();

        FrmDrawForm(frm);
        SyncLinkPopups(frm);
        DrawMainStatus(frm);
        return true;
    }

    case frmUpdateEvent: {
        FormType *frm = FrmGetActiveForm();

        FrmDrawForm(frm);
        DrawMainStatus(frm);
        return true;
    }

    case popSelectEvent:
        if (e->data.popSelect.controlID == LinkPopTrigger ||
            e->data.popSelect.controlID == BaudPopTrigger) {
            if (e->data.popSelect.controlID == LinkPopTrigger) {
                gLinkIdx = (UInt16)e->data.popSelect.selection;
            } else {
                gBaudIdx = (UInt16)e->data.popSelect.selection;
            }
            SaveLinkPref();
            if (fuji_palmos_is_open()) {
                fuji_palmos_close();
            }
            StrCopy(gConnLine, "Not connected");
            DrawMainStatus(FrmGetActiveForm());
            return false; /* let the default handler update the trigger label */
        }
        return false;

    case ctlSelectEvent:
        switch (e->data.ctlSelect.controlID) {
        case ConnectButton:
            DoConnect(FrmGetActiveForm());
            return true;
        case WifiButton:
            if (RequireConnected()) {
                FrmGotoForm(WifiForm);
            }
            return true;
        case HostsButton:
            if (RequireConnected()) {
                FrmGotoForm(HostsForm);
            }
            return true;
        case BrowseButton:
            if (RequireConnected()) {
                StartBrowse(0);
            }
            return true;
        default:
            return false;
        }

    default:
        return false;
    }
}

/* ------------------------------------------------------------------ */
/* WiFi form                                                            */
/* ------------------------------------------------------------------ */

static void WifiListDrawFunc(Int16 itemNum, RectangleType *bounds, Char **itemsText)
{
    char name[SSID_MAXLEN];
    char line[48];

    if (itemNum < 0 || (UInt16)itemNum >= gWifiCount) {
        return;
    }
    ClampedCopy(name, sizeof(name), gWifiEntries[itemNum].ssid, sizeof(gWifiEntries[itemNum].ssid));
    StrPrintF(line, "%s (%d dBm)", name, (Int16)gWifiEntries[itemNum].rssi);
    WinDrawChars(line, StrLen(line), bounds->topLeft.x, bounds->topLeft.y);
}

static void DoWifiScan(FormType *frm)
{
    uint8_t count = 0;
    UInt16 i;
    ListType *lst = (ListType *)GetObj(frm, WifiList);
    ScrollBarType *bar = (ScrollBarType *)GetObj(frm, WifiScrollBar);

    EvtResetAutoOffTimer();
    if (!fuji_scan_for_networks(&count)) {
        ShowError("Scan failed", "");
        return;
    }

    if (count > WIFI_MAX_ENTRIES) {
        count = WIFI_MAX_ENTRIES;
    }
    gWifiCount = 0;
    for (i = 0; i < count; i++) {
        SSIDInfo info;

        EvtResetAutoOffTimer();
        if (fuji_get_scan_result((uint8_t)i, &info)) {
            gWifiEntries[gWifiCount] = info;
            gWifiCount++;
        }
    }

    gListTop = 0;
    LstSetListChoices(lst, 0, (Int16)gWifiCount);
    ListScrollSetup(lst, bar, (Int16)gWifiCount);
    LstDrawList(lst);
}

static Boolean WifiFormHandleEvent(EventType *e)
{
    switch (e->eType) {
    case frmOpenEvent: {
        FormType *frm = FrmGetActiveForm();
        ListType *lst = (ListType *)GetObj(frm, WifiList);
        ScrollBarType *bar = (ScrollBarType *)GetObj(frm, WifiScrollBar);

        FrmDrawForm(frm);
        gListTop = 0;
        LstSetDrawFunction(lst, WifiListDrawFunc);
        LstSetListChoices(lst, 0, (Int16)gWifiCount);
        ListScrollSetup(lst, bar, (Int16)gWifiCount);
        LstDrawList(lst);
        return true;
    }

    case frmUpdateEvent: {
        FormType *frm = FrmGetActiveForm();
        ListType *lst = (ListType *)GetObj(frm, WifiList);

        FrmDrawForm(frm);
        LstDrawList(lst);
        return true;
    }

    case ctlSelectEvent:
        if (e->data.ctlSelect.controlID == WifiScanButton) {
            DoWifiScan(FrmGetActiveForm());
            return true;
        }
        if (e->data.ctlSelect.controlID == WifiDoneButton) {
            FrmGotoForm(MainForm);
            return true;
        }
        return false;

    case lstSelectEvent:
        if (e->data.lstSelect.listID == WifiList) {
            Int16 sel = e->data.lstSelect.selection;

            if (sel >= 0 && (UInt16)sel < gWifiCount) {
                ClampedCopy(gWifiSelectedSsid, sizeof(gWifiSelectedSsid),
                            gWifiEntries[sel].ssid, sizeof(gWifiEntries[sel].ssid));
                FrmPopupForm(WifiPasswordDialog);
            }
            return true;
        }
        return false;

    case sclRepeatEvent:
        return HandleListScroll(FrmGetActiveForm(), WifiList, e);

    default:
        return false;
    }
}

static void DoWifiConnect(FormType *frm)
{
    NetConfig nc;
    UInt8 i;
    Boolean ok = false;
    Boolean failed = false;

    FieldGetEditText((FieldType *)GetObj(frm, WifiPasswordField),
                     gWifiPasswordBuf, sizeof(gWifiPasswordBuf));
    MemSet(&nc, sizeof(nc), 0);
    StrNCopy(nc.ssid, gWifiSelectedSsid, sizeof(nc.ssid) - 1);
    StrNCopy(nc.password, gWifiPasswordBuf, sizeof(nc.password) - 1);

    if (!fuji_set_ssid(&nc)) {
        ShowError("WiFi connect failed", "set_ssid");
        FrmReturnToForm(0);
        return;
    }

    DrawTextLine("Connecting...", 6, 18, 136);

    for (i = 0; i < 15; i++) {
        EventType e2;
        uint8_t status = 0;

        EvtResetAutoOffTimer();
        EvtGetEvent(&e2, SysTicksPerSecond());
        if (e2.eType != nilEvent) {
            SysHandleEvent(&e2);
        }
        if (!fuji_get_wifi_status(&status)) {
            continue;
        }
        if (status == connected) {
            ok = true;
            break;
        }
        if (status == connect_failed || status == connection_lost) {
            failed = true;
            break;
        }
    }

    if (ok) {
        ShowInfo("WiFi connected.");
    } else if (failed) {
        ShowError("WiFi connect failed", "");
    } else {
        ShowError("WiFi connect timed out", "");
    }

    FrmReturnToForm(0);
}

static Boolean WifiPasswordHandleEvent(EventType *e)
{
    switch (e->eType) {
    case frmOpenEvent: {
        FormType *frm = FrmGetActiveForm();
        FieldType *fld = (FieldType *)GetObj(frm, WifiPasswordField);

        FrmDrawForm(frm);
        DrawTextLine(gWifiSelectedSsid, 6, 18, 136);
        MemSet(gWifiPasswordBuf, sizeof(gWifiPasswordBuf), 0);
        FieldSetEditText(fld, "", sizeof(gWifiPasswordBuf));
        FrmSetFocus(frm, FrmGetObjectIndex(frm, WifiPasswordField));
        return true;
    }

    case ctlSelectEvent:
        if (e->data.ctlSelect.controlID == WifiCancelButton) {
            FrmReturnToForm(0);
            return true;
        }
        if (e->data.ctlSelect.controlID == WifiConnectButton) {
            DoWifiConnect(FrmGetActiveForm());
            return true;
        }
        return false;

    default:
        return false;
    }
}

/* ------------------------------------------------------------------ */
/* Hosts form                                                           */
/* ------------------------------------------------------------------ */

static void HostsListDrawFunc(Int16 itemNum, RectangleType *bounds, Char **itemsText)
{
    char name[32];
    char line[40];

    if (itemNum < 0 || itemNum >= HOST_SLOT_COUNT) {
        return;
    }
    ClampedCopy(name, sizeof(name), (char *)gHostSlots[itemNum], sizeof(gHostSlots[itemNum]));
    if (name[0] == '\0') {
        StrPrintF(line, "%d: (empty)", (Int16)(itemNum + 1));
    } else {
        StrPrintF(line, "%d: %s", (Int16)(itemNum + 1), name);
    }
    WinDrawChars(line, StrLen(line), bounds->topLeft.x, bounds->topLeft.y);
}

static Boolean HostsFormHandleEvent(EventType *e)
{
    switch (e->eType) {
    case frmOpenEvent: {
        FormType *frm = FrmGetActiveForm();
        ListType *lst = (ListType *)GetObj(frm, HostsList);

        FrmDrawForm(frm);
        if (!fuji_get_host_slots(gHostSlots, HOST_SLOT_COUNT)) {
            ShowError("Could not read host slots", "");
        }
        gHostsSelectedIdx = noListSelection;
        LstSetDrawFunction(lst, HostsListDrawFunc);
        LstSetListChoices(lst, 0, HOST_SLOT_COUNT);
        LstDrawList(lst);
        return true;
    }

    case frmUpdateEvent: {
        FormType *frm = FrmGetActiveForm();
        ListType *lst = (ListType *)GetObj(frm, HostsList);

        FrmDrawForm(frm);
        LstDrawList(lst);
        return true;
    }

    case ctlSelectEvent:
        if (e->data.ctlSelect.controlID == HostsDoneButton) {
            FrmGotoForm(MainForm);
            return true;
        }
        if (e->data.ctlSelect.controlID == HostsBrowseButton) {
            UInt8 slot = (gHostsSelectedIdx != noListSelection) ? (UInt8)gHostsSelectedIdx : 0;

            StartBrowse(slot);
            return true;
        }
        return false;

    case lstSelectEvent:
        if (e->data.lstSelect.listID == HostsList) {
            gHostsSelectedIdx = e->data.lstSelect.selection;
            gEditSlotIdx = (UInt16)gHostsSelectedIdx;
            ClampedCopy(gEditBuf, sizeof(gEditBuf),
                        (char *)gHostSlots[gEditSlotIdx], sizeof(gHostSlots[gEditSlotIdx]));
            FrmPopupForm(HostEditDialog);
            return true;
        }
        return false;

    default:
        return false;
    }
}

static Boolean HostEditHandleEvent(EventType *e)
{
    switch (e->eType) {
    case frmOpenEvent: {
        FormType *frm = FrmGetActiveForm();
        FieldType *fld = (FieldType *)GetObj(frm, HostEditField);

        FrmDrawForm(frm);
        FieldSetEditText(fld, gEditBuf, sizeof(gEditBuf));
        FrmSetFocus(frm, FrmGetObjectIndex(frm, HostEditField));
        return true;
    }

    case ctlSelectEvent:
        if (e->data.ctlSelect.controlID == HostEditCancelButton) {
            FrmReturnToForm(0);
            return true;
        }
        if (e->data.ctlSelect.controlID == HostEditOkButton) {
            FieldGetEditText((FieldType *)GetObj(FrmGetActiveForm(), HostEditField),
                             gEditBuf, sizeof(gEditBuf));
            MemSet(gHostSlots[gEditSlotIdx], sizeof(gHostSlots[gEditSlotIdx]), 0);
            StrNCopy((char *)gHostSlots[gEditSlotIdx], gEditBuf, sizeof(gHostSlots[gEditSlotIdx]) - 1);
            if (!fuji_put_host_slots(gHostSlots, HOST_SLOT_COUNT)) {
                ShowError("Could not save host slots", "");
            }
            FrmReturnToForm(0);
            FrmUpdateForm(HostsForm, frmRedrawUpdateCode);
            return true;
        }
        return false;

    default:
        return false;
    }
}

/* ------------------------------------------------------------------ */
/* Browse form                                                          */
/* ------------------------------------------------------------------ */

static void FreeBrowseEntries(void)
{
    if (gBrowseEntriesH) {
        MemHandleFree(gBrowseEntriesH);
        gBrowseEntriesH = 0;
    }
    gBrowseCount = 0;
}

/* Mounts hostSlot and lists path into gBrowseEntriesH/gBrowseCount (a
 * synthetic ".." row first, if not at root). */
static Boolean ListDirectory(UInt8 hostSlot, const char *path)
{
    char pathbuf[MAX_FILENAME_LEN];
    UInt16 plen;
    BrowseEntry *entries;
    UInt16 count = 0;
    UInt16 iter;
    Boolean ok = true;

    FreeBrowseEntries();

    if (!fuji_mount_host_slot(hostSlot)) {
        ShowError("Could not mount host", "");
        return false;
    }

    plen = StrLen(path);
    if (plen >= sizeof(pathbuf)) {
        plen = sizeof(pathbuf) - 1;
    }
    MemSet(pathbuf, sizeof(pathbuf), 0);
    MemMove(pathbuf, (void *)path, plen);
    /* fuji_open_directory_filter() has a bug when the filter is empty: it
     * forwards our path pointer straight into fuji_open_directory(), which
     * unconditionally reads a fixed MAX_FILENAME_LEN (256) bytes from it,
     * reading OOB garbage past a short path and sending it as request
     * payload (the firmware then treats bytes after the embedded NUL as an
     * fnmatch filter). Build our own zero-padded 256-byte buffer instead
     * and call the low-level fuji_open_directory() macro directly. */
    if (!fuji_open_directory(hostSlot, pathbuf)) {
        ShowError("Could not open directory", "");
        return false;
    }

    gBrowseEntriesH = MemHandleNew((UInt32)BROWSE_MAX_ENTRIES * sizeof(BrowseEntry));
    if (!gBrowseEntriesH) {
        fuji_close_directory();
        ShowError("Out of memory", "");
        return false;
    }
    entries = (BrowseEntry *)MemHandleLock(gBrowseEntriesH);

    if (StrCompare((char *)path, "/") != 0) {
        StrCopy(entries[0].name, "..");
        entries[0].isDir = 1;
        count = 1;
    }

    for (iter = 0; iter < 500 && count < BROWSE_MAX_ENTRIES; iter++) {
        UInt8 buf[256];
        UInt16 nlen;
        Boolean isDir;

        EvtResetAutoOffTimer();

        if (!fuji_read_directory(255, 0, buf)) {
            ok = false;
            break;
        }
        if (buf[0] == 0x7F && buf[1] == 0x7F) {
            break; /* end of listing */
        }
        buf[255] = '\0';

        nlen = StrLen((char *)buf);
        isDir = (nlen > 0 && buf[nlen - 1] == '/') ? true : false;
        if (isDir) {
            buf[nlen - 1] = '\0';
        }

        ClampedCopy(entries[count].name, BROWSE_NAME_CAP, (char *)buf, sizeof(buf));
        entries[count].isDir = (UInt8)isDir;
        count++;
    }

    fuji_close_directory();
    MemHandleUnlock(gBrowseEntriesH);
    gBrowseCount = count;

    if (!ok) {
        ShowError("Directory listing failed", "");
    }
    return ok;
}

static void BrowseListDrawFunc(Int16 itemNum, RectangleType *bounds, Char **itemsText)
{
    BrowseEntry *entries;
    char line[BROWSE_NAME_CAP + 4];

    if (!gBrowseEntriesH || itemNum < 0 || (UInt16)itemNum >= gBrowseCount) {
        return;
    }
    entries = (BrowseEntry *)MemHandleLock(gBrowseEntriesH);
    if (entries[itemNum].isDir) {
        StrPrintF(line, "%s/", entries[itemNum].name);
    } else {
        StrCopy(line, entries[itemNum].name);
    }
    WinDrawChars(line, StrLen(line), bounds->topLeft.x, bounds->topLeft.y);
    MemHandleUnlock(gBrowseEntriesH);
}

static void BrowseRelist(FormType *frm)
{
    ListType *lst = (ListType *)GetObj(frm, BrowseList);
    ScrollBarType *bar = (ScrollBarType *)GetObj(frm, BrowseScrollBar);

    /* FrmSetTitle keeps a pointer, so the buffer must outlive the form.
     * FrmCopyTitle can't be used: it overwrites the resource's title in
     * place and a path longer than "Browse" tramples the list after it. */
    StrPrintF(gBrowseTitle, "%s:", gBrowseHostValue);
    StrNCat(gBrowseTitle, gBrowsePath, sizeof(gBrowseTitle));
    FrmSetTitle(frm, gBrowseTitle);

    ListDirectory(gBrowseHostSlot, gBrowsePath);

    gListTop = 0;
    LstSetDrawFunction(lst, BrowseListDrawFunc);
    LstSetListChoices(lst, 0, (Int16)gBrowseCount);
    ListScrollSetup(lst, bar, (Int16)gBrowseCount);
    LstDrawList(lst);
}

static void BrowseEntryTap(FormType *frm, Int16 sel)
{
    BrowseEntry *entries;
    BrowseEntry entry;

    if (!gBrowseEntriesH || sel < 0 || (UInt16)sel >= gBrowseCount) {
        return;
    }

    entries = (BrowseEntry *)MemHandleLock(gBrowseEntriesH);
    entry = entries[sel];
    MemHandleUnlock(gBrowseEntriesH);

    if (entry.isDir && StrCompare(entry.name, "..") == 0) {
        PathPop(gBrowsePath);
        BrowseRelist(frm);
        return;
    }

    if (entry.isDir) {
        PathAppend(gBrowsePath, sizeof(gBrowsePath), entry.name);
        BrowseRelist(frm);
        return;
    }

    if (HasExtCI(entry.name, ".prc") || HasExtCI(entry.name, ".pdb") || HasExtCI(entry.name, ".pqa")) {
        StrNCopy(gInstallFilename, entry.name, sizeof(gInstallFilename) - 1);
        gInstallFilename[sizeof(gInstallFilename) - 1] = '\0';
        StrCopy(gInstallPath, gBrowsePath);
        PathAppend(gInstallPath, sizeof(gInstallPath), entry.name);
        gInstallCanRun = HasExtCI(entry.name, ".prc");
        FrmPopupForm(InstallConfirmDialog);
    } else {
        ShowInfo("Not a Palm database.");
    }
}

static Boolean BrowseFormHandleEvent(EventType *e)
{
    switch (e->eType) {
    case frmOpenEvent: {
        FormType *frm = FrmGetActiveForm();

        FrmDrawForm(frm);
        BrowseRelist(frm);
        return true;
    }

    case frmUpdateEvent: {
        FormType *frm = FrmGetActiveForm();
        ListType *lst = (ListType *)GetObj(frm, BrowseList);

        FrmDrawForm(frm);
        LstDrawList(lst);
        return true;
    }

    case frmCloseEvent:
        FreeBrowseEntries();
        return false;

    case ctlSelectEvent:
        if (e->data.ctlSelect.controlID == BrowseDoneButton) {
            FrmGotoForm(MainForm);
            return true;
        }
        return false;

    case lstSelectEvent:
        if (e->data.lstSelect.listID == BrowseList) {
            BrowseEntryTap(FrmGetActiveForm(), e->data.lstSelect.selection);
            return true;
        }
        return false;

    case sclRepeatEvent:
        return HandleListScroll(FrmGetActiveForm(), BrowseList, e);

    default:
        return false;
    }
}

/* ------------------------------------------------------------------ */
/* Install confirm dialog                                               */
/* ------------------------------------------------------------------ */

static Boolean InstallConfirmHandleEvent(EventType *e)
{
    switch (e->eType) {
    case frmOpenEvent: {
        FormType *frm = FrmGetActiveForm();
        ControlType *runBtn;
        char msg[64];

        FrmDrawForm(frm);
        StrPrintF(msg, "Install %s?", gInstallFilename);
        DrawTextLine(msg, 6, 18, 136);
        runBtn = (ControlType *)GetObj(frm, InstallRunButton);
        CtlSetEnabled(runBtn, gInstallCanRun);
        return true;
    }

    case ctlSelectEvent:
        if (e->data.ctlSelect.controlID == InstallCancelButton) {
            FrmReturnToForm(0);
            return true;
        }
        if (e->data.ctlSelect.controlID == InstallInstallButton) {
            gInstallRun = false;
            FrmGotoForm(InstallForm);
            return true;
        }
        if (e->data.ctlSelect.controlID == InstallRunButton) {
            gInstallRun = true;
            FrmGotoForm(InstallForm);
            return true;
        }
        return false;

    default:
        return false;
    }
}

/* ------------------------------------------------------------------ */
/* Install (progress) form                                              */
/* ------------------------------------------------------------------ */

static void DrawInstallStatus(FormType *frm)
{
    RectangleType r;
    char line[32];
    char num[NUMBUF_CAP];

    if (!frm) {
        return;
    }

    r.topLeft.x = 4;
    r.topLeft.y = 20;
    r.extent.x = 150;
    r.extent.y = 110;
    WinEraseRectangle(&r, 0);

    WinDrawChars(gInstallStatusLine, StrLen(gInstallStatusLine), 4, 20);

    StrIToA(num, (Int32)gInstallTotalBytes);
    StrCopy(line, num);
    StrCat(line, " bytes");
    WinDrawChars(line, StrLen(line), 4, 34);
}

static Err InstallReadProc(void *dataP, UInt32 *sizeP, void *userDataP)
{
    UInt16 reqLen = (UInt16)*sizeP;
    int16_t n;

    if (reqLen > 4096) {
        reqLen = 4096;
    }
    n = network_read(gInstallDeviceSpec, dataP, reqLen);
    if (n < 0) {
        *sizeP = 0;
        return exgErrUnknown;
    }
    *sizeP = (UInt32)n;
    gInstallTotalBytes += (UInt32)n;
    EvtResetAutoOffTimer();
    DrawInstallStatus(FrmGetActiveForm());
    return errNone;
}

static Boolean InstallDeleteProc(const char *nameP, UInt16 version, UInt16 cardNo, LocalID dbID, void *userDataP)
{
    UInt16 curCard;
    LocalID curID;

    if (SysCurAppDatabase(&curCard, &curID) == errNone && curCard == cardNo && curID == dbID) {
        return false; /* refuse to delete ourselves */
    }
    DmDeleteDatabase(cardNo, dbID);
    return true;
}

static void RunInstall(FormType *frm)
{
    LocalID dbID = 0;
    UInt16 cardNo = 0;
    Boolean needReset = false;
    Err err;
    FN_ERR nrc;

    gInstallTotalBytes = 0;
    StrCopy(gInstallStatusLine, "Installing...");
    BuildDeviceSpec(gBrowseHostValue, gInstallPath, gInstallDeviceSpec, sizeof(gInstallDeviceSpec));
    DrawInstallStatus(frm);

    nrc = network_open(gInstallDeviceSpec, OPEN_MODE_READ, OPEN_TRANS_NONE);
    if (nrc != FN_ERR_OK) {
        ShowError("Install failed", "open");
        StrCopy(gInstallStatusLine, "Failed.");
        DrawInstallStatus(frm);
        return;
    }

    err = ExgDBRead(InstallReadProc, InstallDeleteProc, 0, &dbID, cardNo, &needReset, true);
    network_close(gInstallDeviceSpec);

    if (err != errNone) {
        ShowError("Install failed", "");
        StrCopy(gInstallStatusLine, "Failed.");
        DrawInstallStatus(frm);
        return;
    }

    StrCopy(gInstallStatusLine, needReset ? "Installed (reset may be needed)." : "Installed.");
    DrawInstallStatus(frm);

    if (gInstallRun) {
        UInt32 type = 0;
        UInt32 creator = 0;

        if (DmDatabaseInfo(cardNo, dbID, 0, 0, 0, 0, 0, 0, 0, 0, 0, &type, &creator) == errNone
            && type == sysFileTApplication) {
            if (fuji_palmos_is_open()) {
                fuji_palmos_close();
            }
            SysUIAppSwitch(cardNo, dbID, sysAppLaunchCmdNormalLaunch, 0);
        }
    }
}

static Boolean InstallFormHandleEvent(EventType *e)
{
    switch (e->eType) {
    case frmOpenEvent: {
        FormType *frm = FrmGetActiveForm();

        FrmDrawForm(frm);
        RunInstall(frm);
        return true;
    }

    case frmUpdateEvent: {
        FormType *frm = FrmGetActiveForm();

        FrmDrawForm(frm);
        DrawInstallStatus(frm);
        return true;
    }

    case ctlSelectEvent:
        if (e->data.ctlSelect.controlID == InstallDoneButton) {
            FrmGotoForm(BrowseForm);
            return true;
        }
        return false;

    default:
        return false;
    }
}

/* ------------------------------------------------------------------ */
/* Form / event handling                                                */
/* ------------------------------------------------------------------ */

static Boolean AppHandleEvent(EventType *e)
{
    if (e->eType == frmLoadEvent) {
        UInt16 formId = e->data.frmLoad.formID;
        FormType *frm = FrmInitForm(formId);

        FrmSetActiveForm(frm);
        switch (formId) {
        case MainForm:
            FrmSetEventHandler(frm, MainFormHandleEvent);
            break;
        case WifiForm:
            FrmSetEventHandler(frm, WifiFormHandleEvent);
            break;
        case WifiPasswordDialog:
            FrmSetEventHandler(frm, WifiPasswordHandleEvent);
            break;
        case HostsForm:
            FrmSetEventHandler(frm, HostsFormHandleEvent);
            break;
        case HostEditDialog:
            FrmSetEventHandler(frm, HostEditHandleEvent);
            break;
        case BrowseForm:
            FrmSetEventHandler(frm, BrowseFormHandleEvent);
            break;
        case InstallConfirmDialog:
            FrmSetEventHandler(frm, InstallConfirmHandleEvent);
            break;
        case InstallForm:
            FrmSetEventHandler(frm, InstallFormHandleEvent);
            break;
        default:
            break;
        }
        return true;
    }
    return false;
}

UInt32 PilotMain(UInt16 cmd, MemPtr cmdPBP, UInt16 launchFlags)
{
    EventType e;
    UInt16 err;

    if (cmd != sysAppLaunchCmdNormalLaunch) {
        return 0;
    }

    LoadLinkPref();
    SaveLinkPref(); /* make sure the defaults exist for the other apps */
    FrmGotoForm(MainForm);

    do {
        EvtGetEvent(&e, evtWaitForever);
        if (SysHandleEvent(&e)) continue;
        if (MenuHandleEvent(0, &e, &err)) continue;
        if (AppHandleEvent(&e)) continue;
        FrmDispatchEvent(&e);
    } while (e.eType != appStopEvent);

    if (fuji_palmos_is_open()) {
        fuji_palmos_close();
    }
    FreeBrowseEntries();

    FrmCloseAllForms();
    return 0;
}
