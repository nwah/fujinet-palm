/* palm/apps/news/news.c
 *
 * News -- a FujiNet 8-bit news reader client for
 * https://fujinet.online/8bitnews/news.php. Category list -> paged
 * headline list -> article reader with a scrolling body field. Targets
 * Palm OS 3.1 / DragonBall EZ (Handspring Visor Deluxe), 160x160
 * mono/4-gray.
 *
 * Built on fujinet-lib-palmos exactly like ../fujiconfig/../isstracker/
 * ../weather: network_open/network_read/network_close (see
 * fujinet-network.h), wrapped by ../common/fnapp.c's
 * fnapp_http_get_all(). Never calls fuji_palmos_open() itself -- the
 * first fuji_bus_call() opens the link lazily from FujiConfig's saved
 * preference (see fujinet-palmos.h) -- but does call fuji_palmos_close()
 * on appStopEvent.
 *
 * Header order matters: see fujiconfig.c's header comment / fnapp.h.
 */
#include "fujinet-network.h"
#include "fujinet-palmos.h"
#include <PalmOS.h>
#include "fnapp.h"
#include "htmlclean.h"
#include "news_rsc.h"

#define NUMBUF_CAP 16
#define ARTICLES_PER_PAGE 5
#define ARTICLE_BODY_CAP 16000uL
#define URL_BASE "https://fujinet.online/8bitnews/news.php"

/* ------------------------------------------------------------------ */
/* Category tables (same order as news.rcp's CategoryList choices)     */
/* ------------------------------------------------------------------ */

static const char *const kCatParam[9] = {
    "top", "world", "science", "business", "technology",
    "health", "entertainment", "politics", "sports"
};

static const char *const kCatTitle[9] = {
    "Top Stories", "World News", "Science", "Business", "Technology",
    "Health", "Entertainment", "Politics", "Sports"
};

/* ------------------------------------------------------------------ */
/* Article listing state                                               */
/* ------------------------------------------------------------------ */

typedef struct {
    UInt32 id;
    char title[80];
} ArticleRow;

static Int16 gCategory = 0;
static UInt16 gListPage = 1;
static UInt16 gListPagesMax = 1;
static ArticleRow gRows[ARTICLES_PER_PAGE];
static UInt8 gRowCount = 0;
static char gArticlesTitle[32];
static char gPageLine[24];

/* ------------------------------------------------------------------ */
/* Article state                                                       */
/* ------------------------------------------------------------------ */

static UInt32 gArticleId;
static char gArticleTitle[80];
static char gArticleDateSrc[80];
static MemHandle gArticleBodyH = 0;
static UInt32 gArticleBodyLen = 0;

/* ------------------------------------------------------------------ */
/* Small generic helpers                                               */
/* ------------------------------------------------------------------ */

static void *GetObj(FormType *frm, UInt16 id)
{
    return FrmGetObjectPtr(frm, FrmGetObjectIndex(frm, id));
}

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

/* Mutating single-delimiter field splitter (this codebase never uses
 * strtok -- see fujiconfig.c/mastodon.c for the general style of hand-
 * rolled parsing). Writes a NUL over the first occurrence of delim
 * starting at *cursor, returns the field that preceded it, and advances
 * *cursor past it. Returns NULL (leaving *cursor untouched) if *cursor is
 * already NULL. If delim isn't found, the whole remaining text is
 * returned and *cursor becomes NULL (no more fields). */
static char *NextField(char **cursor, char delim)
{
    char *start = *cursor;
    char *p;

    if (!start) {
        return NULL;
    }
    p = StrChr(start, delim);
    if (p) {
        *p = '\0';
        *cursor = p + 1;
    } else {
        *cursor = NULL;
    }
    return start;
}

/* Truncates src to fit within maxWidth pixels (current font), copying
 * into out (cap bytes). Appends "..." if truncated. Uses the Palm OS
 * Font Manager's FntCharsInWidth (Font.h): on input *stringWidthP is the
 * available width and *stringLengthP the string's length; on return
 * *stringLengthP is how many characters actually fit and
 * *fitWithinWidth says whether the whole string fit. */
static void TruncateToWidth(char *out, UInt16 outCap, const char *src, Int16 maxWidth)
{
    Int16 width;
    Int16 len;
    Boolean fits;

    StrNCopy(out, src, (Int16)(outCap - 1));
    out[outCap - 1] = '\0';
    len = StrLen(out);
    width = maxWidth;
    FntCharsInWidth(out, &width, &len, &fits);

    if (!fits) {
        Int16 ellipsisWidth = FntCharsWidth("...", 3);
        Int16 width2 = (Int16)(maxWidth - ellipsisWidth);
        Int16 len2 = StrLen(out);

        if (width2 < 0) {
            width2 = 0;
        }
        FntCharsInWidth(out, &width2, &len2, &fits);
        out[len2] = '\0';
        StrCat(out, "...");
    } else {
        out[len] = '\0';
    }
}

/* ------------------------------------------------------------------ */
/* Article listing fetch                                               */
/* ------------------------------------------------------------------ */

static Boolean FetchListing(void)
{
    char url[128];
    MemHandle h;
    UInt32 len;
    char *buf;
    char *cursor;
    char *pageLine;
    char *slash;

    StrPrintF(url, "N:%s?t=lf&ps=255x24&l=%d&p=%u&c=%s",
              URL_BASE, ARTICLES_PER_PAGE, gListPage, kCatParam[gCategory]);

    if (fnapp_http_get_all(url, &h, &len, 4096uL) != errNone) {
        return false;
    }

    buf = (char *)MemHandleLock(h);
    cursor = buf;

    pageLine = NextField(&cursor, '\n');
    gListPagesMax = 1;
    if (pageLine) {
        slash = StrChr(pageLine, '/');
        if (slash) {
            gListPagesMax = (UInt16)fnapp_atol(slash + 1);
            if (gListPagesMax == 0) {
                gListPagesMax = 1;
            }
        }
    }
    StrNCopy(gPageLine, pageLine ? pageLine : "", sizeof(gPageLine) - 1);
    gPageLine[sizeof(gPageLine) - 1] = '\0';

    gRowCount = 0;
    while (cursor && gRowCount < ARTICLES_PER_PAGE) {
        char *idStr = NextField(&cursor, '|');
        char *dateStr;
        char *titleStr;

        if (!idStr || idStr[0] == '\0') {
            break;
        }
        dateStr = NextField(&cursor, '|');
        (void)dateStr; /* not shown in the list, per spec */
        titleStr = NextField(&cursor, '\n');
        if (!titleStr) {
            break;
        }

        gRows[gRowCount].id = (UInt32)fnapp_atol(idStr);
        html_clean(titleStr, StrLen(titleStr) + 1);
        StrNCopy(gRows[gRowCount].title, titleStr, sizeof(gRows[gRowCount].title) - 1);
        gRows[gRowCount].title[sizeof(gRows[gRowCount].title) - 1] = '\0';
        gRowCount++;
    }

    MemHandleUnlock(h);
    MemHandleFree(h);
    return true;
}

/* ------------------------------------------------------------------ */
/* Article fetch (all pages, capped)                                   */
/* ------------------------------------------------------------------ */

static void FreeArticleBody(void)
{
    if (gArticleBodyH) {
        MemHandleFree(gArticleBodyH);
        gArticleBodyH = 0;
    }
    gArticleBodyLen = 0;
}

static Boolean FetchArticlePage(UInt16 page, Boolean wantHeader, UInt16 *pagesOut)
{
    char url[128];
    MemHandle h;
    UInt32 len;
    char *buf;
    char *cursor;
    char *title, *date, *source, *pageLine, *body;
    UInt32 bodyLen;
    char *dst;

    StrPrintF(url, "N:%s?t=lf&ps=255x60&l=%d&p=%u&a=%lu",
              URL_BASE, ARTICLES_PER_PAGE, page, gArticleId);

    if (fnapp_http_get_all(url, &h, &len, 8192uL) != errNone) {
        return false;
    }

    buf = (char *)MemHandleLock(h);
    cursor = buf;

    title = NextField(&cursor, '\n');
    date = NextField(&cursor, '\n');
    source = NextField(&cursor, '\n');
    pageLine = NextField(&cursor, '\n');
    body = cursor;

    *pagesOut = 1;
    if (pageLine) {
        char *slash = StrChr(pageLine, '/');

        if (slash) {
            *pagesOut = (UInt16)fnapp_atol(slash + 1);
            if (*pagesOut == 0) {
                *pagesOut = 1;
            }
        }
    }

    if (wantHeader) {
        if (title) {
            html_clean(title, StrLen(title) + 1);
            StrNCopy(gArticleTitle, title, sizeof(gArticleTitle) - 1);
            gArticleTitle[sizeof(gArticleTitle) - 1] = '\0';
        }
        if (date && source) {
            StrPrintF(gArticleDateSrc, "%s   %s", date, source);
        } else {
            gArticleDateSrc[0] = '\0';
        }
        html_clean(gArticleDateSrc, sizeof(gArticleDateSrc));
    }

    if (body) {
        char *end = buf + len;

        bodyLen = (UInt32)(end - body);
        while (bodyLen > 0 &&
               (body[bodyLen - 1] == '\n' || body[bodyLen - 1] == ' ' ||
                body[bodyLen - 1] == '\r' || body[bodyLen - 1] == '\t')) {
            bodyLen--;
        }

        if (gArticleBodyH && gArticleBodyLen < ARTICLE_BODY_CAP - 1) {
            UInt32 room = ARTICLE_BODY_CAP - 1 - gArticleBodyLen;

            dst = (char *)MemHandleLock(gArticleBodyH);

            if (gArticleBodyLen > 0 && room > 0) {
                dst[gArticleBodyLen] = '\n';
                gArticleBodyLen++;
                room--;
            }
            if (bodyLen > room) {
                bodyLen = room;
            }
            if (bodyLen > 0) {
                MemMove(dst + gArticleBodyLen, body, bodyLen);
                gArticleBodyLen += bodyLen;
            }
            dst[gArticleBodyLen] = '\0';
            MemHandleUnlock(gArticleBodyH);
        }
    }

    MemHandleUnlock(h);
    MemHandleFree(h);
    return true;
}

static Boolean FetchArticle(void)
{
    UInt16 pages = 1;
    UInt16 p;

    FreeArticleBody();
    gArticleBodyH = MemHandleNew(ARTICLE_BODY_CAP);
    if (!gArticleBodyH) {
        return false;
    }
    {
        char *dst = (char *)MemHandleLock(gArticleBodyH);

        dst[0] = '\0';
        MemHandleUnlock(gArticleBodyH);
    }
    gArticleBodyLen = 0;

    if (!FetchArticlePage(1, true, &pages)) {
        FreeArticleBody();
        return false;
    }

    for (p = 2; p <= pages && gArticleBodyLen < ARTICLE_BODY_CAP - 1; p++) {
        UInt16 dummy;

        EvtResetAutoOffTimer();
        if (!FetchArticlePage(p, false, &dummy)) {
            break; /* accept truncation rather than fail the whole article */
        }
    }

    html_clean((char *)MemHandleLock(gArticleBodyH), ARTICLE_BODY_CAP);
    MemHandleUnlock(gArticleBodyH);

    return true;
}

/* ------------------------------------------------------------------ */
/* CategoryForm                                                        */
/* ------------------------------------------------------------------ */

static Boolean CategoryFormHandleEvent(EventType *e)
{
    switch (e->eType) {
    case frmOpenEvent:
        FrmDrawForm(FrmGetActiveForm());
        return true;

    case lstSelectEvent:
        if (e->data.lstSelect.listID == CategoryList) {
            Int16 sel = e->data.lstSelect.selection;

            if (sel >= 0 && sel < 9) {
                gCategory = sel;
                gListPage = 1;
                EvtResetAutoOffTimer();
                if (FetchListing()) {
                    FrmGotoForm(ArticlesForm);
                } else {
                    ShowError("Could not load articles", "");
                }
            }
            return true;
        }
        return false;

    default:
        return false;
    }
}

/* ------------------------------------------------------------------ */
/* ArticlesForm                                                        */
/* ------------------------------------------------------------------ */

static void ArticlesListDrawFunc(Int16 itemNum, RectangleType *bounds, Char **itemsText)
{
    char line[80];

    if (itemNum < 0 || (UInt8)itemNum >= gRowCount) {
        return;
    }
    TruncateToWidth(line, sizeof(line), gRows[itemNum].title, bounds->extent.x);
    WinDrawChars(line, StrLen(line), bounds->topLeft.x, bounds->topLeft.y);
}

static void DrawArticlesPageLine(void)
{
    char line[32];

    StrPrintF(line, "Page %s", gPageLine);
    DrawTextLine(line, 4, 16, 152);
}

static void RelistArticles(FormType *frm)
{
    ListType *lst = (ListType *)GetObj(frm, ArticlesList);

    StrNCopy(gArticlesTitle, kCatTitle[gCategory], sizeof(gArticlesTitle) - 1);
    gArticlesTitle[sizeof(gArticlesTitle) - 1] = '\0';
    FrmSetTitle(frm, gArticlesTitle);

    DrawArticlesPageLine();

    LstSetDrawFunction(lst, ArticlesListDrawFunc);
    LstSetListChoices(lst, 0, (Int16)gRowCount);
    LstDrawList(lst);
}

static Boolean ArticlesFormHandleEvent(EventType *e)
{
    switch (e->eType) {
    case frmOpenEvent: {
        FormType *frm = FrmGetActiveForm();

        FrmDrawForm(frm);
        RelistArticles(frm);
        return true;
    }

    case frmUpdateEvent: {
        FormType *frm = FrmGetActiveForm();
        ListType *lst = (ListType *)GetObj(frm, ArticlesList);

        FrmDrawForm(frm);
        DrawArticlesPageLine();
        LstDrawList(lst);
        return true;
    }

    case ctlSelectEvent:
        switch (e->data.ctlSelect.controlID) {
        case ArticlesBackButton:
            FrmGotoForm(CategoryForm);
            return true;
        case ArticlesPrevButton:
            if (gListPage > 1) {
                gListPage--;
                EvtResetAutoOffTimer();
                if (FetchListing()) {
                    RelistArticles(FrmGetActiveForm());
                } else {
                    ShowError("Could not load articles", "");
                }
            }
            return true;
        case ArticlesNextButton:
            if (gListPage < gListPagesMax) {
                gListPage++;
                EvtResetAutoOffTimer();
                if (FetchListing()) {
                    RelistArticles(FrmGetActiveForm());
                } else {
                    ShowError("Could not load articles", "");
                }
            }
            return true;
        default:
            return false;
        }

    case lstSelectEvent:
        if (e->data.lstSelect.listID == ArticlesList) {
            Int16 sel = e->data.lstSelect.selection;

            if (sel >= 0 && (UInt8)sel < gRowCount) {
                gArticleId = gRows[sel].id;
                EvtResetAutoOffTimer();
                if (FetchArticle()) {
                    FrmGotoForm(ArticleForm);
                } else {
                    ShowError("Could not load article", "");
                }
            }
            return true;
        }
        return false;

    default:
        return false;
    }
}

/* ------------------------------------------------------------------ */
/* ArticleForm                                                         */
/* ------------------------------------------------------------------ */

static FieldType *GetArticleField(FormType *frm)
{
    return (FieldType *)GetObj(frm, ArticleBodyField);
}

static ScrollBarType *GetArticleScrollBar(FormType *frm)
{
    return (ScrollBarType *)GetObj(frm, ArticleBodyScrollBar);
}

static void UpdateArticleScrollBar(FormType *frm)
{
    FieldType *fld = GetArticleField(frm);
    ScrollBarType *bar = GetArticleScrollBar(frm);
    UInt16 scrollPos, textHeight, fieldHeight;
    Int16 maxVal;

    FldGetScrollValues(fld, &scrollPos, &textHeight, &fieldHeight);
    maxVal = (textHeight > fieldHeight) ? (Int16)(textHeight - fieldHeight) : 0;
    SclSetScrollBar(bar, (Int16)scrollPos, 0, maxVal, (Int16)fieldHeight);
}

static void DrawArticleHeader(void)
{
    char line[40];
    FontID oldFont;
    RectangleType r;

    r.topLeft.x = 4;
    r.topLeft.y = 16;
    r.extent.x = 152;
    r.extent.y = FntLineHeight();
    WinEraseRectangle(&r, 0);
    oldFont = FntSetFont(boldFont);
    TruncateToWidth(line, sizeof(line), gArticleTitle, 152);
    WinDrawChars(line, StrLen(line), 4, 16);
    FntSetFont(oldFont);

    TruncateToWidth(line, sizeof(line), gArticleDateSrc, 152);
    DrawTextLine(line, 4, 28, 152);
}

static Boolean ArticleFormHandleEvent(EventType *e)
{
    switch (e->eType) {
    case frmOpenEvent: {
        FormType *frm = FrmGetActiveForm();
        FieldType *fld = GetArticleField(frm);

        FrmDrawForm(frm);
        DrawArticleHeader();
        FldSetTextHandle(fld, gArticleBodyH);
        FldSetScrollPosition(fld, 0);
        FldRecalculateField(fld, true);
        UpdateArticleScrollBar(frm);
        return true;
    }

    case frmCloseEvent: {
        FormType *frm = FrmGetActiveForm();
        FieldType *fld = GetArticleField(frm);

        FldSetTextHandle(fld, NULL);
        return false;
    }

    case ctlSelectEvent:
        if (e->data.ctlSelect.controlID == ArticleBackButton) {
            FrmGotoForm(ArticlesForm);
            return true;
        }
        return false;

    case sclRepeatEvent: {
        FormType *frm = FrmGetActiveForm();
        FieldType *fld = GetArticleField(frm);
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
            FieldType *fld = GetArticleField(frm);
            WinDirectionType dir = (e->data.keyDown.chr == vchrPageUp) ? winUp : winDown;
            UInt16 lines = FldGetVisibleLines(fld);

            if (lines == 0) {
                lines = 1;
            }
            if (FldScrollable(fld, dir)) {
                FldScrollField(fld, lines, dir);
                UpdateArticleScrollBar(frm);
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
        case CategoryForm:
            FrmSetEventHandler(frm, CategoryFormHandleEvent);
            break;
        case ArticlesForm:
            FrmSetEventHandler(frm, ArticlesFormHandleEvent);
            break;
        case ArticleForm:
            FrmSetEventHandler(frm, ArticleFormHandleEvent);
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

    FrmGotoForm(CategoryForm);

    do {
        EvtGetEvent(&e, evtWaitForever);
        if (SysHandleEvent(&e)) continue;
        if (MenuHandleEvent(0, &e, &err)) continue;
        if (AppHandleEvent(&e)) continue;
        FrmDispatchEvent(&e);
    } while (e.eType != appStopEvent);

    FreeArticleBody();

    if (fuji_palmos_is_open()) {
        fuji_palmos_close();
    }

    FrmCloseAllForms();
    return 0;
}
