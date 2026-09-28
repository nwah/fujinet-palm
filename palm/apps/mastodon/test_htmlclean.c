/* palm/apps/mastodon/test_htmlclean.c
 *
 * Host-side unit test for htmlclean.c. ANSI C89, builds with a plain host
 * `cc` (no PalmOS headers, no special flags):
 *
 *   cc -std=c89 -Wall -Wextra -o test_htmlclean test_htmlclean.c htmlclean.c
 *   ./test_htmlclean
 *
 * Not part of the mastodon.prc build (the Makefile never compiles this
 * file for the m68k target) -- it exists purely to exercise html_clean()
 * on the Mac before trusting it on the device.
 */
#include <stdio.h>
#include <string.h>
#include "htmlclean.h"

static int gFail = 0;
static int gCount = 0;

static void check(const char *label, const char *input, const char *expect)
{
    char buf[4096];
    size_t n = strlen(input);

    gCount++;
    if (n >= sizeof(buf)) {
        printf("FAIL %-28s input too long for test buffer\n", label);
        gFail++;
        return;
    }
    memcpy(buf, input, n + 1);

    html_clean(buf, (unsigned long)sizeof(buf));

    if (strcmp(buf, expect) == 0) {
        printf("ok   %-28s -> \"%s\"\n", label, buf);
    } else {
        printf("FAIL %-28s got \"%s\", want \"%s\"\n", label, buf, expect);
        gFail++;
    }
}

/* Like check(), but cap is passed explicitly (smaller than sizeof(buf)) so
 * we can prove the routine never writes at or past cap. Takes a snapshot of
 * the untouched input before calling html_clean, then afterward verifies
 * every byte from cap onward through the original input's own NUL is still
 * exactly what it was -- i.e. html_clean touched nothing at or past cap.
 * (An in-buffer canary byte at buf[cap] doesn't work here: cap is often
 * smaller than strlen(input), so buf[cap] falls inside the live input and
 * planting a canary there would corrupt what html_clean is supposed to
 * read, rather than testing what it's allowed to write.) */
static void check_cap(const char *label, const char *input, unsigned long cap,
                       const char *expect)
{
    char buf[4096];
    char orig[4096];
    size_t n = strlen(input);
    unsigned long k;
    int overrun = 0;

    gCount++;
    if (n >= sizeof(buf) - 1 || cap >= sizeof(buf) - 1) {
        printf("FAIL %-28s test setup too small\n", label);
        gFail++;
        return;
    }
    memcpy(buf, input, n + 1);
    memcpy(orig, input, n + 1);

    html_clean(buf, cap);

    for (k = cap; k <= n; k++) {
        if (buf[k] != orig[k]) {
            overrun = 1;
            break;
        }
    }
    if (overrun) {
        printf("FAIL %-28s wrote past cap=%lu (byte %lu changed)\n", label, cap, k);
        gFail++;
        return;
    }
    if (strcmp(buf, expect) == 0) {
        printf("ok   %-28s cap=%-4lu -> \"%s\"\n", label, cap, buf);
    } else {
        printf("FAIL %-28s cap=%-4lu got \"%s\", want \"%s\"\n", label, cap, buf, expect);
        gFail++;
    }
}

int main(void)
{
    /* --- tag stripping --- */
    check("plain passthrough", "hello world", "hello world");
    check("strip simple tags", "<b>bold</b> and <i>italic</i>", "bold and italic");
    check("strip tag with attrs", "a <a href=\"http://x/?a=1&b=2\">link</a> b", "a link b");
    check("unterminated tag", "abc<span cla", "abc");

    /* --- br / </p> -> newline, run collapsing --- */
    check("br variants", "a<br>b<br/>c<br />d", "a\nb\nc\nd");
    check("close-p newline", "<p>Para1</p><p>Para2</p>", "Para1\nPara2");
    check("collapse long run", "x<br><br><br><br>y", "x\n\ny");
    check("trim leading/trailing ws", "  \n\t hello world  \n", "hello world");
    check("trailing p newline trimmed", "<p>only</p>", "only");

    /* --- named entities --- */
    check("amp", "a&amp;b", "a&b");
    check("lt", "a&lt;b", "a<b");
    check("gt", "a&gt;b", "a>b");
    check("quot", "a&quot;b", "a\"b");
    check("apos", "a&apos;b", "a'b");
    check("nbsp", "a&nbsp;b", "a b");
    check("unknown entity passthrough", "a&unknown;b", "a&unknown;b");
    check("truncated entity at end", "abc&amp", "abc&amp");

    /* --- numeric entities --- */
    check("decimal entity #39", "it&#39;s", "it's");
    check("decimal entity #65", "&#65;", "A");
    check("hex entity #x41", "&#x41;", "A");
    check("hex entity uppercase X", "&#X41;", "A");
    check("numeric entity >=256 -> ?", "&#128512;", "?");
    check("hex numeric >=256 -> ?", "&#x1F600;", "?");
    check("malformed numeric entity", "&#zz;end", "&#zz;end");

    /* --- UTF-8 -> Latin-1 (2-byte, U+00A0..U+00FF) --- */
    check("utf8 e-acute (cafe)", "caf\xC3\xA9", "caf\xE9");
    check("utf8 i-diaeresis (naive)", "na\xC3\xAF" "ve", "na\xEF" "ve");
    check("utf8 nbsp-range low", "a\xC2\xA0" "b", "a\xA0" "b");

    /* --- smart punctuation (3-byte UTF-8) --- */
    check("smart single quotes",
          "\xE2\x80\x98quoted\xE2\x80\x99",
          "'quoted'");
    check("smart double quotes",
          "\xE2\x80\x9Ctext\xE2\x80\x9D",
          "\"text\"");
    check("en/em dash",
          "dash\xE2\x80\x93" "here\xE2\x80\x94" "there",
          "dash-here-there");
    check("ellipsis",
          "wait\xE2\x80\xA6" "end",
          "wait...end");
    check("other 3-byte codepoint -> ?", "\xE4\xB8\xAD" "text", "?text"); /* CJK U+4E2D */

    /* --- emoji / 4-byte UTF-8 -> single '?' each --- */
    check("single emoji", "hi\xF0\x9F\x98\x80" "bye", "hi?bye");
    check("two emoji, one ? each",
          "\xF0\x9F\x98\x80\xF0\x9F\x98\x81",
          "??");

    /* --- truncated UTF-8 at the very end of the buffer --- */
    check("truncated 2-byte seq at end", "abc\xC3", "abc?");
    check("truncated 3-byte seq at end (1/2)", "abc\xE2\x80", "abc?");
    check("truncated 3-byte seq at end (0/2)", "abc\xE2", "abc?");
    check("truncated 4-byte seq at end (2/3)", "abc\xF0\x9F\x98", "abc?");
    check("truncated 4-byte seq at end (1/3)", "abc\xF0\x9F", "abc?");
    check("truncated 4-byte seq at end (0/3)", "abc\xF0", "abc?");
    check("stray continuation byte", "a\x80" "b", "a?b");
    check("invalid lead byte 0xFF", "a\xFF" "b", "a?b");

    /* --- explicit small-cap overrun guard --- */
    check_cap("cap truncates cleanly", "0123456789ABCDEF", 8, "0123456");
    check_cap("cap mid entity", "AB&amp;CDEFGH", 6, "AB&CD");
    check_cap("cap mid utf8 seq", "AB\xC3\xA9" "CDEF", 4, "AB\xE9");

    /* --- real content fetched live from host/fnhost against fujinet-pc,
     * oldbytes.space public timeline (see task notes). Captured with:
     *   host/fnhost json "https://oldbytes.space/api/v1/timelines/public?limit=1" /0/content
     *   host/fnhost json "https://oldbytes.space/api/v1/timelines/public?limit=1" /0/account/display_name
     * Both end with the wire's trailing "\n\n", which html_clean's
     * trailing-whitespace trim must absorb along with the HTML markup. */
    check("real content sample",
          "<p>*healing vibes* <a href=\"https://www.youtube.com/watch?v=D6exw6xT0oo\" rel=\"nofollow noopener\" translate=\"no\" target=\"_blank\"><span class=\"invisible\">https://www.</span><span class=\"\">youtube.com/watch?v=D6exw6xT0oo</span><span class=\"invisible\"></span></a></p>\n\n",
          "*healing vibes* https://www.youtube.com/watch?v=D6exw6xT0oo");

    check("real display_name sample (flag emoji)",
          "JESUS ATE YOUR FILESYSTEM 🇺🇦🇨🇿\n\n",
          "JESUS ATE YOUR FILESYSTEM ????");

    printf("\n%d/%d checks passed\n", gCount - gFail, gCount);
    return gFail ? 1 : 0;
}
