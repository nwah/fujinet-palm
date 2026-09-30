/* palm/apps/fujiconfig/fujiconfig.c
 *
 * FujiConfig -- configures a FujiNet adapter (WiFi, host slots) and
 * installs/launches Palm apps from it. Targets Palm OS 3.1 / DragonBall EZ
 * (Handspring Visor Deluxe), 160x160 mono/4-gray.
 *
 * Unlike ../mastodon and ../fnlink, this app talks to the adapter through
 * the standard fujinet-lib-palmos client library (the fuji_ and network_
 * functions), not fujinet-palm's own core/ FujiBus API. See
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
#include "fnnetlib.h"

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
static Boolean gInstallRun = false;  /* Run: a temporary install, see RunTemporarily */

/* The app being run temporarily, remembered (unsaved preference) until it
 * is deleted, so a reset while it runs doesn't leave it behind. */
#define TEMP_APP_PREF_ID      3
#define TEMP_APP_PREF_VERSION 1
typedef struct {
    UInt32 creator;
    char   name[dmDBNameLength];
} TempAppPref;
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
/* Connection                                                           */
/* ------------------------------------------------------------------ */

static Boolean gHostSlotsValid = false;  /* gHostSlots read since connecting */
static Boolean gTriedConnect = false;    /* the automatic connect has run */

static void ClearAdapterInfo(void)
{
    gSsid[0] = '\0';
    gHost[0] = '\0';
    gIp[0] = '\0';
    gFw[0] = '\0';
}

/* Opens the link, reads the adapter config and the host slots. `quiet`
 * skips the error alerts (the automatic connect at launch). */
static Boolean ConnectLink(Boolean quiet)
{
    AdapterConfig ac;

    if (fuji_palmos_is_open()) {
        fuji_palmos_close();
    }
    gHostSlotsValid = false;
    ClearAdapterInfo();

    /* Save the link on every Connect, not only when a popup changes: the
     * other fujinet-lib apps open whatever the preference says, and a
     * never-saved preference falls back to "Serial Library". */
    SaveLinkPref();
    if (!fuji_palmos_open((char *)kLibNames[gLinkIdx], kBauds[gBaudIdx])) {
        if (!quiet) {
            ShowError("Connect failed", "");
        }
        StrCopy(gConnLine, "Not connected");
        return false;
    }

    if (fuji_get_adapter_config_extended(&gAdapterExt)) {
        ClampedCopy(gSsid, sizeof(gSsid), gAdapterExt.ssid, sizeof(gAdapterExt.ssid));
        ClampedCopy(gHost, sizeof(gHost), gAdapterExt.hostname, sizeof(gAdapterExt.hostname));
        ClampedCopy(gIp, sizeof(gIp), gAdapterExt.sLocalIP, sizeof(gAdapterExt.sLocalIP));
        ClampedCopy(gFw, sizeof(gFw), gAdapterExt.fn_version, sizeof(gAdapterExt.fn_version));
    } else if (fuji_get_adapter_config(&ac)) {
        ClampedCopy(gSsid, sizeof(gSsid), ac.ssid, sizeof(ac.ssid));
        ClampedCopy(gHost, sizeof(gHost), ac.hostname, sizeof(ac.hostname));
        StrPrintF(gIp, "%d.%d.%d.%d", ac.localIP[0], ac.localIP[1], ac.localIP[2], ac.localIP[3]);
        ClampedCopy(gFw, sizeof(gFw), ac.fn_version, sizeof(ac.fn_version));
    } else {
        if (!quiet) {
            ShowError("Could not read adapter config", "");
        }
        fuji_palmos_close();
        StrCopy(gConnLine, "No answer from FujiNet");
        return false;
    }

    gHostSlotsValid = fuji_get_host_slots(gHostSlots, HOST_SLOT_COUNT);
    StrCopy(gConnLine, "Connected");
    return true;
}

static Boolean RequireConnected(void)
{
    if (!fuji_palmos_is_open()) {
        ShowInfo("Not connected to FujiNet. Check Settings.");
        return false;
    }
    return true;
}

/* ------------------------------------------------------------------ */
/* Home: host slots                                                     */
/* ------------------------------------------------------------------ */

static void HostsListDrawFunc(Int16 itemNum, RectangleType *bounds, Char **itemsText);

static void DrawHomeStatus(FormType *frm)
{
    char line[48];

    if (fuji_palmos_is_open() && gSsid[0] != '\0') {
        StrPrintF(line, "Connected via %s", gSsid);
    } else {
        StrCopy(line, gConnLine);
    }
    DrawTextLine(line, 4, 17, 152);
}

static void HomeRelist(FormType *frm)
{
    ListType *lst = (ListType *)GetObj(frm, MainHostsList);

    LstSetDrawFunction(lst, HostsListDrawFunc);
    LstSetListChoices(lst, 0, gHostSlotsValid ? HOST_SLOT_COUNT : 0);
    LstSetSelection(lst, noListSelection);
    LstDrawList(lst);
    DrawHomeStatus(frm);
}

static void OpenHostSlot(UInt16 slot)
{
    char name[32];

    ClampedCopy(name, sizeof(name), (char *)gHostSlots[slot], sizeof(gHostSlots[slot]));
    if (name[0] == '\0') {
        /* An empty slot: edit it instead. */
        gEditSlotIdx = slot;
        gEditBuf[0] = '\0';
        FrmPopupForm(HostEditDialog);
        return;
    }
    gBrowseHostSlot = (UInt8)slot;
    StrCopy(gBrowseHostValue, name);
    StrCopy(gBrowsePath, "/");
    FrmGotoForm(BrowseForm);
}

static Boolean MainFormHandleEvent(EventType *e)
{
    FormType *frm = FrmGetActiveForm();

    switch (e->eType) {
    case frmOpenEvent:
        FrmDrawForm(frm);
        if (!gTriedConnect && !fuji_palmos_is_open()) {
            gTriedConnect = true;
            StrCopy(gConnLine, "Connecting...");
            DrawHomeStatus(frm);
            ConnectLink(true);
        }
        HomeRelist(frm);
        return true;

    case frmUpdateEvent:
        FrmDrawForm(frm);
        HomeRelist(frm);
        return true;

    case lstSelectEvent:
        if (e->data.lstSelect.listID == MainHostsList && e->data.lstSelect.selection >= 0) {
            OpenHostSlot((UInt16)e->data.lstSelect.selection);
            return true;
        }
        return false;

    case ctlSelectEvent:
        if (e->data.ctlSelect.controlID == MainSettingsButton) {
            FrmGotoForm(SettingsForm);
            return true;
        }
        return false;

    case menuEvent:
        switch (e->data.menu.itemID) {
        case MenuEditHosts:
            if (RequireConnected()) {
                FrmGotoForm(HostsForm);
            }
            return true;
        case MenuSettings:
            FrmGotoForm(SettingsForm);
            return true;
        case MenuReconnect:
            StrCopy(gConnLine, "Connecting...");
            DrawHomeStatus(frm);
            ConnectLink(false);
            HomeRelist(frm);
            return true;
        default:
            return false;
        }

    default:
        return false;
    }
}

/* ------------------------------------------------------------------ */
/* Settings                                                             */
/* ------------------------------------------------------------------ */

static void DrawSettingsInfo(FormType *frm)
{
    RectangleType r;
    char line[96];

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

static Boolean SettingsFormHandleEvent(EventType *e)
{
    FormType *frm = FrmGetActiveForm();

    switch (e->eType) {
    case frmOpenEvent:
        FrmDrawForm(frm);
        SyncLinkPopups(frm);
        DrawSettingsInfo(frm);
        return true;

    case frmUpdateEvent:
        FrmDrawForm(frm);
        DrawSettingsInfo(frm);
        return true;

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
            gHostSlotsValid = false;
            ClearAdapterInfo();
            StrCopy(gConnLine, "Not connected");
            DrawSettingsInfo(frm);
            return false; /* let the default handler update the trigger label */
        }
        return false;

    case ctlSelectEvent:
        switch (e->data.ctlSelect.controlID) {
        case ConnectButton:
            StrCopy(gConnLine, "Connecting...");
            DrawSettingsInfo(frm);
            ConnectLink(false);
            DrawSettingsInfo(frm);
            return true;
        case WifiButton:
            if (RequireConnected()) {
                FrmGotoForm(WifiForm);
            }
            return true;
        case SettingsDoneButton:
            FrmGotoForm(MainForm);
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
            FrmGotoForm(SettingsForm);
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
        gHostSlotsValid = fuji_get_host_slots(gHostSlots, HOST_SLOT_COUNT);
        if (!gHostSlotsValid) {
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
            /* Opened from Home (an empty slot) or from Edit Host Slots. */
            FrmUpdateForm(FrmGetFormId(FrmGetActiveForm()), frmRedrawUpdateCode);
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

/* Browse entries in the BrowseGrid gadget, scrolled a row at a time,
 * either as a grid of icon-over-label cells or as a list of small icons
 * with full-width names. The choice is kept as unsaved preference
 * ('FjNt', BROWSE_VIEW_PREF_ID). */
#define BROWSE_VIEW_PREF_ID 4
static Boolean gBrowseAsList = false;

#define GRID_COLS    (gBrowseAsList ? 1 : 4)
#define GRID_CELL_W  (gBrowseAsList ? 152 : 38)
#define GRID_CELL_H  (gBrowseAsList ? 12 : 40)
#define GRID_ROWS    (gBrowseAsList ? 10 : 3)   /* rows visible */
#define GRID_LABEL_Y 25     /* grid: label offset within a cell */
#define LIST_LABEL_X 20     /* list: label offset, after the small icon */

static UInt16 gGridTop = 0;  /* first visible row */

static void LoadBrowseView(void)
{
    UInt16 v = 0, size = sizeof(v);

    if (PrefGetAppPreferences(FUJI_PALMOS_PREF_CREATOR, BROWSE_VIEW_PREF_ID, &v, &size, false) == 1
        && size == sizeof(v)) {
        gBrowseAsList = (Boolean)(v != 0);
    }
}

static void SaveBrowseView(void)
{
    UInt16 v = gBrowseAsList ? 1 : 0;

    PrefSetAppPreferences(FUJI_PALMOS_PREF_CREATOR, BROWSE_VIEW_PREF_ID, 1, &v, sizeof(v), false);
}

static UInt16 GridRowCount(void)
{
    return (UInt16)((gBrowseCount + GRID_COLS - 1) / GRID_COLS);
}

static UInt16 EntryIcon(const BrowseEntry *e)
{
    if (e->isDir) {
        return StrCompare(e->name, "..") == 0 ? IconUp : IconFolder;
    }
    if (HasExtCI(e->name, ".prc")) {
        return IconApp;
    }
    if (HasExtCI(e->name, ".pdb") || HasExtCI(e->name, ".pqa")) {
        return IconDb;
    }
    return IconFile;
}

static void GridBounds(FormType *frm, RectangleType *r)
{
    FrmGetObjectBounds(frm, FrmGetObjectIndex(frm, BrowseGrid), r);
}

/* The screen rectangle of entry `idx`, which must be on a visible row. */
static void GridCellRect(FormType *frm, UInt16 idx, RectangleType *r)
{
    RectangleType g;

    GridBounds(frm, &g);
    r->topLeft.x = (Coord)(g.topLeft.x + (idx % GRID_COLS) * GRID_CELL_W);
    r->topLeft.y = (Coord)(g.topLeft.y + (idx / GRID_COLS - gGridTop) * GRID_CELL_H);
    r->extent.x = GRID_CELL_W;
    r->extent.y = GRID_CELL_H;
}

/* Draws `text` in `width` at (x, y), centred or left-aligned, cut short
 * with an ellipsis if it doesn't fit. */
static void DrawFitLabel(const char *text, Coord x, Coord y, Coord width, Boolean centre)
{
    char buf[BROWSE_NAME_CAP + 2];
    Int16 w = width, len = (Int16)StrLen(text);
    Boolean fits;
    Char ell;

    FntCharsInWidth(text, &w, &len, &fits);
    if (fits) {
        WinDrawChars(text, len, centre ? (Coord)(x + (width - w) / 2) : x, y);
        return;
    }
    ChrHorizEllipsis(&ell);
    w = (Int16)(width - FntCharWidth(ell));
    len = (Int16)StrLen(text);
    FntCharsInWidth(text, &w, &len, &fits);
    MemMove(buf, (void *)text, len);
    buf[len] = ell;
    len++;
    w = FntCharsWidth(buf, len);
    WinDrawChars(buf, len, centre ? (Coord)(x + (width - w) / 2) : x, y);
}

static void GridDrawCell(FormType *frm, UInt16 idx, const BrowseEntry *e)
{
    RectangleType r;
    MemHandle h;
    const char *label = e->name;

    GridCellRect(frm, idx, &r);
    WinEraseRectangle(&r, 0);
    /* Small icons are the next bitmap ID after the large ones. */
    h = DmGetResource(bitmapRsc, (UInt16)(EntryIcon(e) + (gBrowseAsList ? 1 : 0)));
    if (h) {
        if (gBrowseAsList) {
            WinDrawBitmap((BitmapPtr)MemHandleLock(h), (Coord)(r.topLeft.x + 2), (Coord)(r.topLeft.y + 1));
        } else {
            WinDrawBitmap((BitmapPtr)MemHandleLock(h), (Coord)(r.topLeft.x + (GRID_CELL_W - 32) / 2),
                          (Coord)(r.topLeft.y + 1));
        }
        MemHandleUnlock(h);
        DmReleaseResource(h);
    }
    if (StrCompare(label, "..") == 0) {
        label = "Up";
    }
    if (gBrowseAsList) {
        DrawFitLabel(label, (Coord)(r.topLeft.x + LIST_LABEL_X), r.topLeft.y,
                     GRID_CELL_W - LIST_LABEL_X - 2, false);
    } else {
        DrawFitLabel(label, r.topLeft.x, (Coord)(r.topLeft.y + GRID_LABEL_Y), GRID_CELL_W - 2, true);
    }
}

static void GridDraw(FormType *frm)
{
    RectangleType g;
    const BrowseEntry *entries;
    UInt16 idx, rows = GridRowCount();
    UInt16 maxTop = rows > GRID_ROWS ? (UInt16)(rows - GRID_ROWS) : 0;

    if (gGridTop > maxTop) {
        gGridTop = maxTop;
    }
    GridBounds(frm, &g);
    WinEraseRectangle(&g, 0);
    if (gBrowseEntriesH) {
        entries = (const BrowseEntry *)MemHandleLock(gBrowseEntriesH);
        for (idx = (UInt16)(gGridTop * GRID_COLS);
             idx < gBrowseCount && idx < (gGridTop + GRID_ROWS) * GRID_COLS; idx++) {
            GridDrawCell(frm, idx, &entries[idx]);
        }
        MemHandleUnlock(gBrowseEntriesH);
    }
    SclSetScrollBar((ScrollBarType *)GetObj(frm, BrowseScrollBar), (Int16)gGridTop, 0,
                    (Int16)maxTop, GRID_ROWS);
}

static void GridScroll(FormType *frm, Int16 rows)
{
    Int16 top = (Int16)gGridTop + rows;

    gGridTop = (UInt16)(top < 0 ? 0 : top);
    GridDraw(frm);
}

/* Handles a pen-down in the grid: highlights the cell while the pen is
 * down, and returns the entry index if it comes up inside the same cell,
 * else -1. */
static Int16 GridTrackTap(FormType *frm, Coord x, Coord y)
{
    RectangleType g, r;
    Int16 px, py;
    Boolean down = true, inside = true, shown = true;
    UInt16 idx;

    GridBounds(frm, &g);
    if (!RctPtInRectangle(x, y, &g)) {
        return -1;
    }
    idx = (UInt16)((gGridTop + (y - g.topLeft.y) / GRID_CELL_H) * GRID_COLS
                   + (x - g.topLeft.x) / GRID_CELL_W);
    if ((x - g.topLeft.x) / GRID_CELL_W >= GRID_COLS || idx >= gBrowseCount) {
        return -1;
    }
    GridCellRect(frm, idx, &r);
    WinInvertRectangle(&r, 0);
    while (down) {
        EvtGetPen(&px, &py, &down);
        inside = RctPtInRectangle(px, py, &r);
        if (inside != shown) {
            WinInvertRectangle(&r, 0);
            shown = inside;
        }
    }
    if (shown) {
        WinInvertRectangle(&r, 0);
    }
    return inside ? (Int16)idx : -1;
}

static void BrowseRelist(FormType *frm)
{
    /* FrmSetTitle keeps a pointer, so the buffer must outlive the form.
     * FrmCopyTitle can't be used: it overwrites the resource's title in
     * place and a path longer than "Browse" tramples the object after it. */
    StrPrintF(gBrowseTitle, "%s:", gBrowseHostValue);
    StrNCat(gBrowseTitle, gBrowsePath, sizeof(gBrowseTitle));
    FrmSetTitle(frm, gBrowseTitle);

    ListDirectory(gBrowseHostSlot, gBrowsePath);
    gGridTop = 0;
    GridDraw(frm);
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
    FormType *frm = FrmGetActiveForm();
    Int16 idx;

    switch (e->eType) {
    case frmOpenEvent:
        LoadBrowseView();
        FrmSetControlGroupSelection(frm, BrowseViewGroup, gBrowseAsList ? BrowseListButton : BrowseGridButton);
        FrmDrawForm(frm);
        BrowseRelist(frm);
        return true;

    case frmUpdateEvent:
        FrmDrawForm(frm);
        GridDraw(frm);
        return true;

    case frmCloseEvent:
        FreeBrowseEntries();
        return false;

    case ctlSelectEvent:
        switch (e->data.ctlSelect.controlID) {
        case BrowseDoneButton:
            FrmGotoForm(MainForm);
            return true;
        case BrowseGridButton:
        case BrowseListButton:
            if (gBrowseAsList != (e->data.ctlSelect.controlID == BrowseListButton)) {
                /* Keep the same entries in view. */
                UInt16 first = (UInt16)(gGridTop * GRID_COLS);

                gBrowseAsList = (Boolean)(e->data.ctlSelect.controlID == BrowseListButton);
                gGridTop = (UInt16)(first / GRID_COLS);
                SaveBrowseView();
                GridDraw(frm);
            }
            return true;
        default:
            return false;
        }

    case penDownEvent:
        idx = GridTrackTap(frm, e->screenX, e->screenY);
        if (idx >= 0) {
            BrowseEntryTap(frm, idx);
            return true;
        }
        return false;

    case keyDownEvent:
        if (e->data.keyDown.chr == pageUpChr) {
            GridScroll(frm, -GRID_ROWS);
            return true;
        }
        if (e->data.keyDown.chr == pageDownChr) {
            GridScroll(frm, GRID_ROWS);
            return true;
        }
        return false;

    case sclRepeatEvent:
        gGridTop = (UInt16)e->data.sclRepeat.newValue;
        GridDraw(frm);
        return false;   /* keep the scroll bar repeating */

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

/* Download size estimate for the progress bar, from the database header
 * as it arrives: a .prc/.pdb header (78 bytes) is followed by its
 * resource (10-byte) or record (8-byte) entries, each holding the offset
 * where its data starts, so the largest offset is nearly the file size. */
#define EST_BUF_LEN 512
static UInt8 gEstBuf[EST_BUF_LEN];
static UInt16 gEstLen;
static UInt32 gInstallEstimate;   /* 0 until known */
static Boolean gInstallDone;

static UInt32 Be32(const UInt8 *p)
{
    return ((UInt32)p[0] << 24) | ((UInt32)p[1] << 16) | ((UInt32)p[2] << 8) | p[3];
}

static void EstimateFromHeader(const UInt8 *data, UInt32 n)
{
    UInt16 count, entry, offAt, i, fit;
    UInt32 maxOff = 0;

    if (gInstallEstimate != 0 || gEstLen >= EST_BUF_LEN) {
        return;
    }
    if (n > (UInt32)(EST_BUF_LEN - gEstLen)) {
        n = EST_BUF_LEN - gEstLen;
    }
    MemMove(gEstBuf + gEstLen, (void *)data, n);
    gEstLen = (UInt16)(gEstLen + n);
    if (gEstLen < 78) {
        return;
    }
    count = (UInt16)((gEstBuf[76] << 8) | gEstBuf[77]);
    entry = (gEstBuf[33] & dmHdrAttrResDB) ? 10 : 8;
    offAt = (entry == 10) ? 6 : 0;
    fit = (UInt16)((gEstLen - 78) / entry);
    if (fit < count && gEstLen < EST_BUF_LEN) {
        return;   /* wait for the whole table, or as much as fits */
    }
    for (i = 0; i < count && i < fit; i++) {
        UInt32 off = Be32(gEstBuf + 78 + i * entry + offAt);

        if (off > maxOff) {
            maxOff = off;
        }
    }
    gInstallEstimate = maxOff > 78 ? maxOff : 78;
}

static void DrawInstallStatus(FormType *frm)
{
    RectangleType r;
    char line[40];
    UInt32 pct = 0;

    if (!frm) {
        return;
    }

    r.topLeft.x = 4;
    r.topLeft.y = 20;
    r.extent.x = 152;
    r.extent.y = 50;
    WinEraseRectangle(&r, 0);

    WinDrawChars(gInstallStatusLine, StrLen(gInstallStatusLine), 4, 20);
    if (gInstallEstimate != 0) {
        StrPrintF(line, "%ld of about %ld bytes", (Int32)gInstallTotalBytes, (Int32)gInstallEstimate);
        pct = gInstallDone ? 100 : (gInstallTotalBytes * 100) / gInstallEstimate;
        if (pct > 99 && !gInstallDone) {
            pct = 99;
        }
    } else {
        StrPrintF(line, "%ld bytes", (Int32)gInstallTotalBytes);
        pct = gInstallDone ? 100 : 0;
    }
    WinDrawChars(line, StrLen(line), 4, 34);

    /* Progress bar. */
    r.topLeft.x = 8;
    r.topLeft.y = 52;
    r.extent.x = 144;
    r.extent.y = 10;
    WinDrawRectangleFrame(simpleFrame, &r);
    r.extent.x = (Coord)((144 * pct) / 100);
    if (r.extent.x > 0) {
        WinDrawRectangle(&r, 0);
    }
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
    EstimateFromHeader((const UInt8 *)dataP, (UInt32)n);
    gInstallTotalBytes += (UInt32)n;
    EvtResetAutoOffTimer();
    DrawInstallStatus(FrmGetActiveForm());
    return errNone;
}

/* Run of an app that is already installed: its database, found by
 * InstallDeleteProc instead of replacing it. */
static LocalID gRunExistingID;

static Boolean InstallDeleteProc(const char *nameP, UInt16 version, UInt16 cardNo, LocalID dbID, void *userDataP)
{
    UInt16 curCard;
    LocalID curID;

    if (gInstallRun) {
        /* Keep the installed copy: Run launches it instead of replacing it
         * with one that would be deleted afterwards. */
        gRunExistingID = dbID;
        return false;
    }
    if (SysCurAppDatabase(&curCard, &curID) == errNone && curCard == cardNo && curID == dbID) {
        return false; /* refuse to delete ourselves */
    }
    DmDeleteDatabase(cardNo, dbID);
    return true;
}

/* Deletes the app recorded by RunTemporarily, if it is still there. */
static void DeleteTempApp(void)
{
    TempAppPref pref;
    UInt16 size = sizeof(pref);
    UInt32 creator = 0;
    LocalID id;

    if (PrefGetAppPreferences(FUJI_PALMOS_PREF_CREATOR, TEMP_APP_PREF_ID, &pref, &size, false)
            != TEMP_APP_PREF_VERSION || size != sizeof(pref)) {
        return;
    }
    pref.name[sizeof(pref.name) - 1] = '\0';
    id = DmFindDatabase(0, pref.name);
    if (id != 0 && DmDatabaseInfo(0, id, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, &creator) == errNone
        && creator == pref.creator) {
        DmDeleteDatabase(0, id);
    }
    PrefSetAppPreferences(FUJI_PALMOS_PREF_CREATOR, TEMP_APP_PREF_ID, TEMP_APP_PREF_VERSION,
                          NULL, 0, false);
}

/* Run: starts the just-installed app nested inside FujiConfig
 * (SysAppLaunch rather than SysUIAppSwitch), so control comes back here
 * when it exits and the app can be deleted straight away. Its own data
 * and preferences are left alone. Leaving the app (Home, another app)
 * stops it; FujiConfig then deletes it and quits too, so the switch the
 * user asked for goes ahead. */
static void RunTemporarily(UInt16 cardNo, LocalID dbID)
{
    TempAppPref pref;
    UInt32 type = 0, result = 0;
    EventType stop;

    MemSet(&pref, sizeof(pref), 0);
    if (DmDatabaseInfo(cardNo, dbID, pref.name, 0, 0, 0, 0, 0, 0, 0, 0, &type, &pref.creator) != errNone
        || type != sysFileTApplication) {
        return;
    }
    PrefSetAppPreferences(FUJI_PALMOS_PREF_CREATOR, TEMP_APP_PREF_ID, TEMP_APP_PREF_VERSION,
                          &pref, sizeof(pref), false);

    /* The app may use the link itself, and needs the memory. */
    if (fuji_palmos_is_open()) {
        fuji_palmos_close();
    }
    FreeBrowseEntries();
    /* Close our forms while our globals are current: the app's first
     * FrmGotoForm would otherwise close them and call our handlers with
     * its globals in place. */
    FrmCloseAllForms();

    SysAppLaunch(cardNo, dbID, sysAppLaunchFlagNewGlobals, sysAppLaunchCmdNormalLaunch, NULL, &result);

    DeleteTempApp();

    /* The app normally stops on an appStopEvent that was meant for the
     * whole UI app (Home, or another app); quit as well. */
    MemSet(&stop, sizeof(stop), 0);
    stop.eType = appStopEvent;
    EvtAddEventToQueue(&stop);
}

static void RunInstall(FormType *frm)
{
    LocalID dbID = 0;
    UInt16 cardNo = 0;
    Boolean needReset = false;
    Err err;
    FN_ERR nrc;

    gInstallTotalBytes = 0;
    gEstLen = 0;
    gInstallEstimate = 0;
    gInstallDone = false;
    StrCopy(gInstallStatusLine, gInstallRun ? "Downloading..." : "Installing...");
    BuildDeviceSpec(gBrowseHostValue, gInstallPath, gInstallDeviceSpec, sizeof(gInstallDeviceSpec));
    DrawInstallStatus(frm);

    nrc = network_open(gInstallDeviceSpec, OPEN_MODE_READ, OPEN_TRANS_NONE);
    if (nrc != FN_ERR_OK) {
        ShowError("Install failed", "open");
        StrCopy(gInstallStatusLine, "Failed.");
        DrawInstallStatus(frm);
        return;
    }

    gRunExistingID = 0;
    err = ExgDBRead(InstallReadProc, InstallDeleteProc, 0, &dbID, cardNo, &needReset, true);
    network_close(gInstallDeviceSpec);

    if (gInstallRun && gRunExistingID != 0) {
        /* Already installed: just open it, as a normal (permanent) app. */
        if (fuji_palmos_is_open()) {
            fuji_palmos_close();
        }
        SysUIAppSwitch(0, gRunExistingID, sysAppLaunchCmdNormalLaunch, 0);
        return;
    }
    if (err != errNone) {
        ShowError("Install failed", "");
        StrCopy(gInstallStatusLine, "Failed.");
        DrawInstallStatus(frm);
        return;
    }

    gInstallDone = true;
    if (gInstallRun) {
        StrCopy(gInstallStatusLine, "Launching...");
    } else {
        StrCopy(gInstallStatusLine, needReset ? "Installed (reset may be needed)." : "Installed.");
    }
    DrawInstallStatus(frm);

    if (gInstallRun) {
        RunTemporarily(cardNo, dbID);
    }
}

static Boolean InstallFormHandleEvent(EventType *e)
{
    switch (e->eType) {
    case frmOpenEvent: {
        FormType *frm = FrmGetActiveForm();

        /* A literal: FrmSetTitle keeps the pointer. */
        FrmSetTitle(frm, gInstallRun ? "Launching" : "Installing");
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
        case SettingsForm:
            FrmSetEventHandler(frm, SettingsFormHandleEvent);
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

/* An app that used the NetLib shim may have exited without closing it,
 * leaving the serial port held; take it back before using the link. */
static void ReleaseNetShim(void)
{
    UInt16 ref;

    if (SysLibFind(fnNLLibName, &ref) == errNone) {
        FnNLDisconnect(ref);
    }
}

UInt32 PilotMain(UInt16 cmd, MemPtr cmdPBP, UInt16 launchFlags)
{
    EventType e;
    UInt16 err;

    if (cmd != sysAppLaunchCmdNormalLaunch) {
        return 0;
    }

    ReleaseNetShim();
    DeleteTempApp();   /* left over if a reset interrupted a Run */
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
