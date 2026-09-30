/* palm/apps/weather/weather.c
 *
 * Weather -- shows current conditions and a 5-day forecast for the
 * device's IP-geolocated city (or a manually searched city), using
 * ip-api.com for geolocation and Open-Meteo for weather (no API key).
 * Targets Palm OS 3.1 / DragonBall EZ (Handspring Visor Deluxe), 160x160
 * mono/4-gray. No floating point on this target -- every numeric field
 * from the JSON is either displayed as the raw decimal string the server
 * sent, or split by hand with fnapp_parse_fixed()/fnapp_atol().
 *
 * Built on fujinet-lib-palmos exactly like ../fujiconfig and
 * ../isstracker: network_open/network_json_parse/network_json_query/
 * network_close (see fujinet-network.h), wrapped by ../common/fnapp.c.
 * Never calls fuji_palmos_open() itself -- the first fuji_bus_call()
 * opens the link lazily from FujiConfig's saved preference (see
 * fujinet-palmos.h) -- but does call fuji_palmos_close() on appStopEvent.
 *
 * Header order matters: see fujiconfig.c's header comment / fnapp.h.
 */
#include "fujinet-network.h"
#include "fujinet-palmos.h"
#include <PalmOS.h>
#include "fnapp.h"
#include "weather_rsc.h"

#define NUMBUF_CAP 16

/* ------------------------------------------------------------------ */
/* Prefs                                                               */
/* ------------------------------------------------------------------ */

#define WEATHER_PREF_CREATOR 'FnWx'
#define WEATHER_PREF_ID 0
#define WEATHER_PREF_VERSION 1

typedef struct {
    UInt16 version;
    Boolean isF;
    char city[40]; /* last searched city; empty = use IP geolocation */
} WeatherPref;

static WeatherPref gPref;
static Boolean gHaveSavedPref = false;

static void LoadPref(void)
{
    UInt16 size = sizeof(gPref);
    Int16 rc;

    gPref.version = WEATHER_PREF_VERSION;
    gPref.isF = false;
    gPref.city[0] = '\0';

    rc = PrefGetAppPreferences(WEATHER_PREF_CREATOR, WEATHER_PREF_ID, &gPref, &size, false);
    gHaveSavedPref = (rc == WEATHER_PREF_VERSION && size == sizeof(gPref));
}

static void SavePref(void)
{
    PrefSetAppPreferences(WEATHER_PREF_CREATOR, WEATHER_PREF_ID,
                           WEATHER_PREF_VERSION, &gPref, sizeof(gPref), false);
    gHaveSavedPref = true;
}

/* ------------------------------------------------------------------ */
/* Location / weather state                                            */
/* ------------------------------------------------------------------ */

typedef struct {
    char city[32];
    char region[32];
    char countryCode[8];
    char lat[20];
    char lon[20];
} Location;

static Location gLoc;
static Boolean gHaveLoc = false;

/* Form title text. FrmSetTitle keeps the pointer, so this must be global
 * (and FrmCopyTitle must not be used -- see fujiconfig.c). */
/* Two title buffers, used alternately: FrmSetTitle erases the width of
 * the title it is replacing by measuring that string, so the old one must
 * still be intact when the new one is set. */
static char gTitleBuf[2][40];
static UInt8 gTitleIdx = 0;

typedef struct {
    UInt32 date;
    char hi[16];
    char lo[16];
    UInt8 code;
} DayInfo;

typedef struct {
    char temp[16];
    char feels[16];
    char humidity[16];
    char pressure[16];
    char windSpeed[16];
    Int16 windDeg;
    UInt8 code;
    char todayHi[16];
    char todayLo[16];
    Int32 utcOffset; /* seconds east of UTC, may be negative */
    UInt32 sunrise;  /* unix seconds UTC, 0 = unknown */
    UInt32 sunset;
    DayInfo day[5];
} WeatherData;

static WeatherData gW;
static Boolean gHaveWeather = false;

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

/* Appends src to dst (which must already be NUL-terminated, cap total
 * bytes), replacing every space with "%20" -- the only URL-encoding this
 * app needs (city names). */
static void AppendUrlEncoded(char *dst, UInt16 cap, const char *src)
{
    UInt16 len = StrLen(dst);

    while (*src != '\0' && len + 4 < cap) {
        if (*src == ' ') {
            dst[len++] = '%';
            dst[len++] = '2';
            dst[len++] = '0';
        } else {
            dst[len++] = *src;
        }
        src++;
    }
    dst[len] = '\0';
}

/* ------------------------------------------------------------------ */
/* WMO weather-code tables (verbatim from the fujinet-weather MS-DOS
 * client's weather.c decode_description()/icon_code()).                */
/* ------------------------------------------------------------------ */

static void DecodeDescription(UInt8 code, char *buf, UInt16 cap)
{
    const char *s;

    switch (code) {
    case 0:  s = "Sunny"; break;
    case 1:  s = "Mainly sunny"; break;
    case 2:  s = "Partly cloudy"; break;
    case 3:  s = "Cloudy"; break;
    case 45: s = "Foggy"; break;
    case 48: s = "Rime fog"; break;
    case 51: s = "Light drizzle"; break;
    case 53: s = "Drizzle"; break;
    case 55: s = "Heavy drizzle"; break;
    case 56: s = "Light freezing drizzle"; break;
    case 57: s = "Freezing drizzle"; break;
    case 61: s = "Light rain"; break;
    case 63: s = "Rain"; break;
    case 65: s = "Heavy rain"; break;
    case 66: s = "Light freezing rain"; break;
    case 67: s = "Freezing rain"; break;
    case 71: s = "Light snow"; break;
    case 73: s = "Snow"; break;
    case 75: s = "Heavy snow"; break;
    case 77: s = "Snow grains"; break;
    case 80: s = "Light showers"; break;
    case 81: s = "Showers"; break;
    case 82: s = "Heavy showers"; break;
    case 85: s = "Light snow showers"; break;
    case 86: s = "Snow showers"; break;
    case 95: s = "Thunderstorm"; break;
    case 96: s = "Thunderstorms w/ hail"; break;
    case 99: s = "Thunderstorm w/ hail"; break;
    default: s = "???"; break;
    }
    StrNCopy(buf, s, (Int16)(cap - 1));
    buf[cap - 1] = '\0';
}

static UInt8 IconClass(UInt8 code)
{
    switch (code) {
    case 0:  return 0;
    case 1:  return 1;
    case 2:  return 2;
    case 3:  return 3;
    case 51: case 53: case 55: case 56: case 57:
    case 80: case 81: case 82: return 4;
    case 61: case 63: case 65: case 66: case 67: return 5;
    case 95: case 96: case 99: return 6;
    case 71: case 73: case 75: case 77: case 85: case 86: return 7;
    case 45: case 48: return 8;
    default: return 1;
    }
}

static const char *const kWindDeg[8] = { "N", "NE", "E", "SE", "S", "SW", "W", "NW" };
static const char *const kDayNames[7] = { "Sun", "Mon", "Tue", "Wed", "Thu", "Fri", "Sat" };

static Int16 WeekdayFromEpoch(UInt32 unixSecs)
{
    UInt32 daysSinceEpoch = unixSecs / 86400uL;

    return (Int16)((daysSinceEpoch + 4uL) % 7uL); /* Jan 1 1970 was a Thursday */
}

/* ------------------------------------------------------------------ */
/* Fetching                                                            */
/* ------------------------------------------------------------------ */

#define IP_URL "N:http://ip-api.com/json/?fields=status,city,regionName,countryCode,lon,lat"

static Boolean GetIpLocation(void)
{
    char buf[16];
    FN_ERR rc;

    rc = fnapp_json_open(IP_URL);
    if (rc != FN_ERR_OK) {
        return false;
    }

    fnapp_json_query(IP_URL, "/status", buf, sizeof(buf));
    if (StrCompare(buf, "success") != 0) {
        fnapp_json_close(IP_URL);
        return false;
    }

    fnapp_json_query(IP_URL, "/city", gLoc.city, sizeof(gLoc.city));
    fnapp_json_query(IP_URL, "/regionName", gLoc.region, sizeof(gLoc.region));
    fnapp_json_query(IP_URL, "/countryCode", gLoc.countryCode, sizeof(gLoc.countryCode));
    fnapp_json_query(IP_URL, "/lat", gLoc.lat, sizeof(gLoc.lat));
    fnapp_json_query(IP_URL, "/lon", gLoc.lon, sizeof(gLoc.lon));

    fnapp_json_close(IP_URL);
    return true;
}

static Boolean GeocodeCity(const char *city)
{
    char url[160];

    /* http, NOT https: https://geocoding-api.open-meteo.com was confirmed
     * flaky through fujinet-pc (intermittent 0-byte bodies) during live
     * verification; http:// against the same host was 100% reliable
     * across repeated runs. See host/fnlib_host.c's test_weather(). */
    StrCopy(url, "N:http://geocoding-api.open-meteo.com/v1/search?name=");
    AppendUrlEncoded(url, sizeof(url), city);
    StrNCat(url, "&count=1&language=en&format=json", (Int16)sizeof(url));

    if (fnapp_json_open(url) != FN_ERR_OK) {
        return false;
    }

    fnapp_json_query(url, "/results/0/name", gLoc.city, sizeof(gLoc.city));
    if (gLoc.city[0] == '\0') {
        fnapp_json_close(url);
        return false;
    }
    fnapp_json_query(url, "/results/0/latitude", gLoc.lat, sizeof(gLoc.lat));
    fnapp_json_query(url, "/results/0/longitude", gLoc.lon, sizeof(gLoc.lon));
    fnapp_json_query(url, "/results/0/country_code", gLoc.countryCode, sizeof(gLoc.countryCode));
    fnapp_json_query(url, "/results/0/admin1", gLoc.region, sizeof(gLoc.region));

    fnapp_json_close(url);
    return true;
}

/* fujinet-pc/the firmware silently truncates a devicespec (the whole
 * "N:..." string, INCLUDING the "N:" prefix) at 256 bytes regardless of
 * this platform's FUJI_VARIABLE_LEN_PACKETS=1 (confirmed live 2026-09-28:
 * a 357-byte combined-request URL got cut at exactly byte 256, mid
 * parameter value, silently producing a different/broken query with no
 * open/parse error -- see host/fnlib_host.c's test_weather() for the
 * repro). So unlike ISS/News (whose URLs are all well under 100 bytes),
 * Weather's Open-Meteo forecast query -- which needs many fields -- MUST
 * be split into several short requests, each safely under ~230 bytes even
 * with worst-case (9-byte) lat/lon strings (the longest, request 3 with
 * &timezone=auto, is ~210 bytes). This mirrors why the original
 * fujinet-weather MS-DOS client (msdos/src/openmeteo.c) also splits its
 * queries into several small requests rather than one big one. */
static void BuildBaseUrl(char *url, UInt16 cap)
{
    StrCopy(url, "N:http://api.open-meteo.com/v1/forecast?latitude=");
    StrNCat(url, gLoc.lat, (Int16)cap);
    StrNCat(url, "&longitude=", (Int16)cap);
    StrNCat(url, gLoc.lon, (Int16)cap);
}

static Boolean GetWeather(void)
{
    char url[256];
    char buf[16];
    char path[24];
    UInt8 i;

    /* Request 1: current temp/feels-like/wind (unit-dependent fields). */
    BuildBaseUrl(url, sizeof(url));
    StrNCat(url, "&current=temperature_2m,apparent_temperature,wind_speed_10m,"
                 "wind_direction_10m&temperature_unit=", (Int16)sizeof(url));
    StrNCat(url, gPref.isF ? "fahrenheit" : "celsius", (Int16)sizeof(url));
    StrNCat(url, "&wind_speed_unit=", (Int16)sizeof(url));
    StrNCat(url, gPref.isF ? "mph" : "kmh", (Int16)sizeof(url));
    StrNCat(url, "&timeformat=unixtime", (Int16)sizeof(url));

    if (fnapp_json_open(url) != FN_ERR_OK) {
        return false;
    }
    fnapp_json_query(url, "/current/temperature_2m", gW.temp, sizeof(gW.temp));
    fnapp_json_query(url, "/current/apparent_temperature", gW.feels, sizeof(gW.feels));
    fnapp_json_query(url, "/current/wind_speed_10m", gW.windSpeed, sizeof(gW.windSpeed));
    fnapp_json_query(url, "/current/wind_direction_10m", buf, sizeof(buf));
    gW.windDeg = (Int16)fnapp_atol(buf);
    fnapp_json_close(url);

    /* Request 2: current humidity/weather code/pressure (unit-independent),
     * plus today's sunrise/sunset and the location's UTC offset. */
    BuildBaseUrl(url, sizeof(url));
    StrNCat(url, "&current=relative_humidity_2m,weather_code,surface_pressure"
                 "&daily=sunrise,sunset&forecast_days=1&timezone=auto"
                 "&timeformat=unixtime", (Int16)sizeof(url));

    if (fnapp_json_open(url) != FN_ERR_OK) {
        return false;
    }
    fnapp_json_query(url, "/current/relative_humidity_2m", gW.humidity, sizeof(gW.humidity));
    fnapp_json_query(url, "/current/surface_pressure", gW.pressure, sizeof(gW.pressure));
    fnapp_json_query(url, "/current/weather_code", buf, sizeof(buf));
    gW.code = (UInt8)fnapp_atol(buf);
    fnapp_json_query(url, "/utc_offset_seconds", buf, sizeof(buf));
    gW.utcOffset = fnapp_atol(buf);
    fnapp_json_query(url, "/daily/sunrise/0", buf, sizeof(buf));
    gW.sunrise = (UInt32)fnapp_atol(buf);
    fnapp_json_query(url, "/daily/sunset/0", buf, sizeof(buf));
    gW.sunset = (UInt32)fnapp_atol(buf);
    fnapp_json_close(url);

    /* Request 3: today's hi/lo (index 0) and the 5-day forecast (1..5);
     * timezone=auto makes daily/time local midnights. */
    BuildBaseUrl(url, sizeof(url));
    StrNCat(url, "&forecast_days=6&daily=temperature_2m_max,temperature_2m_min,"
                 "weather_code&temperature_unit=", (Int16)sizeof(url));
    StrNCat(url, gPref.isF ? "fahrenheit" : "celsius", (Int16)sizeof(url));
    StrNCat(url, "&timezone=auto&timeformat=unixtime", (Int16)sizeof(url));

    if (fnapp_json_open(url) != FN_ERR_OK) {
        return false;
    }
    fnapp_json_query(url, "/daily/temperature_2m_max/0", gW.todayHi, sizeof(gW.todayHi));
    fnapp_json_query(url, "/daily/temperature_2m_min/0", gW.todayLo, sizeof(gW.todayLo));
    for (i = 0; i < 5; i++) {
        StrPrintF(path, "/daily/time/%d", (Int16)(i + 1));
        fnapp_json_query(url, path, buf, sizeof(buf));
        gW.day[i].date = (UInt32)fnapp_atol(buf);

        StrPrintF(path, "/daily/temperature_2m_max/%d", (Int16)(i + 1));
        fnapp_json_query(url, path, gW.day[i].hi, sizeof(gW.day[i].hi));

        StrPrintF(path, "/daily/temperature_2m_min/%d", (Int16)(i + 1));
        fnapp_json_query(url, path, gW.day[i].lo, sizeof(gW.day[i].lo));

        StrPrintF(path, "/daily/weather_code/%d", (Int16)(i + 1));
        fnapp_json_query(url, path, buf, sizeof(buf));
        gW.day[i].code = (UInt8)fnapp_atol(buf);
    }
    fnapp_json_close(url);

    return true;
}

/* ------------------------------------------------------------------ */
/* Drawing                                                             */
/* ------------------------------------------------------------------ */

/* Rounds a decimal string to the nearest integer (halves away from zero).
 * Never yields a negative zero: the sign only survives if the result != 0. */
static Int32 RoundFixed(const char *raw)
{
    FixedPt fp;
    Int32 mag;

    fnapp_parse_fixed(raw, &fp);
    mag = (Int32)fp.whole + (fp.frac >= 50 ? 1 : 0);
    return (fp.neg && mag != 0) ? -mag : mag;
}

static void RoundedUnit(const char *raw, char *out, UInt16 outCap, const char *unit)
{
    StrPrintF(out, "%ld%s", RoundFixed(raw), unit);
    out[outCap - 1] = '\0';
}

static void DrawBitmapRes(UInt16 id, Coord x, Coord y)
{
    MemHandle h = DmGetResource(bitmapRsc, id);

    if (h) {
        BitmapType *bmp = (BitmapType *)MemHandleLock(h);

        WinDrawBitmap(bmp, x, y);
        MemHandleUnlock(h);
        DmReleaseResource(h);
    }
}

/* Draws text at (x, y), cutting it to the longest prefix that fits maxW. */
static void DrawTrunc(const char *text, Coord x, Coord y, Coord maxW)
{
    Int16 width = maxW;
    Int16 len = (Int16)StrLen(text);
    Boolean fits;

    FntCharsInWidth(text, &width, &len, &fits);
    WinDrawChars(text, len, x, y);
}

static void DrawCentered(const char *text, Coord cx, Coord y)
{
    Int16 w = FntCharsWidth(text, (Int16)StrLen(text));

    WinDrawChars(text, (Int16)StrLen(text), (Coord)(cx - w / 2), y);
}

static void DrawRight(const char *text, Coord xr, Coord y)
{
    Int16 w = FntCharsWidth(text, (Int16)StrLen(text));

    WinDrawChars(text, (Int16)StrLen(text), (Coord)(xr - w), y);
}

/* Draws a sunrise/sunset glyph at (x, 78) and the local time after it. */
static void DrawSunCell(UInt16 bitmapId, UInt32 unixSecs, Coord x)
{
    char buf[timeStringLength + 1];
    Int32 t = (Int32)unixSecs + gW.utcOffset;
    Int32 secs = ((t % 86400L) + 86400L) % 86400L;

    DrawBitmapRes(bitmapId, x, 78);
    TimeToAscii((UInt8)(secs / 3600L), (UInt8)((secs % 3600L) / 60L),
                (TimeFormatType)PrefGetPreference(prefTimeFormat), buf);
    DrawTrunc(buf, (Coord)(x + 14), 77, 62);
}

/* Sets the MainForm title to the city name, cut (with "...") to fit 112 px
 * in boldFont. Only call with the MainForm pointer. */
static void UpdateTitle(FormType *frm)
{
    FontID oldFont;
    Int16 width = 112;
    Int16 len;
    Boolean fits;
    char *title;

    if (!gHaveLoc) {
        return;
    }
    gTitleIdx ^= 1;
    title = gTitleBuf[gTitleIdx];
    StrNCopy(title, gLoc.city, (Int16)(sizeof(gTitleBuf[0]) - 1));
    title[sizeof(gTitleBuf[0]) - 1] = '\0';

    oldFont = FntSetFont(boldFont);
    len = (Int16)StrLen(title);
    FntCharsInWidth(title, &width, &len, &fits);
    if (len < (Int16)StrLen(title)) {
        /* Cut: leave room for the ellipsis. */
        while (len > 0 &&
               FntCharsWidth(title, len) + FntCharsWidth("...", 3) > 112) {
            len--;
        }
        StrCopy(title + len, "...");
    }
    FntSetFont(oldFont);
    FrmSetTitle(frm, title);
}

static void DrawWeather(void)
{
    char line[64];
    char temp[16];
    char hilo[24];
    char tmp2[16];
    RectangleType r;
    FontID oldFont;
    Int16 tempRight;
    Int16 hiloW;
    Int32 spd;
    UInt8 i;

    if (!gHaveLoc || !gHaveWeather) {
        return;
    }

    r.topLeft.x = 0;
    r.topLeft.y = 15;
    r.extent.x = 160;
    r.extent.y = 131;
    WinEraseRectangle(&r, 0);

    oldFont = FntSetFont(boldFont);

    /* Large icon, current temperature, today's hi/lo, condition */
    DrawBitmapRes((UInt16)(LargeIconBase + IconClass(gW.code)), 2, 18);

    StrPrintF(temp, "%ld\260%s", RoundFixed(gW.temp), gPref.isF ? "F" : "C");
    FntSetFont(largeBoldFont);
    tempRight = (Int16)(38 + FntCharsWidth(temp, (Int16)StrLen(temp)));
    WinDrawChars(temp, (Int16)StrLen(temp), 38, 18);

    FntSetFont(stdFont);
    StrPrintF(hilo, "H %ld\260 L %ld\260", RoundFixed(gW.todayHi), RoundFixed(gW.todayLo));
    hiloW = FntCharsWidth(hilo, (Int16)StrLen(hilo));
    if (tempRight + 4 > 158 - hiloW) {
        StrPrintF(hilo, "%ld\260/%ld\260", RoundFixed(gW.todayHi), RoundFixed(gW.todayLo));
    }
    DrawRight(hilo, 158, 21);

    DecodeDescription(gW.code, line, sizeof(line));
    DrawTrunc(line, 38, 35, 120);

    /* Details, two columns */
    RoundedUnit(gW.feels, tmp2, sizeof(tmp2), "\260");
    StrPrintF(line, "Feels %s", tmp2);
    DrawTrunc(line, 2, 55, 76);

    RoundedUnit(gW.humidity, tmp2, sizeof(tmp2), "%");
    StrPrintF(line, "Humidity %s", tmp2);
    DrawTrunc(line, 82, 55, 76);

    spd = RoundFixed(gW.windSpeed);
    if (spd == 0) {
        StrCopy(line, "Wind calm");
    } else {
        Int16 idx = (Int16)((((gW.windDeg % 360) + 360) % 360) / 45);

        StrPrintF(line, "Wind %ld %s %s", spd, gPref.isF ? "mph" : "km/h", kWindDeg[idx]);
    }
    DrawTrunc(line, 2, 66, 76);

    RoundedUnit(gW.pressure, tmp2, sizeof(tmp2), " hPa");
    DrawTrunc(tmp2, 82, 66, 76);

    if (gW.sunrise != 0) {
        DrawSunCell(SunriseBitmap, gW.sunrise, 2);
        if (gW.sunset != 0) {
            DrawSunCell(SunsetBitmap, gW.sunset, 82);
        }
    }

    /* Rule */
    WinDrawGrayLine(2, 91, 157, 91);

    /* 5-day forecast */
    for (i = 0; i < 5; i++) {
        Coord cx = (Coord)(i * 32 + 16);
        UInt32 local = gW.day[i].date + (UInt32)gW.utcOffset; /* wraps correctly */
        char t[16];

        FntSetFont(boldFont);
        DrawCentered(kDayNames[WeekdayFromEpoch(local)], cx, 95);
        DrawBitmapRes((UInt16)(SmallIconBase + IconClass(gW.day[i].code)),
                      (Coord)(i * 32 + 8), 108);
        RoundedUnit(gW.day[i].hi, t, sizeof(t), "\260");
        DrawCentered(t, cx, 125);
        FntSetFont(stdFont);
        RoundedUnit(gW.day[i].lo, t, sizeof(t), "\260");
        DrawCentered(t, cx, 135);
    }

    FntSetFont(oldFont);
}

static void SyncUnitButtons(FormType *frm)
{
    CtlSetValue((ControlType *)GetObj(frm, UnitCButton), gPref.isF ? 0 : 1);
    CtlSetValue((ControlType *)GetObj(frm, UnitFButton), gPref.isF ? 1 : 0);
}

/* ------------------------------------------------------------------ */
/* Refresh flow                                                        */
/* ------------------------------------------------------------------ */

static Boolean RefreshLocation(void)
{
    Boolean ok;
    Boolean firstEver = !gHaveSavedPref;

    if (gPref.city[0] == '\0') {
        ok = GetIpLocation();
    } else {
        ok = GeocodeCity(gPref.city);
    }
    if (!ok) {
        return false;
    }
    gHaveLoc = true;

    if (firstEver) {
        gPref.isF = (StrCompare(gLoc.countryCode, "US") == 0);
        SavePref();
    }
    return true;
}

static void FullRefresh(FormType *frm)
{
    if (!RefreshLocation()) {
        ShowError("Could not get location", "");
        return;
    }
    if (!GetWeather()) {
        ShowError("Could not get weather", "");
        gHaveWeather = false;
        return;
    }
    gHaveWeather = true;
    if (frm != 0) {
        UpdateTitle(frm);
        DrawWeather();
    }
}

static void WeatherOnlyRefresh(FormType *frm)
{
    if (!gHaveLoc) {
        FullRefresh(frm);
        return;
    }
    if (!GetWeather()) {
        ShowError("Could not get weather", "");
        return;
    }
    gHaveWeather = true;
    if (frm != 0) {
        DrawWeather();
    }
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
        SyncUnitButtons(frm);
        FullRefresh(frm);
        SyncUnitButtons(frm); /* first run picks C/F from the country */
        return true;
    }

    case frmUpdateEvent: {
        FormType *frm = FrmGetActiveForm();

        FrmDrawForm(frm);
        SyncUnitButtons(frm);
        UpdateTitle(frm);
        DrawWeather();
        return true;
    }

    case ctlSelectEvent:
        switch (e->data.ctlSelect.controlID) {
        case RefreshButton:
            WeatherOnlyRefresh(FrmGetActiveForm());
            return true;
        case UnitCButton:
        case UnitFButton:
            gPref.isF = (e->data.ctlSelect.controlID == UnitFButton);
            SavePref();
            WeatherOnlyRefresh(FrmGetActiveForm());
            return true;
        case CityButton:
            FrmPopupForm(CityDialog);
            return true;
        default:
            return false;
        }

    default:
        return false;
    }
}

/* ------------------------------------------------------------------ */
/* CityDialog                                                          */
/* ------------------------------------------------------------------ */

static Boolean CityDialogHandleEvent(EventType *e)
{
    switch (e->eType) {
    case frmOpenEvent: {
        FormType *frm = FrmGetActiveForm();
        FieldType *fld = (FieldType *)GetObj(frm, CityField);

        FrmDrawForm(frm);
        FieldSetEditText(fld, gPref.city, sizeof(gPref.city));
        FrmSetFocus(frm, FrmGetObjectIndex(frm, CityField));
        return true;
    }

    case ctlSelectEvent:
        if (e->data.ctlSelect.controlID == CityCancelButton) {
            FrmReturnToForm(0);
            return true;
        }
        if (e->data.ctlSelect.controlID == CityGoButton) {
            char cityBuf[40];

            FieldGetEditText((FieldType *)GetObj(FrmGetActiveForm(), CityField),
                              cityBuf, sizeof(cityBuf));

            if (cityBuf[0] == '\0') {
                gPref.city[0] = '\0';
                SavePref();
                gHaveLoc = false;
                FrmReturnToForm(0);
                FullRefresh(FrmGetActiveForm());
                FrmUpdateForm(MainForm, frmRedrawUpdateCode);
                return true;
            }

            if (GeocodeCity(cityBuf)) {
                StrNCopy(gPref.city, cityBuf, sizeof(gPref.city) - 1);
                gPref.city[sizeof(gPref.city) - 1] = '\0';
                SavePref();
                gHaveLoc = true;
                if (GetWeather()) {
                    gHaveWeather = true;
                }
                FrmReturnToForm(0);
                FrmUpdateForm(MainForm, frmRedrawUpdateCode);
            } else {
                ShowError("City not found", "");
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
        case CityDialog:
            FrmSetEventHandler(frm, CityDialogHandleEvent);
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

    LoadPref();
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

    FrmCloseAllForms();
    return 0;
}
