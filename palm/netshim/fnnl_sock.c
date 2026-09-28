/* palm/netshim/fnnl_sock.c
 *
 * Table B entries for socket creation/option/control calls: SocketOpen,
 * SocketClose, SocketOptionSet/Get, SocketBind, SocketConnect,
 * SocketListen/Accept (both stubbed -- no TCP server support yet),
 * SocketShutdown, SocketAddr.
 *
 * Only netSocketTypeStream (TCP) sockets do real work; the FujiBus network
 * device doesn't distinguish "socket()" from "connect()" the way BSD
 * sockets do -- a FujiBus net unit only comes into existence once we know
 * the destination host:port, so SocketOpen() only reserves a slot (and
 * picks the FujiBus unit == slot+1) and the actual fn_net_open() happens in
 * SocketConnect().
 */
#include "fnnl_priv.h"

FnNLSocket *FnNLSlot(FnNLGlobals *g, NetSocketRef sock, Err *errP)
{
    UInt16 idx;

    if (g == NULL || g->openCount == 0) {
        if (errP != NULL) *errP = netErrNotOpen;
        return NULL;
    }
    if (sock < 1 || sock > FNNL_MAX_SOCKETS) {
        if (errP != NULL) *errP = netErrParamErr;
        return NULL;
    }
    idx = (UInt16)(sock - 1);
    if (!g->sockets[idx].inUse) {
        if (errP != NULL) *errP = netErrParamErr;
        return NULL;
    }
    return &g->sockets[idx];
}

NetSocketRef FnNLB_SocketOpen(UInt16 refNum, NetSocketAddrEnum domain,
                               NetSocketTypeEnum type, Int16 protocol,
                               Int32 timeout, Err *errP)
{
    FnNLGlobals *g = FnNLGetGlobals();
    UInt16       i;

    (void)refNum;
    (void)domain;
    (void)protocol;
    (void)timeout;

    if (g == NULL || g->openCount == 0) {
        if (errP != NULL) *errP = netErrNotOpen;
        return -1;
    }
    if (type != netSocketTypeStream) {
        if (errP != NULL) *errP = netErrParamErr;
        return -1;
    }

    for (i = 0; i < FNNL_MAX_SOCKETS; i++) {
        if (!g->sockets[i].inUse) {
            MemSet(&g->sockets[i], sizeof(g->sockets[i]), 0);
            g->sockets[i].inUse = true;
            g->sockets[i].type  = (UInt8)type;
            g->sockets[i].unit  = (UInt8)(i + 1);
            if (errP != NULL) *errP = errNone;
            return (NetSocketRef)(i + 1);
        }
    }

    if (errP != NULL) *errP = netErrNoMoreSockets;
    return -1;
}

Int16 FnNLB_SocketClose(UInt16 refNum, NetSocketRef socket, Int32 timeout, Err *errP)
{
    FnNLGlobals *g = FnNLGetGlobals();
    FnNLSocket  *s  = FnNLSlot(g, socket, errP);

    (void)refNum;
    (void)timeout;

    if (s == NULL)
        return -1;

    if (s->connected)
        fn_net_close(&g->ctx, s->unit);
    s->inUse = false;

    if (errP != NULL) *errP = errNone;
    return 0;
}

Int16 FnNLB_SocketOptionSet(UInt16 refNum, NetSocketRef socket,
                             UInt16 level, UInt16 option,
                             void *optValueP, UInt16 optValueLen,
                             Int32 timeout, Err *errP)
{
    FnNLGlobals *g = FnNLGetGlobals();
    FnNLSocket  *s  = FnNLSlot(g, socket, errP);

    (void)refNum;
    (void)level;
    (void)timeout;

    if (s == NULL)
        return -1;

    if (option == netSocketOptSockNonBlocking) {
        UInt16 v = 0;
        if (optValueP != NULL && optValueLen >= sizeof(UInt16))
            v = *(UInt16 *)optValueP;
        else if (optValueP != NULL && optValueLen >= sizeof(UInt8))
            v = *(UInt8 *)optValueP;
        s->nonBlocking = (Boolean)(v != 0);
    }
    /* All other options: accept silently (spec). */

    if (errP != NULL) *errP = errNone;
    return 0;
}

Int16 FnNLB_SocketOptionGet(UInt16 refNum, NetSocketRef socket,
                             UInt16 level, UInt16 option,
                             void *optValueP, UInt16 *optValueLenP,
                             Int32 timeout, Err *errP)
{
    FnNLGlobals *g = FnNLGetGlobals();
    FnNLSocket  *s  = FnNLSlot(g, socket, errP);
    UInt16       v;

    (void)refNum;
    (void)level;
    (void)timeout;

    if (s == NULL)
        return -1;

    v = (UInt16)((option == netSocketOptSockNonBlocking && s->nonBlocking) ? 1 : 0);

    if (optValueP != NULL && optValueLenP != NULL && *optValueLenP >= sizeof(UInt16)) {
        *(UInt16 *)optValueP = v;
        *optValueLenP = sizeof(UInt16);
    } else if (optValueLenP != NULL) {
        *optValueLenP = 0;
    }

    if (errP != NULL) *errP = errNone;
    return 0;
}

/* Real NetLib tracks the bound local address/port for wildcard-bind
 * servers; this shim has no server-side support yet (SocketListen/Accept
 * are stubbed out below) and SocketAddr() below always synthesizes a fixed
 * local address, so there is nothing meaningful to store -- just validate
 * the socket ref and succeed, per spec ("store and return 0"). */
Int16 FnNLB_SocketBind(UInt16 refNum, NetSocketRef socket,
                        NetSocketAddrType *sockAddrP, Int16 addrLen,
                        Int32 timeout, Err *errP)
{
    FnNLGlobals *g = FnNLGetGlobals();
    FnNLSocket  *s  = FnNLSlot(g, socket, errP);

    (void)refNum;
    (void)sockAddrP;
    (void)addrLen;
    (void)timeout;

    if (s == NULL)
        return -1;

    if (errP != NULL) *errP = errNone;
    return 0;
}

Int16 FnNLB_SocketConnect(UInt16 refNum, NetSocketRef socket,
                           NetSocketAddrType *sockAddrP, Int16 addrLen,
                           Int32 timeout, Err *errP)
{
    FnNLGlobals          *g = FnNLGetGlobals();
    FnNLSocket           *s = FnNLSlot(g, socket, errP);
    NetSocketAddrINType  *inAddr;
    char                  hostbuf[64];
    FnErr                 fe;

    (void)refNum;
    (void)addrLen;
    (void)timeout;

    if (s == NULL)
        return -1;
    if (sockAddrP == NULL) {
        if (errP != NULL) *errP = netErrParamErr;
        return -1;
    }

    inAddr = (NetSocketAddrINType *)sockAddrP;
    FnNLAddrToHostStr(g, inAddr->addr, hostbuf);

    StrPrintF(g->scratch, "N:TCP://%s:%u/", hostbuf, (unsigned int)inAddr->port);

    fe = fn_net_open(&g->ctx, s->unit, g->scratch, FN_OPEN_RW, FN_TRANS_NONE);
    if (fe != FN_OK) {
        g->lastErr = (Err)fe;
        if (errP != NULL)
            *errP = (fe == FN_ERR_TIMEOUT) ? netErrTimeout : netErrSocketNotConnected;
        return -1;
    }

    s->connected = true;
    s->remote = *inAddr;

    if (errP != NULL) *errP = errNone;
    return 0;
}

/* No TCP server support yet. */
Int16 FnNLB_SocketListen(UInt16 refNum, NetSocketRef socket, UInt16 queueLen,
                          Int32 timeout, Err *errP)
{
    (void)refNum;
    (void)socket;
    (void)queueLen;
    (void)timeout;
    if (errP != NULL) *errP = netErrParamErr;
    return -1;
}

Int16 FnNLB_SocketAccept(UInt16 refNum, NetSocketRef socket,
                          NetSocketAddrType *sockAddrP, Int16 *addrLenP,
                          Int32 timeout, Err *errP)
{
    (void)refNum;
    (void)socket;
    (void)sockAddrP;
    (void)addrLenP;
    (void)timeout;
    if (errP != NULL) *errP = netErrParamErr;
    return -1;
}

Int16 FnNLB_SocketShutdown(UInt16 refNum, NetSocketRef socket, Int16 direction,
                            Int32 timeout, Err *errP)
{
    FnNLGlobals *g = FnNLGetGlobals();
    FnNLSocket  *s  = FnNLSlot(g, socket, errP);

    (void)refNum;
    (void)direction;
    (void)timeout;

    if (s == NULL)
        return -1;

    if (errP != NULL) *errP = errNone;
    return 0;
}

Int16 FnNLB_SocketAddr(UInt16 refNum, NetSocketRef socket,
                        NetSocketAddrType *locAddrP, Int16 *locAddrLenP,
                        NetSocketAddrType *remAddrP, Int16 *remAddrLenP,
                        Int32 timeout, Err *errP)
{
    FnNLGlobals *g = FnNLGetGlobals();
    FnNLSocket  *s  = FnNLSlot(g, socket, errP);

    (void)refNum;
    (void)timeout;

    if (s == NULL)
        return -1;

    if (locAddrP != NULL) {
        NetSocketAddrINType loc;
        MemSet(&loc, sizeof(loc), 0);
        loc.family = netSocketAddrINET;
        loc.port = (UInt16)(1024 + (socket - 1));
        loc.addr = ((NetIPAddr)10 << 24) | ((NetIPAddr)2);   /* 10.0.0.2 */
        MemMove(locAddrP, &loc, sizeof(loc));
        if (locAddrLenP != NULL) *locAddrLenP = sizeof(loc);
    }
    if (remAddrP != NULL) {
        MemMove(remAddrP, &s->remote, sizeof(s->remote));
        if (remAddrLenP != NULL) *remAddrLenP = sizeof(s->remote);
    }

    if (errP != NULL) *errP = errNone;
    return 0;
}
