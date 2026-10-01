/* palm/netshim/fnnl_glob.c
 *
 * The one dynamically-allocated globals block (see fnnl_priv.h for why this
 * library can't use ordinary static globals), reached via FtrGet/FtrSet
 * under (fnNLCreator, 0) rather than SysLibTblEntry->globalsP so that it's
 * reachable from table B's trap handlers, which run under the ROM Net.lib's
 * refNum, not ours. Also: error-code mapping, dotted-quad/host-string
 * helpers, and the fake host table used by GetHostByName()/SocketConnect().
 */
#include "fnnl_priv.h"

#define FUJI_PALMOS_PREF_CREATOR ((UInt32)'FNCF')
#define FUJI_PALMOS_PREF_ID      0
#define FUJI_PALMOS_PREF_VERSION 1

typedef struct {
    UInt16 version;
    UInt32 baud;
    char   lib_name[32];
} FnNLLinkPref;

#define FNNL_DEFAULT_LIB  "Serial Library"
#define FNNL_DEFAULT_BAUD 115200UL

FnNLGlobals *FnNLGetGlobals(void)
{
    UInt32 v;

    if (FtrGet(fnNLCreator, 0, &v) != errNone)
        return NULL;
    return (FnNLGlobals *)v;
}

FnNLGlobals *FnNLGetOrCreateGlobals(void)
{
    FnNLGlobals *g;
    UInt32       v;

    if (FtrGet(fnNLCreator, 0, &v) == errNone)
        return (FnNLGlobals *)v;

    g = (FnNLGlobals *)MemPtrNew(sizeof(FnNLGlobals));
    if (g == NULL)
        return NULL;

    MemSet(g, sizeof(FnNLGlobals), 0);
    MemPtrSetOwner((MemPtr)g, 0);

    if (FtrSet(fnNLCreator, 0, (UInt32)g) != errNone) {
        MemPtrFree((MemPtr)g);
        return NULL;
    }

    return g;
}

Err FnNLMapFnErr(FnErr e)
{
    switch (e) {
        case FN_OK:            return errNone;
        case FN_ERR_TIMEOUT:   return netErrTimeout;
        case FN_ERR_NAK:       return netErrUnexpectedCmd;
        case FN_ERR_CHECKSUM:  return netErrInternal;
        case FN_ERR_LENGTH:    return netErrInternal;
        case FN_ERR_DEVICE:    return netErrInternal;
        case FN_ERR_IO:        return netErrInterfaceNotFound;
        case FN_ERR_OVERFLOW:  return netErrMessageTooBig;
        case FN_ERR_PARAM:     return netErrParamErr;
        default:               return netErrInternal;
    }
}

Err FnNLMapSerErr(Err e)
{
    if (e == errNone)
        return errNone;
    /* Most fn_ser_open() failures are "couldn't find/open the serial
     * library" -- the closest NetMgr.h concept is "the interface (driver)
     * isn't there". */
    return netErrInterfaceNotFound;
}

void FnNLAddrToDotted(NetIPAddr addr, Char *buf)
{
    /* NetIPAddr is already in network byte order, which on this
     * big-endian m68k target is identical to host order, so the MSB of
     * the UInt32 is octet 0 with no byte-swapping needed either way. */
    UInt8 a = (UInt8)((addr >> 24) & 0xFF);
    UInt8 b = (UInt8)((addr >> 16) & 0xFF);
    UInt8 c = (UInt8)((addr >> 8) & 0xFF);
    UInt8 d = (UInt8)(addr & 0xFF);

    StrPrintF(buf, "%d.%d.%d.%d", (int)a, (int)b, (int)c, (int)d);
}

Boolean FnNLParseDotted(const Char *s, NetIPAddr *outP)
{
    UInt32 parts[4];
    UInt16 partIdx = 0;
    UInt32 cur = 0;
    Boolean haveDigit = false;
    const Char *p = s;

    if (s == NULL)
        return false;

    for (;;) {
        Char ch = *p;

        if (ch >= '0' && ch <= '9') {
            cur = cur * 10 + (UInt32)(ch - '0');
            if (cur > 255)
                return false;
            haveDigit = true;
        } else if (ch == '.' || ch == '\0') {
            if (!haveDigit || partIdx >= 4)
                return false;
            parts[partIdx++] = cur;
            cur = 0;
            haveDigit = false;
            if (ch == '\0')
                break;
        } else {
            return false;
        }
        p++;
    }

    if (partIdx != 4)
        return false;

    *outP = (parts[0] << 24) | (parts[1] << 16) | (parts[2] << 8) | parts[3];
    return true;
}

static Boolean FnNLNameEq(const Char *a, const Char *b)
{
    /* Case-insensitive compare; StrCaselessCompare is the Palm OS
     * StringMgr equivalent of strcasecmp(). */
    return StrCaselessCompare(a, b) == 0;
}

NetIPAddr FnNLFakeHostLookup(FnNLGlobals *g, const Char *name)
{
    UInt16 i;
    UInt16 slot;

    for (i = 0; i < FNNL_MAX_FAKE_HOSTS; i++) {
        if (g->fakeHosts[i].name[0] != '\0' && FnNLNameEq(g->fakeHosts[i].name, name)) {
            slot = i;
            return ((NetIPAddr)FNNL_FAKE_HOST_A << 24) | ((NetIPAddr)FNNL_FAKE_HOST_B << 16) |
                   ((NetIPAddr)FNNL_FAKE_HOST_C << 8) | (NetIPAddr)(slot + 1);
        }
    }

    /* Not found: prefer an empty slot, else evict round-robin. */
    slot = FNNL_MAX_FAKE_HOSTS; /* sentinel: none found yet */
    for (i = 0; i < FNNL_MAX_FAKE_HOSTS; i++) {
        if (g->fakeHosts[i].name[0] == '\0') {
            slot = i;
            break;
        }
    }
    if (slot == FNNL_MAX_FAKE_HOSTS) {
        slot = g->fakeHostNext;
        g->fakeHostNext = (UInt16)((g->fakeHostNext + 1) % FNNL_MAX_FAKE_HOSTS);
    }

    StrNCopy(g->fakeHosts[slot].name, name, sizeof(g->fakeHosts[slot].name) - 1);
    g->fakeHosts[slot].name[sizeof(g->fakeHosts[slot].name) - 1] = '\0';

    return ((NetIPAddr)FNNL_FAKE_HOST_A << 24) | ((NetIPAddr)FNNL_FAKE_HOST_B << 16) |
           ((NetIPAddr)FNNL_FAKE_HOST_C << 8) | (NetIPAddr)(slot + 1);
}

Int16 FnNLFakeHostReverse(FnNLGlobals *g, NetIPAddr addr)
{
    UInt8 a = (UInt8)((addr >> 24) & 0xFF);
    UInt8 b = (UInt8)((addr >> 16) & 0xFF);
    UInt8 c = (UInt8)((addr >> 8) & 0xFF);
    UInt8 d = (UInt8)(addr & 0xFF);
    Int16 slot;

    if (a != FNNL_FAKE_HOST_A || b != FNNL_FAKE_HOST_B || c != FNNL_FAKE_HOST_C)
        return -1;
    if (d < 1 || d > FNNL_MAX_FAKE_HOSTS)
        return -1;

    slot = (Int16)(d - 1);
    if (g->fakeHosts[slot].name[0] == '\0')
        return -1;

    return slot;
}

void FnNLAddrToHostStr(FnNLGlobals *g, NetIPAddr addr, Char *buf)
{
    Int16 slot = FnNLFakeHostReverse(g, addr);

    if (slot >= 0) {
        StrNCopy(buf, g->fakeHosts[slot].name, 63);
        buf[63] = '\0';
    } else {
        FnNLAddrToDotted(addr, buf);
    }
}

void FnNLReadLinkPref(Char *libNameBuf, UInt32 *baudP)
{
    FnNLLinkPref pref;
    UInt16       size;
    UInt16       version;

    MemSet(&pref, sizeof(pref), 0);
    size = sizeof(pref);
    version = PrefGetAppPreferences(FUJI_PALMOS_PREF_CREATOR, FUJI_PALMOS_PREF_ID,
                                     &pref, &size, false);

    if (version == FUJI_PALMOS_PREF_VERSION && size >= sizeof(pref)) {
        pref.lib_name[sizeof(pref.lib_name) - 1] = '\0';
        StrNCopy(libNameBuf, pref.lib_name, 31);
        libNameBuf[31] = '\0';
        *baudP = pref.baud;
    } else {
        StrNCopy(libNameBuf, FNNL_DEFAULT_LIB, 31);
        libNameBuf[31] = '\0';
        *baudP = FNNL_DEFAULT_BAUD;
    }
}
