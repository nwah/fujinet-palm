/* palm/apps/mastodon/mastodon.c
 *
 * Mastodon -- shows the latest public post from oldbytes.space, refetched
 * every 60 seconds, over the FujiNet Palm OS RS232 transport. Port of
 * fujinet-apps/mastodon/c64 (main.c/fetch_post.c/show_post.c) to Palm OS
 * 3.1 / DragonBall EZ (Handspring Visor Deluxe), 160x160 mono.
 *
 * Networking is fujinet-lib (network_open/network_json_parse/
 * network_json_query_n/network_close, via ../common/fnapp.c), like the
 * Weather and News apps. The link opens lazily on the first call, from the
 * "FujiNet link" preference FujiConfig saves, and is closed on
 * appStopEvent. Each fetch opens and closes the network unit around the
 * JSON queries.
 *
 * HTML/entity/UTF-8 cleanup of the fetched text lives in htmlclean.c/.h --
 * a separate, portable, Palm-free module with its own host-side unit test
 * (test_htmlclean.c). See that file for exactly what it does.
 */
/* fujinet headers before PalmOS.h, see ../common/fnapp.h */
#include "fujinet-network.h"
#include "fujinet-palmos.h"
#include <PalmOS.h>
#include "fnapp.h"
#include "mastodon_rsc.h"
#include "htmlclean.h"

/* ------------------------------------------------------------------ */
/* Config                                                              */
/* ------------------------------------------------------------------ */

#define URL "N:https://oldbytes.space/api/v1/timelines/public?limit=1"
#define Q_DISPLAY_NAME "/0/account/display_name"
#define Q_CREATED_AT   "/0/created_at"
#define Q_CONTENT      "/0/content"
#define Q_REPLIES      "/0/replies_count"
#define Q_REBLOGS      "/0/reblogs_count"
/* NOTE: the field is "favourites_count" (British spelling) on the real
 * Mastodon API -- the c64 original's "favorites_count" query was simply
 * wrong and would always come back empty. */
#define Q_FAVOURITES   "/0/favourites_count"

#define FETCH_INTERVAL_TICKS_SEC 60uL

#define DISPLAY_NAME_CAP 80
#define CREATED_AT_CAP   32
#define CREATED_AT_DISP_CAP 20
#define CONTENT_CAP      2048
#define COUNTS_LINE_CAP  64
#define STATUS_LINE_CAP  48
#define NUMBUF_CAP       16

/* ------------------------------------------------------------------ */
/* Displayed post state (only overwritten on a fully successful fetch, */
/* so a failed refresh leaves the previous post on screen).            */
/* ------------------------------------------------------------------ */

static char  gDisplayName[DISPLAY_NAME_CAP];
static char  gCreatedAtDisp[CREATED_AT_DISP_CAP]; /* "YYYY-MM-DD HH:MM" */
static char  gContent[CONTENT_CAP];
static Int32 gReplies;
static Int32 gReblogs;
static Int32 gFavourites;

static char  gCountsLine[COUNTS_LINE_CAP];
static char  gStatusLine[STATUS_LINE_CAP];

/* Scratch used while a fetch is in flight; only copied into the above on
 * complete success, per FetchPost()'s contract. */
typedef struct {
    char  display_name[DISPLAY_NAME_CAP];
    char  created_at[CREATED_AT_CAP];
    char  content[CONTENT_CAP];
    Int32 replies;
    Int32 reblogs;
    Int32 favourites;
} PostScratch;

static PostScratch gScratch;

/* Which step of FetchPost() failed, for the "Error: <step> err=<n>"
 * status line -- always a pointer to a string literal, never freed. */
static const char *gFailStep;

static UInt32 gNextFetchTicks;

/* ------------------------------------------------------------------ */
/* Small helpers                                                       */
/* ------------------------------------------------------------------ */

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

/* "YYYY-MM-DDTHH:MM:SS.sssZ" -> "YYYY-MM-DD HH:MM" by slicing the ISO
 * string (already UTC, per the API) -- no timezone math needed. Degrades
 * to a plain (trimmed) copy if the input is shorter than expected. */
static void FormatCreatedAt(const char *iso, char *out, UInt16 outcap)
{
    Int16 len = StrLen(iso);

    if (len < 16 || outcap < 18) {
        StrNCopy(out, iso, (Int16)(outcap - 1));
        out[outcap - 1] = '\0';
        return;
    }

    MemMove(out, iso, 10);       /* YYYY-MM-DD */
    out[10] = ' ';
    MemMove(out + 11, iso + 11, 5); /* HH:MM */
    out[16] = '\0';
}

static void BuildCountsLine(void)
{
    char num[NUMBUF_CAP];

    StrCopy(gCountsLine, "Replies ");
    StrIToA(num, gReplies);
    StrCat(gCountsLine, num);
    StrCat(gCountsLine, "  Boosts ");
    StrIToA(num, gReblogs);
    StrCat(gCountsLine, num);
    StrCat(gCountsLine, "  Favs ");
    StrIToA(num, gFavourites);
    StrCat(gCountsLine, num);
}

static void SetStatusError(const char *step, Int32 err)
{
    char num[NUMBUF_CAP];

    StrCopy(gStatusLine, "Error: ");
    StrCat(gStatusLine, step);
    StrCat(gStatusLine, " err=");
    StrIToA(num, err);
    StrCat(gStatusLine, num);
}

static void SetStatusUpdated(void)
{
    DateTimeType dt;

    TimSecondsToDateTime(TimGetSeconds(), &dt);
    StrCopy(gStatusLine, "Updated ");
    AppendPad2(gStatusLine, dt.hour);
    StrCat(gStatusLine, ":");
    AppendPad2(gStatusLine, dt.minute);
}

static void ScheduleNextFetch(void)
{
    gNextFetchTicks = TimGetTicks() + (SysTicksPerSecond() * FETCH_INTERVAL_TICKS_SEC);
}

/* ------------------------------------------------------------------ */
/* Networking                                                          */
/* ------------------------------------------------------------------ */

/* Runs one JSON query into out (at most outcap - 1 bytes; a longer value
 * is truncated). On failure *step names the step for the status line. */
static Int16 FetchQuery(const char *query, char *out, UInt16 outcap, const char **step)
{
    Int16 n = network_json_query_n(URL, query, out, outcap);

    if (n < 0) {
        *step = "query";
        return (Int16)-n;
    }
    return 0;
}

static Int16 FetchNumericQuery(const char *query, Int32 *out, const char **step)
{
    char numbuf[NUMBUF_CAP];
    Int16 rc = FetchQuery(query, numbuf, sizeof(numbuf), step);

    if (rc != 0) {
        return rc;
    }
    *out = StrAToI(numbuf);
    return 0;
}

/* Fetches the whole post into gScratch: opens the URL with the JSON parser,
 * runs the six queries, and closes it on every path once the open
 * succeeded. On failure gFailStep names the step ("open"/"query") and the
 * return value is the FN_ERR; gScratch may be partly overwritten, but the
 * caller only commits it on success (0). */
static Int16 FetchPost(void)
{
    Int16 rc;

    rc = (Int16)fnapp_json_open(URL);
    if (rc != FN_ERR_OK) {
        gFailStep = "open";
        return rc;
    }

    if ((rc = FetchQuery(Q_DISPLAY_NAME, gScratch.display_name,
                         sizeof(gScratch.display_name), &gFailStep)) == 0 &&
        (rc = FetchQuery(Q_CREATED_AT, gScratch.created_at,
                         sizeof(gScratch.created_at), &gFailStep)) == 0 &&
        (rc = FetchQuery(Q_CONTENT, gScratch.content,
                         sizeof(gScratch.content), &gFailStep)) == 0 &&
        (rc = FetchNumericQuery(Q_REPLIES, &gScratch.replies, &gFailStep)) == 0 &&
        (rc = FetchNumericQuery(Q_REBLOGS, &gScratch.reblogs, &gFailStep)) == 0) {
        rc = FetchNumericQuery(Q_FAVOURITES, &gScratch.favourites, &gFailStep);
    }

    fnapp_json_close(URL);
    return rc;
}

/* ------------------------------------------------------------------ */
/* Drawing                                                             */
/* ------------------------------------------------------------------ */

#define HEADER_X   4
#define HEADER_Y   16
#define HEADER_W   152
#define HEADER_H   27
#define CREATED_Y  29

#define FOOTER_X   4
#define FOOTER_Y   126
#define FOOTER_W   152
#define FOOTER_H   12
#define STATUS_Y   139
#define STATUS_W   98

static void DrawHeader(void)
{
    RectangleType r;
    FontID oldFont;

    r.topLeft.x = HEADER_X;
    r.topLeft.y = HEADER_Y;
    r.extent.x = HEADER_W;
    r.extent.y = HEADER_H;
    WinEraseRectangle(&r, 0);

    oldFont = FntSetFont(boldFont);
    WinDrawChars(gDisplayName, StrLen(gDisplayName), HEADER_X, HEADER_Y);
    FntSetFont(stdFont);
    WinDrawChars(gCreatedAtDisp, StrLen(gCreatedAtDisp), HEADER_X, CREATED_Y);
    FntSetFont(oldFont);
}

static void DrawFooter(void)
{
    RectangleType r;

    r.topLeft.x = FOOTER_X;
    r.topLeft.y = FOOTER_Y;
    r.extent.x = FOOTER_W;
    r.extent.y = FOOTER_H;
    WinEraseRectangle(&r, 0);
    WinDrawChars(gCountsLine, StrLen(gCountsLine), FOOTER_X, FOOTER_Y);

    r.topLeft.x = FOOTER_X;
    r.topLeft.y = STATUS_Y;
    r.extent.x = STATUS_W;
    r.extent.y = 12;
    WinEraseRectangle(&r, 0);
    WinDrawChars(gStatusLine, StrLen(gStatusLine), FOOTER_X, STATUS_Y);
}

static FieldType *GetContentField(FormType *frm)
{
    UInt16 idx = FrmGetObjectIndex(frm, ContentField);
    return (FieldType *)FrmGetObjectPtr(frm, idx);
}

static ScrollBarType *GetContentScrollBar(FormType *frm)
{
    UInt16 idx = FrmGetObjectIndex(frm, ContentScrollBar);
    return (ScrollBarType *)FrmGetObjectPtr(frm, idx);
}

static void UpdateScrollBar(FormType *frm)
{
    FieldType *fld = GetContentField(frm);
    ScrollBarType *bar = GetContentScrollBar(frm);
    UInt16 scrollPos, textHeight, fieldHeight;
    Int16 maxVal;

    FldGetScrollValues(fld, &scrollPos, &textHeight, &fieldHeight);
    maxVal = (textHeight > fieldHeight) ? (Int16)(textHeight - fieldHeight) : 0;

    SclSetScrollBar(bar, (Int16)scrollPos, 0, maxVal, (Int16)fieldHeight);
}

/* Re-points the (non-editable) field at gContent and resets it to the top
 * of the new post. gContent is a static buffer that outlives the field, so
 * FldSetTextPtr never needs a MemHandle or any freeing. */
static void UpdateContentField(FormType *frm)
{
    FieldType *fld = GetContentField(frm);

    FldSetTextPtr(fld, gContent);
    FldSetScrollPosition(fld, 0);
    FldRecalculateField(fld, true);
    UpdateScrollBar(frm);
}

/* ------------------------------------------------------------------ */
/* Refresh                                                             */
/* ------------------------------------------------------------------ */

static void DoRefresh(void)
{
    FormType *frm = FrmGetActiveForm();
    Int16 rc;

    StrCopy(gStatusLine, "Fetching...");
    if (frm != 0) {
        DrawFooter(); /* paint "Fetching..." before the blocking fetch below */
    }

    gFailStep = "";
    rc = FetchPost();

    if (rc == 0) {
        StrCopy(gDisplayName, gScratch.display_name);
        html_clean(gDisplayName, (unsigned long)sizeof(gDisplayName));

        FormatCreatedAt(gScratch.created_at, gCreatedAtDisp, sizeof(gCreatedAtDisp));

        StrCopy(gContent, gScratch.content);
        html_clean(gContent, (unsigned long)sizeof(gContent));

        gReplies = gScratch.replies;
        gReblogs = gScratch.reblogs;
        gFavourites = gScratch.favourites;

        BuildCountsLine();
        SetStatusUpdated();

        if (frm != 0) {
            UpdateContentField(frm);
            DrawHeader();
        }
    } else {
        SetStatusError(gFailStep, (Int32)rc);
    }

    if (frm != 0) {
        DrawFooter();
    }
    ScheduleNextFetch();
}

/* ------------------------------------------------------------------ */
/* Form / event handling                                               */
/* ------------------------------------------------------------------ */

static Boolean MainFormHandleEvent(EventType *e)
{
    switch (e->eType) {
    case frmOpenEvent: {
        FormType *frm = FrmGetActiveForm();

        FrmDrawForm(frm);
        StrCopy(gStatusLine, "");
        StrCopy(gCountsLine, "");
        DrawHeader();
        DrawFooter();
        DoRefresh(); /* blocking first fetch; draws "Fetching..." itself */
        return true;
    }

    case frmUpdateEvent: {
        FormType *frm = FrmGetActiveForm();

        FrmDrawForm(frm);
        DrawHeader();
        DrawFooter();
        return true;
    }

    case frmCloseEvent: {
        FormType *frm = FrmGetActiveForm();
        FieldType *fld = GetContentField(frm);

        FldSetTextPtr(fld, 0); /* detach our static buffer before the form goes away */
        return false; /* let the system finish closing the form */
    }

    case ctlSelectEvent:
        if (e->data.ctlSelect.controlID == RefreshButton) {
            DoRefresh();
            return true;
        }
        return false;

    case sclRepeatEvent: {
        FormType *frm = FrmGetActiveForm();
        FieldType *fld = GetContentField(frm);
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
            FieldType *fld = GetContentField(frm);
            WinDirectionType dir = (e->data.keyDown.chr == vchrPageUp) ? winUp : winDown;
            UInt16 lines = FldGetVisibleLines(fld);

            if (lines == 0) {
                lines = 1;
            }
            if (FldScrollable(fld, dir)) {
                FldScrollField(fld, lines, dir);
                UpdateScrollBar(frm);
            }
            return true;
        }
        return false;

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
                DoRefresh();
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
