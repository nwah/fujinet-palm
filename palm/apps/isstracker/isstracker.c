/* palm/apps/isstracker/isstracker.c
 *
 * ISS Tracker -- shows the International Space Station's current position
 * on a small world map, refetched every 30 seconds, plus a "Crew" list of
 * everyone currently in space. Targets Palm OS 3.1 / DragonBall EZ
 * (Handspring Visor Deluxe), 160x160 mono/4-gray.
 *
 * Built on fujinet-lib-palmos (the standard FujiNet client library), the
 * same way ../fujiconfig does: network_open/network_json_parse/
 * network_json_query/network_close (see fujinet-network.h), wrapped by
 * ../common/fnapp.c's small fnapp_json_* helpers. Like fujiconfig, this
 * app never calls fuji_palmos_open() itself -- the first fuji_bus_call()
 * opens the link lazily from FujiConfig's saved preference (see
 * fujinet-palmos.h) -- but it DOES call fuji_palmos_close() on
 * appStopEvent so HotSync etc. can use the port afterward.
 *
 * Header order matters: fujinet-int.h (pulled in via fnapp.h) needs the
 * compiler's stdbool.h seen before PalmOS.h's PalmTypes.h defines its own
 * true/false, so every fujinet header comes before PalmOS.h (see
 * fujiconfig.c's own header comment, and fnapp.h which enforces this
 * ordering for the headers it pulls in).
 *
 * Single top-level event loop, same shape as fujiconfig.c/mastodon.c: no
 * FrmDoDialog anywhere.
 */
#include "fujinet-network.h"
#include "fujinet-palmos.h"
#include <PalmOS.h>
#include "fnapp.h"
#include "isstracker_rsc.h"

#define NUMBUF_CAP 16

/* ------------------------------------------------------------------ */
/* Endpoints (confirmed live via curl 2026-09-28)                      */
/* ------------------------------------------------------------------ */

#define ISS_URL    "N:http://api.open-notify.org/iss-now.json"
#define Q_TS       "/timestamp"
#define Q_LAT      "/iss_position/latitude"
#define Q_LON      "/iss_position/longitude"

#define ASTROS_URL "N:http://api.open-notify.org/astros.json"
#define MAX_PEOPLE 16

#define REFRESH_INTERVAL_SEC 30uL

/* ------------------------------------------------------------------ */
/* Position / map state                                                */
/* ------------------------------------------------------------------ */

#define MAP_X 0
#define MAP_Y 16
#define MAP_W 160
#define MAP_H 80

static FixedPt gLat, gLon;
static UInt32  gTimestamp;
static Boolean gHavePos = false;

static Coord gCurX, gCurY; /* current marker, screen coords */

#define TRAIL_LEN 5
static Coord  gTrailX[TRAIL_LEN];
static Coord  gTrailY[TRAIL_LEN];
static UInt8  gTrailCount = 0;

static char gLatLonLine[48];
static char gTimeLine[24];
static char gStatusLine[48] = "";

static UInt32 gNextFetchTicks;

/* ------------------------------------------------------------------ */
/* Crew state (static globals, not stack -- see FetchCrew)             */
/* ------------------------------------------------------------------ */

static char gCrewNames[MAX_PEOPLE][32];
static char gCrewCrafts[MAX_PEOPLE][24];
static Boolean gCrewUsed[MAX_PEOPLE];
static char gCrewText[1600];

/* ------------------------------------------------------------------ */
/* Small generic helpers (same shapes as fujiconfig.c)                 */
/* ------------------------------------------------------------------ */

static void *GetObj(FormType *frm, UInt16 id)
{
    return FrmGetObjectPtr(frm, FrmGetObjectIndex(frm, id));
}

static void ShowInfo(const char *msg)
{
    FrmCustomAlert(InfoAlert, msg, "", "");
}

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

static void AppendPad2(char *dst, Int16 v)
{
    char num[6];

    if (v < 0) {
        v = 0;
    }
    if (v < 10) {
        StrCat(dst, "0");
    }
    StrIToA(num, (Int32)v);
    StrCat(dst, num);
}

/* ------------------------------------------------------------------ */
/* ISS position fetch                                                  */
/* ------------------------------------------------------------------ */

static Boolean FetchIss(const char **step)
{
    char buf[24];
    Int16 n;
    FN_ERR rc;

    rc = fnapp_json_open(ISS_URL);
    if (rc != FN_ERR_OK) {
        *step = "open";
        return false;
    }

    n = fnapp_json_query(ISS_URL, Q_TS, buf, sizeof(buf));
    if (n < 0) {
        *step = "timestamp";
        fnapp_json_close(ISS_URL);
        return false;
    }
    gTimestamp = (UInt32)fnapp_atol(buf);

    n = fnapp_json_query(ISS_URL, Q_LAT, buf, sizeof(buf));
    if (n < 0) {
        *step = "latitude";
        fnapp_json_close(ISS_URL);
        return false;
    }
    fnapp_parse_fixed(buf, &gLat);

    n = fnapp_json_query(ISS_URL, Q_LON, buf, sizeof(buf));
    if (n < 0) {
        *step = "longitude";
        fnapp_json_close(ISS_URL);
        return false;
    }
    fnapp_parse_fixed(buf, &gLon);

    fnapp_json_close(ISS_URL);
    return true;
}

/* Converts gLat/gLon (already parsed) into screen coordinates over the
 * map (drawn at MAP_X,MAP_Y, MAP_W x MAP_H) and pushes the previous
 * marker position into the trail ring. Int32 math throughout: on this
 * 16-bit-int target, (lonSigned+180)*159 can reach ~57000, well past a
 * 16-bit signed int's +/-32767 range. */
static void ComputeMapPosition(void)
{
    Int32 lonSigned = gLon.neg ? -(Int32)gLon.whole : (Int32)gLon.whole;
    Int32 latSigned = gLat.neg ? -(Int32)gLat.whole : (Int32)gLat.whole;
    Int32 mapX = ((lonSigned + 180L) * (Int32)(MAP_W - 1)) / 360L;
    Int32 mapY = ((90L - latSigned) * (Int32)(MAP_H - 1)) / 180L;

    if (mapX < 0) {
        mapX = 0;
    }
    if (mapX > MAP_W - 1) {
        mapX = MAP_W - 1;
    }
    if (mapY < 0) {
        mapY = 0;
    }
    if (mapY > MAP_H - 1) {
        mapY = MAP_H - 1;
    }

    if (gHavePos) {
        if (gTrailCount < TRAIL_LEN) {
            gTrailX[gTrailCount] = gCurX;
            gTrailY[gTrailCount] = gCurY;
            gTrailCount++;
        } else {
            UInt8 i;

            for (i = 1; i < TRAIL_LEN; i++) {
                gTrailX[i - 1] = gTrailX[i];
                gTrailY[i - 1] = gTrailY[i];
            }
            gTrailX[TRAIL_LEN - 1] = gCurX;
            gTrailY[TRAIL_LEN - 1] = gCurY;
        }
    }

    gCurX = MAP_X + (Coord)mapX;
    gCurY = MAP_Y + (Coord)mapY;
    gHavePos = true;
}

/* ------------------------------------------------------------------ */
/* Drawing                                                             */
/* ------------------------------------------------------------------ */

static void DrawMap(void)
{
    MemHandle h;
    UInt8 i;
    RectangleType r;

    h = DmGetResource(bitmapRsc, MapBitmap);
    if (h) {
        BitmapType *bmp = (BitmapType *)MemHandleLock(h);

        WinDrawBitmap(bmp, MAP_X, MAP_Y);
        MemHandleUnlock(h);
        DmReleaseResource(h);
    }

    for (i = 0; i < gTrailCount; i++) {
        WinDrawLine(gTrailX[i], gTrailY[i], gTrailX[i], gTrailY[i]);
    }

    if (gHavePos) {
        r.topLeft.x = (Coord)(gCurX - 3);
        r.topLeft.y = (Coord)(gCurY - 3);
        r.extent.x = 7;
        r.extent.y = 7;
        WinEraseRectangle(&r, 0);

        WinDrawLine((Coord)(gCurX - 3), (Coord)(gCurY - 3), (Coord)(gCurX + 3), (Coord)(gCurY + 3));
        WinDrawLine((Coord)(gCurX - 3), (Coord)(gCurY + 3), (Coord)(gCurX + 3), (Coord)(gCurY - 3));
        WinDrawLine((Coord)(gCurX - 3), gCurY, (Coord)(gCurX + 3), gCurY);
        WinDrawLine(gCurX, (Coord)(gCurY - 3), gCurX, (Coord)(gCurY + 3));
    }
}

/* Lat/lon + UTC time lines -- only meaningful once a fetch has ever
 * succeeded (gHavePos), since they read gLat/gLon/gTimestamp. */
static void DrawPosition(void)
{
    char latStr[10];
    char lonStr[10];
    FixedPt tmp;
    UInt16 hh, mm, ss;

    tmp = gLat;
    tmp.neg = false;
    fnapp_format_fixed(latStr, sizeof(latStr), &tmp);
    tmp = gLon;
    tmp.neg = false;
    fnapp_format_fixed(lonStr, sizeof(lonStr), &tmp);

    StrPrintF(gLatLonLine, "Lat %s %s  Lon %s %s",
              latStr, gLat.neg ? "S" : "N", lonStr, gLon.neg ? "W" : "E");
    DrawTextLine(gLatLonLine, 4, 98, 152);

    fnapp_epoch_to_hms_utc(gTimestamp, &hh, &mm, &ss);
    StrPrintF(gTimeLine, "");
    AppendPad2(gTimeLine, (Int16)hh);
    StrCat(gTimeLine, ":");
    AppendPad2(gTimeLine, (Int16)mm);
    StrCat(gTimeLine, ":");
    AppendPad2(gTimeLine, (Int16)ss);
    StrCat(gTimeLine, " UTC");
    DrawTextLine(gTimeLine, 4, 110, 152);
}

/* "Updated HH:MM" or "Error: ..." -- always safe to draw, even before the
 * first successful fetch, so a failure is visible on a fresh launch too. */
static void DrawStatus(void)
{
    DateTimeType dt;

    if (gStatusLine[0] == '\0') {
        TimSecondsToDateTime(TimGetSeconds(), &dt);
        StrCopy(gStatusLine, "Updated ");
        AppendPad2(gStatusLine, dt.hour);
        StrCat(gStatusLine, ":");
        AppendPad2(gStatusLine, dt.minute);
    }
    DrawTextLine(gStatusLine, 4, 122, 152);
}

/* ------------------------------------------------------------------ */
/* Refresh                                                             */
/* ------------------------------------------------------------------ */

static void ScheduleNextFetch(void)
{
    gNextFetchTicks = TimGetTicks() + (SysTicksPerSecond() * REFRESH_INTERVAL_SEC);
}

static void DoRefresh(FormType *frm)
{
    const char *step = "";
    Boolean ok;

    ok = FetchIss(&step);

    if (ok) {
        ComputeMapPosition();
        gStatusLine[0] = '\0'; /* DrawStatus() will fill in "Updated HH:MM" */
    } else {
        char num[NUMBUF_CAP];

        StrCopy(gStatusLine, "Error: ");
        StrCat(gStatusLine, step);
        StrCat(gStatusLine, " (0x");
        StrIToH(num, (UInt32)fuji_palmos_last_error());
        StrCat(gStatusLine, num + 4);
        StrCat(gStatusLine, ")");
    }

    if (frm != 0) {
        DrawMap();
        if (gHavePos) {
            DrawPosition();
        }
        DrawStatus();
    }
    ScheduleNextFetch();
}

/* ------------------------------------------------------------------ */
/* Crew                                                                 */
/* ------------------------------------------------------------------ */

static Boolean FetchCrew(void)
{
    char buf[8];
    char path[32];
    Int16 count, i, j;
    Int16 n;
    FN_ERR rc;

    rc = fnapp_json_open(ASTROS_URL);
    if (rc != FN_ERR_OK) {
        return false;
    }

    n = fnapp_json_query(ASTROS_URL, "/number", buf, sizeof(buf));
    if (n < 0) {
        fnapp_json_close(ASTROS_URL);
        return false;
    }
    count = (Int16)fnapp_atol(buf);
    if (count > MAX_PEOPLE) {
        count = MAX_PEOPLE;
    }
    if (count < 0) {
        count = 0;
    }

    for (i = 0; i < count; i++) {
        StrPrintF(path, "/people/%d/name", i);
        fnapp_json_query(ASTROS_URL, path, gCrewNames[i], sizeof(gCrewNames[i]));
        StrPrintF(path, "/people/%d/craft", i);
        fnapp_json_query(ASTROS_URL, path, gCrewCrafts[i], sizeof(gCrewCrafts[i]));
        gCrewUsed[i] = false;
    }
    fnapp_json_close(ASTROS_URL);

    gCrewText[0] = '\0';
    StrPrintF(gCrewText, "%d people in space\n\n", count);

    for (i = 0; i < count; i++) {
        if (gCrewUsed[i]) {
            continue;
        }
        StrCat(gCrewText, gCrewCrafts[i]);
        StrCat(gCrewText, ":\n");
        for (j = i; j < count; j++) {
            if (!gCrewUsed[j] && StrCompare(gCrewCrafts[j], gCrewCrafts[i]) == 0) {
                StrCat(gCrewText, "  ");
                StrCat(gCrewText, gCrewNames[j]);
                StrCat(gCrewText, "\n");
                gCrewUsed[j] = true;
            }
        }
        StrCat(gCrewText, "\n");
    }

    return true;
}

/* ------------------------------------------------------------------ */
/* MainForm                                                             */
/* ------------------------------------------------------------------ */

static Boolean MainFormHandleEvent(EventType *e)
{
    switch (e->eType) {
    case frmOpenEvent: {
        FormType *frm = FrmGetActiveForm();

        FrmDrawForm(frm);
        DoRefresh(frm);
        return true;
    }

    case frmUpdateEvent: {
        FormType *frm = FrmGetActiveForm();

        FrmDrawForm(frm);
        DrawMap();
        if (gHavePos) {
            DrawPosition();
        }
        DrawStatus();
        return true;
    }

    case ctlSelectEvent:
        switch (e->data.ctlSelect.controlID) {
        case RefreshButton:
            DoRefresh(FrmGetActiveForm());
            return true;
        case CrewButton:
            EvtResetAutoOffTimer();
            if (FetchCrew()) {
                FrmGotoForm(CrewForm);
            } else {
                ShowInfo("Could not load the crew list.");
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
/* CrewForm                                                             */
/* ------------------------------------------------------------------ */

static FieldType *GetCrewField(FormType *frm)
{
    return (FieldType *)GetObj(frm, CrewField);
}

static ScrollBarType *GetCrewScrollBar(FormType *frm)
{
    return (ScrollBarType *)GetObj(frm, CrewScrollBar);
}

static void UpdateCrewScrollBar(FormType *frm)
{
    FieldType *fld = GetCrewField(frm);
    ScrollBarType *bar = GetCrewScrollBar(frm);
    UInt16 scrollPos, textHeight, fieldHeight;
    Int16 maxVal;

    FldGetScrollValues(fld, &scrollPos, &textHeight, &fieldHeight);
    maxVal = (textHeight > fieldHeight) ? (Int16)(textHeight - fieldHeight) : 0;
    SclSetScrollBar(bar, (Int16)scrollPos, 0, maxVal, (Int16)fieldHeight);
}

static Boolean CrewFormHandleEvent(EventType *e)
{
    switch (e->eType) {
    case frmOpenEvent: {
        FormType *frm = FrmGetActiveForm();
        FieldType *fld = GetCrewField(frm);

        FrmDrawForm(frm);
        FldSetTextPtr(fld, gCrewText);
        FldSetScrollPosition(fld, 0);
        FldRecalculateField(fld, true);
        UpdateCrewScrollBar(frm);
        return true;
    }

    case frmCloseEvent: {
        FormType *frm = FrmGetActiveForm();
        FieldType *fld = GetCrewField(frm);

        FldSetTextPtr(fld, 0);
        return false;
    }

    case ctlSelectEvent:
        if (e->data.ctlSelect.controlID == CrewDoneButton) {
            FrmGotoForm(MainForm);
            return true;
        }
        return false;

    case sclRepeatEvent: {
        FormType *frm = FrmGetActiveForm();
        FieldType *fld = GetCrewField(frm);
        Int16 newVal = e->data.sclRepeat.newValue;
        Int16 curVal = e->data.sclRepeat.value;
        Int16 delta = (Int16)(newVal - curVal);

        if (delta > 0) {
            FldScrollField(fld, (UInt16)delta, winDown);
        } else if (delta < 0) {
            FldScrollField(fld, (UInt16)(-delta), winUp);
        }
        return true;
    }

    case keyDownEvent:
        if (e->data.keyDown.chr == vchrPageUp || e->data.keyDown.chr == vchrPageDown) {
            FormType *frm = FrmGetActiveForm();
            FieldType *fld = GetCrewField(frm);
            WinDirectionType dir = (e->data.keyDown.chr == vchrPageUp) ? winUp : winDown;
            UInt16 lines = FldGetVisibleLines(fld);

            if (lines == 0) {
                lines = 1;
            }
            if (FldScrollable(fld, dir)) {
                FldScrollField(fld, lines, dir);
                UpdateCrewScrollBar(frm);
            }
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
        case CrewForm:
            FrmSetEventHandler(frm, CrewFormHandleEvent);
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

    FrmGotoForm(MainForm);

    do {
        UInt32 now = TimGetTicks();
        Int32 timeout;

        if (now >= gNextFetchTicks) {
            timeout = 0;
        } else {
            timeout = (Int32)(gNextFetchTicks - now);
        }

        EvtGetEvent(&e, timeout);

        if (e.eType == nilEvent) {
            if (TimGetTicks() >= gNextFetchTicks) {
                FormType *frm = FrmGetActiveForm();

                if (frm != 0 && FrmGetActiveFormID() == MainForm) {
                    DoRefresh(frm);
                } else {
                    ScheduleNextFetch();
                }
            }
            continue;
        }

        if (SysHandleEvent(&e)) continue;
        if (MenuHandleEvent(0, &e, &err)) continue;
        if (AppHandleEvent(&e)) continue;
        FrmDispatchEvent(&e);
    } while (e.eType != appStopEvent);

    if (fuji_palmos_is_open()) {
        fuji_palmos_close();
    }

    FrmCloseAllForms();
    return 0;
}
