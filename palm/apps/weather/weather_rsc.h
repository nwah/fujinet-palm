/* palm/apps/weather/weather_rsc.h -- resource IDs, shared by weather.c
 * and weather.rcp. */

#define MainForm             1000
#define UnitCButton          1010  /* PUSHBUTTON, GROUP 1, degrees C */
#define UnitFButton          1011  /* PUSHBUTTON, GROUP 1, degrees F */
#define CityButton           1012
#define RefreshButton        1013

#define CityDialog           2000
#define CityField            2010
#define CityGoButton         2011
#define CityCancelButton     2012

/* Condition icons, indexed by IconClass(): LargeIconBase + class is the
 * 32x32 icon, SmallIconBase + class the 16x16 one. Order: sun, mostly
 * sunny, partly cloudy, cloudy, drizzle, rain, thunder, snow, fog. */
#define LargeIconBase        1100
#define SmallIconBase        1200
#define SunriseBitmap        1300  /* 11x9 */
#define SunsetBitmap         1301

#define GeneralAlert         9000
#define InfoAlert            9001
