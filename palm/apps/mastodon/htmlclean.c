/* palm/apps/mastodon/htmlclean.c
 *
 * See htmlclean.h. ANSI C89, no platform headers, no libc calls (not even
 * string.h) -- everything needed is hand-rolled below so this file builds
 * identically under the m68k-palmos cross compiler and any host `cc`.
 */
#include "htmlclean.h"

/* ---- tiny local helpers ------------------------------------------------ */

static int is_digit(char c)
{
    return c >= '0' && c <= '9';
}

static int hex_val(char c)
{
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

/* Case-insensitive compare of a[0..n) against the fixed lowercase literal
   b[0..n). Never reads past a's NUL (returns false as soon as it hits one,
   without looking further). */
static int ci_eq(const char *a, const char *b, unsigned long n)
{
    unsigned long i;

    for (i = 0; i < n; i++) {
        char ca = a[i];
        char cb = b[i];

        if (ca == '\0') {
            return 0;
        }
        if (ca >= 'A' && ca <= 'Z') {
            ca = (char)(ca + 32);
        }
        if (cb >= 'A' && cb <= 'Z') {
            cb = (char)(cb + 32);
        }
        if (ca != cb) {
            return 0;
        }
    }
    return 1;
}

/* Bounds-checked single-byte append: never writes buf[cap-1] or beyond,
   always leaves room for the final NUL. */
static void emit(char *buf, unsigned long cap, unsigned long *o, char c)
{
    if (*o < cap - 1) {
        buf[*o] = c;
        (*o)++;
    }
}

/* ---- tag classification ------------------------------------------------ */

/* tag points just past '<', taglen is the number of bytes up to (not
   including) the matching '>'. Returns 1 if this is <br>, <br/>, <br />, or
   </p> (case-insensitive), which the caller turns into a newline; 0 for
   every other tag, which the caller drops silently. */
static int tag_is_newline(const char *tag, unsigned long taglen)
{
    unsigned long j;

    if (taglen >= 2 && ci_eq(tag, "br", 2)) {
        j = 2;
        while (j < taglen && (tag[j] == ' ' || tag[j] == '\t')) j++;
        if (j < taglen && tag[j] == '/') {
            j++;
            while (j < taglen && (tag[j] == ' ' || tag[j] == '\t')) j++;
        }
        return j == taglen;
    }

    if (taglen >= 2 && tag[0] == '/' && (tag[1] == 'p' || tag[1] == 'P')) {
        j = 2;
        while (j < taglen && (tag[j] == ' ' || tag[j] == '\t')) j++;
        return j == taglen;
    }

    return 0;
}

/* ---- entity decoding ---------------------------------------------------- */

struct named_entity {
    const char *name;
    char out;
};

static const struct named_entity kNamed[] = {
    { "amp",  '&'  },
    { "lt",   '<'  },
    { "gt",   '>'  },
    { "quot", '"'  },
    { "apos", '\'' },
    { "nbsp", ' '  }
};

#define kNamedCount (sizeof(kNamed) / sizeof(kNamed[0]))

/* s points at '&'. On success, returns 1 and sets *consumed (total input
   bytes including the leading '&' and the trailing ';') and *outbyte (the
   single decoded byte to emit). On failure -- unrecognized name, or no ';'
   found within a short, bounded lookahead (including running off the end
   of the string) -- returns 0 and consumes nothing; the caller emits '&'
   literally and resumes normal scanning from the next byte, which is what
   lets a truncated "...&amp" at the very end of the buffer degrade to
   literal text instead of anything going out of bounds. */
static int parse_entity(const char *s, unsigned long *consumed, int *outbyte)
{
    unsigned long ni;
    unsigned long limit = 12; /* generous bound on how far we search for ';' */

    for (ni = 0; ni < kNamedCount; ni++) {
        unsigned long nlen = 0;
        const char *name = kNamed[ni].name;

        while (name[nlen] != '\0') nlen++;

        if (ci_eq(s + 1, name, nlen) && s[1 + nlen] == ';') {
            *consumed = 1 + nlen + 1;
            *outbyte = (unsigned char)kNamed[ni].out;
            return 1;
        }
    }

    if (s[1] == '#') {
        unsigned long cp = 0;
        unsigned long j;
        unsigned long start;
        int is_hex = (s[2] == 'x' || s[2] == 'X');

        j = is_hex ? 3 : 2;
        start = j;

        if (is_hex) {
            while (j < limit && s[j] != '\0' && s[j] != ';' && hex_val(s[j]) >= 0) {
                cp = cp * 16 + (unsigned long)hex_val(s[j]);
                j++;
            }
        } else {
            while (j < limit && s[j] != '\0' && s[j] != ';' && is_digit(s[j])) {
                cp = cp * 10 + (unsigned long)(s[j] - '0');
                j++;
            }
        }

        if (j == start || s[j] != ';') {
            return 0;
        }

        *consumed = j + 1;
        *outbyte = (cp < 256) ? (int)(unsigned char)cp : '?';
        return 1;
    }

    return 0;
}

/* ---- pass 1: tags, entities, UTF-8 -> Latin-1 -------------------------- */

static void decode_pass(char *buf, unsigned long cap)
{
    unsigned long i = 0;
    unsigned long o = 0;

    while (buf[i] != '\0') {
        unsigned char c = (unsigned char)buf[i];

        if (o >= cap - 1) {
            break;
        }

        if (c == '<') {
            unsigned long j = i + 1;

            while (buf[j] != '\0' && buf[j] != '>') j++;

            if (buf[j] == '>') {
                unsigned long taglen = j - (i + 1);

                if (tag_is_newline(buf + i + 1, taglen)) {
                    emit(buf, cap, &o, '\n');
                }
                i = j + 1;
            } else {
                /* Unterminated tag runs off the end of the string: drop
                   the rest, nothing to emit. */
                i = j;
            }
            continue;
        }

        if (c == '&') {
            unsigned long consumed;
            int outbyte;

            if (parse_entity(buf + i, &consumed, &outbyte)) {
                emit(buf, cap, &o, (char)outbyte);
                i += consumed;
            } else {
                emit(buf, cap, &o, '&');
                i += 1;
            }
            continue;
        }

        if (c < 0x80) {
            emit(buf, cap, &o, (char)c);
            i += 1;
            continue;
        }

        /* UTF-8 multi-byte lead byte (or a stray/invalid byte). */
        {
            unsigned long need;   /* number of continuation bytes expected */
            unsigned long cp = 0;
            unsigned long k;
            int ok = 1;

            if ((c & 0xE0) == 0xC0) {
                need = 1;
                cp = c & 0x1Fu;
            } else if ((c & 0xF0) == 0xE0) {
                need = 2;
                cp = c & 0x0Fu;
            } else if ((c & 0xF8) == 0xF0) {
                need = 3;
                cp = c & 0x07u;
            } else {
                /* Stray continuation byte, or an otherwise invalid lead
                   byte (0xF8-0xFF): not valid UTF-8 here, one unknown
                   code point. */
                emit(buf, cap, &o, '?');
                i += 1;
                continue;
            }

            for (k = 1; k <= need; k++) {
                unsigned char b = (unsigned char)buf[i + k];

                if (b == '\0') {
                    ok = 0;
                    break;
                }
                if ((b & 0xC0) != 0x80) {
                    ok = 0;
                    break;
                }
            }

            if (!ok) {
                /* Truncated (ran into the NUL) or malformed multi-byte
                   sequence. Emit one '?' for the lead byte; if we hit the
                   real end of the buffer while checking, land exactly on
                   the NUL so the outer loop terminates cleanly, otherwise
                   just resync one byte forward. */
                emit(buf, cap, &o, '?');
                if (buf[i + k] == '\0') {
                    i += k;
                } else {
                    i += 1;
                }
                continue;
            }

            for (k = 1; k <= need; k++) {
                cp = (cp << 6) | ((unsigned char)buf[i + k] & 0x3Fu);
            }

            if (need == 1 && cp >= 0xA0 && cp <= 0xFF) {
                emit(buf, cap, &o, (char)cp);
            } else if (need == 2 && (cp == 0x2018 || cp == 0x2019)) {
                emit(buf, cap, &o, '\'');
            } else if (need == 2 && (cp == 0x201C || cp == 0x201D)) {
                emit(buf, cap, &o, '"');
            } else if (need == 2 && (cp == 0x2013 || cp == 0x2014)) {
                emit(buf, cap, &o, '-');
            } else if (need == 2 && cp == 0x2026) {
                emit(buf, cap, &o, '.');
                emit(buf, cap, &o, '.');
                emit(buf, cap, &o, '.');
            } else {
                emit(buf, cap, &o, '?');
            }

            i += need + 1;
        }
    }

    buf[o] = '\0';
}

/* ---- pass 2: collapse newline runs, trim whitespace -------------------- */

static int is_ws(char c)
{
    return c == ' ' || c == '\t' || c == '\r' || c == '\n';
}

static void collapse_and_trim(char *buf)
{
    unsigned long i = 0;
    unsigned long o = 0;
    unsigned long nlrun = 0;

    while (is_ws(buf[i])) i++;

    for (; buf[i] != '\0'; i++) {
        char c = buf[i];

        if (c == '\r') {
            continue; /* drop CR outright; also eats the wire's trailing \r\n */
        }
        if (c == '\n') {
            nlrun++;
            if (nlrun <= 2) {
                buf[o++] = c;
            }
            continue;
        }
        nlrun = 0;
        buf[o++] = c;
    }

    while (o > 0 && is_ws(buf[o - 1])) o--;

    buf[o] = '\0';
}

/* ---- public entry point ------------------------------------------------ */

void html_clean(char *buf, unsigned long cap)
{
    if (buf == 0 || cap == 0) {
        return;
    }
    decode_pass(buf, cap);
    collapse_and_trim(buf);
}
