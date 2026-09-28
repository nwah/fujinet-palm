/* palm/apps/mastodon/htmlclean.h
 *
 * In-place cleanup of Mastodon post text (or any fetched field that may
 * carry stray HTML/entities/UTF-8) for display on a Latin-1 /
 * Windows-1252-ish, monochrome, non-Unicode screen such as Palm OS 3.1's.
 *
 * Deliberately portable ANSI C89 with NO platform headers (no PalmOS.h, no
 * libc string.h even) so it can be compiled and unit tested on any host as
 * well as with the m68k-palmos cross compiler. See test_htmlclean.c for the
 * host-side test harness.
 */
#ifndef HTMLCLEAN_H
#define HTMLCLEAN_H

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Cleans buf in place:
 *
 *   - HTML tags are dropped entirely.
 *   - <br>, <br/>, <br />, and </p> (case-insensitive) become a single '\n'
 *     each; runs of more than two consecutive newlines are then collapsed
 *     down to exactly two.
 *   - Leading and trailing whitespace (space, tab, CR, LF) is trimmed --
 *     this also absorbs the trailing "\r\n"/"\n" that a FujiNet query reply
 *     ends with.
 *   - Named entities &amp; &lt; &gt; &quot; &apos; &nbsp; are decoded (and
 *     &#39; via the generic numeric path below).
 *   - Numeric entities &#NNN; (decimal) and &#xHH; / &#XHH; (hex) are
 *     decoded; code points >= 256 become '?'.
 *   - UTF-8 is decoded toward Latin-1: plain ASCII (<0x80) passes through
 *     unchanged; 2-byte sequences decoding to U+00A0..U+00FF map directly
 *     to that byte; the common "smart" punctuation U+2018/U+2019 -> '\'',
 *     U+201C/U+201D -> '"', U+2013/U+2014 -> '-', and U+2026 -> "..." (3
 *     ASCII bytes, still no longer than the 3-byte UTF-8 input); every
 *     other code point -- including all of the 3-byte range not listed
 *     above and the whole 4-byte range (most emoji) -- becomes a single
 *     '?', once per code point, however many bytes it took on the wire.
 *   - A truncated multi-byte UTF-8 sequence, tag, or entity sitting right
 *     at the end of the buffer is handled gracefully (never read past the
 *     NUL terminator, never overrun cap): it degrades to '?' or a literal
 *     '&', never a crash or a buffer overrun.
 *
 * buf must already be a NUL-terminated string of at most cap-1 bytes; cap
 * is the total size of the buffer including room for the terminator. This
 * routine never writes cap or more bytes into buf and always leaves it
 * NUL-terminated. The result is never longer than the input, so it is
 * always safe to clean a buffer in place.
 *
 * Because every transform above only fires on '<', '&', or a byte >= 0x80,
 * a plain ASCII string with none of those (a plain field like a username)
 * passes through unchanged apart from whitespace trimming -- so it is safe
 * to call this on any fetched text field, not only HTML post content.
 */
void html_clean(char *buf, unsigned long cap);

#ifdef __cplusplus
}
#endif

#endif /* HTMLCLEAN_H */
