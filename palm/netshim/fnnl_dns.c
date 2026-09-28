/* palm/netshim/fnnl_dns.c
 *
 * Table B entries for name/service lookups: GetHostByName, GetHostByAddr,
 * GetServByName, GetMailExchangeByName.
 *
 * FujiNet resolves hostnames itself (SocketConnect just hands it a
 * "N:TCP://host:port/" devicespec) so GetHostByName() never has to do a
 * real DNS round trip: a dotted-quad name is parsed as-is, and anything
 * else is assigned a synthetic 198.18.0.x address from fnnl_glob.c's fake
 * host table (198.18.0.0/15 is IANA-reserved for benchmarking, so it can't
 * collide with a real address) purely so callers get back *something* that
 * round-trips through NetLibAddrINToA/SocketConnect -- resolution only
 * really happens later, at connect time.
 */
#include "fnnl_priv.h"

NetHostInfoPtr FnNLB_GetHostByName(UInt16 refNum, Char *nameP, NetHostInfoBufPtr bufP,
                                    Int32 timeout, Err *errP)
{
    FnNLGlobals *g = FnNLGetGlobals();
    NetIPAddr    addr;
    UInt16       len;

    (void)refNum;
    (void)timeout;

    if (g == NULL) {
        if (errP != NULL) *errP = netErrNotOpen;
        return NULL;
    }
    if (nameP == NULL || bufP == NULL) {
        if (errP != NULL) *errP = netErrParamErr;
        return NULL;
    }

    MemSet(bufP, sizeof(*bufP), 0);

    len = (UInt16)StrLen(nameP);
    if (len > netDNSMaxDomainName)
        len = netDNSMaxDomainName;
    MemMove(bufP->name, nameP, len);
    bufP->name[len] = '\0';

    if (!FnNLParseDotted(nameP, &addr))
        addr = FnNLFakeHostLookup(g, nameP);

    bufP->aliasList[0] = NULL;
    bufP->address[0] = addr;
    bufP->addressList[0] = &bufP->address[0];
    bufP->addressList[1] = NULL;

    bufP->hostInfo.nameP = bufP->name;
    bufP->hostInfo.nameAliasesP = bufP->aliasList;
    bufP->hostInfo.addrType = netSocketAddrINET;
    bufP->hostInfo.addrLen = 4;
    bufP->hostInfo.addrListP = (UInt8 **)bufP->addressList;

    if (errP != NULL) *errP = errNone;
    return &bufP->hostInfo;
}

NetHostInfoPtr FnNLB_GetHostByAddr(UInt16 refNum, UInt8 *addrP, UInt16 len, UInt16 type,
                                    NetHostInfoBufPtr bufP, Int32 timeout, Err *errP)
{
    FnNLGlobals *g = FnNLGetGlobals();
    NetIPAddr    addr;
    Int16        slot;

    (void)refNum;
    (void)timeout;
    (void)type;

    if (g == NULL) {
        if (errP != NULL) *errP = netErrNotOpen;
        return NULL;
    }
    if (addrP == NULL || bufP == NULL || len < 4) {
        if (errP != NULL) *errP = netErrParamErr;
        return NULL;
    }

    addr = ((NetIPAddr)addrP[0] << 24) | ((NetIPAddr)addrP[1] << 16) |
           ((NetIPAddr)addrP[2] << 8) | (NetIPAddr)addrP[3];

    MemSet(bufP, sizeof(*bufP), 0);

    slot = FnNLFakeHostReverse(g, addr);
    if (slot >= 0) {
        StrNCopy(bufP->name, g->fakeHosts[slot].name, netDNSMaxDomainName);
    } else {
        FnNLAddrToDotted(addr, bufP->name);
    }

    bufP->aliasList[0] = NULL;
    bufP->address[0] = addr;
    bufP->addressList[0] = &bufP->address[0];
    bufP->addressList[1] = NULL;

    bufP->hostInfo.nameP = bufP->name;
    bufP->hostInfo.nameAliasesP = bufP->aliasList;
    bufP->hostInfo.addrType = netSocketAddrINET;
    bufP->hostInfo.addrLen = 4;
    bufP->hostInfo.addrListP = (UInt8 **)bufP->addressList;

    if (errP != NULL) *errP = errNone;
    return &bufP->hostInfo;
}

/* A small fixed service table. Deliberately a const array of {char[8],
 * UInt16} PODs rather than an array of {char*, UInt16} -- see fnnl_priv.h's
 * header comment: a pointer table would need load-time relocations that
 * nothing patches for a ROM-resident system library, while inline char
 * arrays are plain data addressed PC-relative like any other const. */
typedef struct {
    char   name[8];
    UInt16 port;
} FnNLServEntry;

static const FnNLServEntry kFnNLServices[] = {
    { "echo",   7 },
    { "ftp",    21 },
    { "telnet", 23 },
    { "smtp",   25 },
    { "finger", 79 },
    { "http",   80 },
    { "pop3",   110 },
    { "nntp",   119 },
    { "imap",   143 },
    { "irc",    6667 }
};
#define kFnNLNumServices (sizeof(kFnNLServices) / sizeof(kFnNLServices[0]))

NetServInfoPtr FnNLB_GetServByName(UInt16 refNum, const Char *servNameP, const Char *protoNameP,
                                    NetServInfoBufPtr bufP, Int32 timeout, Err *errP)
{
    UInt16 i;

    (void)refNum;
    (void)protoNameP;
    (void)timeout;

    if (servNameP == NULL || bufP == NULL) {
        if (errP != NULL) *errP = netErrParamErr;
        return NULL;
    }

    MemSet(bufP, sizeof(*bufP), 0);

    for (i = 0; i < kFnNLNumServices; i++) {
        if (StrCaselessCompare(servNameP, kFnNLServices[i].name) == 0) {
            StrNCopy(bufP->name, kFnNLServices[i].name, sizeof(bufP->name) - 1);
            bufP->aliasList[0] = NULL;
            StrNCopy(bufP->protoName, "tcp", sizeof(bufP->protoName) - 1);

            bufP->servInfo.nameP = bufP->name;
            bufP->servInfo.nameAliasesP = bufP->aliasList;
            bufP->servInfo.port = kFnNLServices[i].port;
            bufP->servInfo.protoP = bufP->protoName;

            if (errP != NULL) *errP = errNone;
            return &bufP->servInfo;
        }
    }

    if (errP != NULL) *errP = netErrUnknownService;
    return NULL;
}

Int16 FnNLB_GetMailExchangeByName(UInt16 refNum, Char *mailNameP, UInt16 maxEntries,
                                   Char hostNames[][netDNSMaxDomainName + 1], UInt16 priorities[],
                                   Int32 timeout, Err *errP)
{
    (void)refNum;
    (void)timeout;

    if (mailNameP == NULL || maxEntries == 0 || hostNames == NULL || priorities == NULL) {
        if (errP != NULL) *errP = netErrParamErr;
        return -1;
    }

    StrNCopy(hostNames[0], mailNameP, netDNSMaxDomainName);
    hostNames[0][netDNSMaxDomainName] = '\0';
    priorities[0] = 0;

    if (errP != NULL) *errP = errNone;
    return 1;
}
