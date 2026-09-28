/* palm/netshim/fnnl_io.c
 *
 * Table B entries for data transfer: Send/SendPB, Receive/ReceivePB,
 * DmReceive, and Select.
 *
 * Receive semantics come directly from Step 0's live observations against
 * fujinet-pc (see host/fnhost.c's `tcp` command and the task report): while
 * a TCP devicespec has data queued, STATUS keeps reporting avail>0 even
 * after the remote has closed (connected drops to 0 immediately once the
 * peer FIN's, independent of how much buffered data is still waiting to be
 * drained) and err==136 once the peer has closed. So "readable" is
 * avail>0 OR connected==0 (EOF), and a real end-of-stream (Receive
 * returning 0, matching NetLib's own EOF convention) is only avail==0 AND
 * connected==0 together.
 */
#include "fnnl_priv.h"

Int16 FnNLB_Send(UInt16 refNum, NetSocketRef socket, void *bufP, UInt16 bufLen,
                  UInt16 flags, void *toAddrP, UInt16 toLen, Int32 timeout, Err *errP)
{
    FnNLGlobals *g = FnNLGetGlobals();
    FnNLSocket  *s  = FnNLSlot(g, socket, errP);
    UInt16       total = 0;
    FnErr        fe = FN_OK;

    (void)refNum;
    (void)flags;
    (void)toAddrP;
    (void)toLen;
    (void)timeout;

    if (s == NULL)
        return -1;
    if (!s->connected) {
        if (errP != NULL) *errP = netErrSocketNotConnected;
        return -1;
    }
    if (bufP == NULL || bufLen == 0) {
        if (errP != NULL) *errP = errNone;
        return 0;
    }

    while (total < bufLen) {
        UInt16 remain = (UInt16)(bufLen - total);
        UInt16 chunk = (remain > FN_MAX_DATA) ? FN_MAX_DATA : remain;
        fe = fn_net_write(&g->ctx, s->unit, (UInt8 *)bufP + total, chunk);
        if (fe != FN_OK)
            break;
        total = (UInt16)(total + chunk);
    }

    if (total == 0 && fe != FN_OK) {
        g->lastErr = (Err)fe;
        if (errP != NULL) *errP = FnNLMapFnErr(fe);
        return -1;
    }

    if (errP != NULL) *errP = errNone;
    return (Int16)total;
}

Int16 FnNLB_SendPB(UInt16 refNum, NetSocketRef socket, NetIOParamType *pbP,
                    UInt16 flags, Int32 timeout, Err *errP)
{
    FnNLGlobals *g = FnNLGetGlobals();
    FnNLSocket  *s  = FnNLSlot(g, socket, errP);
    UInt16       i;
    UInt16       total = 0;
    FnErr        fe = FN_OK;

    (void)refNum;
    (void)flags;
    (void)timeout;

    if (s == NULL)
        return -1;
    if (!s->connected) {
        if (errP != NULL) *errP = netErrSocketNotConnected;
        return -1;
    }
    if (pbP == NULL || pbP->iov == NULL || pbP->iovLen > netIOVecMaxLen) {
        if (errP != NULL) *errP = netErrParamErr;
        return -1;
    }

    for (i = 0; i < pbP->iovLen; i++) {
        UInt8  *bufP = pbP->iov[i].bufP;
        UInt16  bufLen = pbP->iov[i].bufLen;
        UInt16  sent = 0;

        while (sent < bufLen) {
            UInt16 remain = (UInt16)(bufLen - sent);
            UInt16 chunk = (remain > FN_MAX_DATA) ? FN_MAX_DATA : remain;
            fe = fn_net_write(&g->ctx, s->unit, bufP + sent, chunk);
            if (fe != FN_OK)
                break;
            sent = (UInt16)(sent + chunk);
        }
        total = (UInt16)(total + sent);
        if (fe != FN_OK)
            break;
    }

    if (total == 0 && fe != FN_OK) {
        g->lastErr = (Err)fe;
        if (errP != NULL) *errP = FnNLMapFnErr(fe);
        return -1;
    }

    if (errP != NULL) *errP = errNone;
    return (Int16)total;
}

Int16 FnNLB_Receive(UInt16 refNum, NetSocketRef socket, void *bufP, UInt16 bufLen,
                     UInt16 flags, void *fromAddrP, UInt16 *fromLenP,
                     Int32 timeout, Err *errP)
{
    FnNLGlobals *g = FnNLGetGlobals();
    FnNLSocket  *s  = FnNLSlot(g, socket, errP);
    Boolean      infinite = (timeout < 0);
    UInt32       deadline = 0;

    (void)refNum;
    (void)flags;

    if (s == NULL)
        return -1;
    if (bufP == NULL || bufLen == 0) {
        if (errP != NULL) *errP = netErrParamErr;
        return -1;
    }

    if (!infinite)
        deadline = TimGetTicks() + (UInt32)timeout;

    for (;;) {
        fn_u16 avail = 0, got = 0;
        fn_u8  connected = 0, ferr = 0;
        FnErr  fe = fn_net_status(&g->ctx, s->unit, &avail, &connected, &ferr);

        if (fe != FN_OK) {
            g->lastErr = (Err)fe;
            if (errP != NULL) *errP = FnNLMapFnErr(fe);
            return -1;
        }

        if (avail > 0) {
            UInt16 want = bufLen;
            if (avail < want) want = avail;
            if (want > FN_MAX_DATA) want = FN_MAX_DATA;

            fe = fn_net_read_avail(&g->ctx, s->unit, bufP, want, &got, &ferr);
            if (fe != FN_OK) {
                g->lastErr = (Err)fe;
                if (errP != NULL) *errP = FnNLMapFnErr(fe);
                return -1;
            }
            if (fromAddrP != NULL) {
                *(NetSocketAddrINType *)fromAddrP = s->remote;
                if (fromLenP != NULL) *fromLenP = sizeof(NetSocketAddrINType);
            }
            if (connected == 0)
                s->connected = false;
            if (errP != NULL) *errP = errNone;
            return (Int16)got;
        }

        if (connected == 0) {
            s->connected = false;
            if (errP != NULL) *errP = errNone;
            return 0; /* EOF */
        }

        if (s->nonBlocking) {
            if (errP != NULL) *errP = netErrWouldBlock;
            return -1;
        }

        if (timeout == 0) {
            if (errP != NULL) *errP = netErrTimeout;
            return -1;
        }
        if (!infinite && (Int32)(TimGetTicks() - deadline) >= 0) {
            if (errP != NULL) *errP = netErrTimeout;
            return -1;
        }

        SysTaskDelay(2);
        EvtResetAutoOffTimer();
    }
}

Int16 FnNLB_ReceivePB(UInt16 refNum, NetSocketRef socket, NetIOParamType *pbP,
                       UInt16 flags, Int32 timeout, Err *errP)
{
    FnNLGlobals *g = FnNLGetGlobals();
    FnNLSocket  *s  = FnNLSlot(g, socket, errP);
    Boolean      infinite = (timeout < 0);
    UInt32       deadline = 0;
    UInt16       i;
    UInt16       total = 0;

    (void)refNum;
    (void)flags;

    if (s == NULL)
        return -1;
    if (pbP == NULL || pbP->iov == NULL || pbP->iovLen > netIOVecMaxLen) {
        if (errP != NULL) *errP = netErrParamErr;
        return -1;
    }

    if (!infinite)
        deadline = TimGetTicks() + (UInt32)timeout;

    for (i = 0; i < pbP->iovLen; i++) {
        UInt8  *bufP = pbP->iov[i].bufP;
        UInt16  bufLen = pbP->iov[i].bufLen;
        UInt16  filled = 0;

        while (filled < bufLen) {
            fn_u16 avail = 0, got = 0;
            fn_u8  connected = 0, ferr = 0;
            FnErr  fe = fn_net_status(&g->ctx, s->unit, &avail, &connected, &ferr);

            if (fe != FN_OK) {
                g->lastErr = (Err)fe;
                if (errP != NULL) *errP = FnNLMapFnErr(fe);
                return (total > 0) ? (Int16)total : -1;
            }

            if (avail > 0) {
                UInt16 want = (UInt16)(bufLen - filled);
                if (avail < want) want = avail;
                if (want > FN_MAX_DATA) want = FN_MAX_DATA;

                fe = fn_net_read_avail(&g->ctx, s->unit, bufP + filled, want, &got, &ferr);
                if (fe != FN_OK) {
                    g->lastErr = (Err)fe;
                    if (errP != NULL) *errP = FnNLMapFnErr(fe);
                    return (total > 0) ? (Int16)total : -1;
                }
                filled = (UInt16)(filled + got);
                total = (UInt16)(total + got);
                continue;
            }

            if (connected == 0) {
                s->connected = false;
                if (errP != NULL) *errP = errNone;
                return (Int16)total;
            }

            if (s->nonBlocking) {
                if (total > 0) {
                    if (errP != NULL) *errP = errNone;
                    return (Int16)total;
                }
                if (errP != NULL) *errP = netErrWouldBlock;
                return -1;
            }

            if (!infinite && (Int32)(TimGetTicks() - deadline) >= 0) {
                if (total > 0) {
                    if (errP != NULL) *errP = errNone;
                    return (Int16)total;
                }
                if (errP != NULL) *errP = netErrTimeout;
                return -1;
            }

            SysTaskDelay(2);
            EvtResetAutoOffTimer();
        }
    }

    if (errP != NULL) *errP = errNone;
    return (Int16)total;
}

/* Receives into the library's own scratch buffer (bounded to its size, and
 * to FN_MAX_DATA/rcvLen), then DmWrite()s that into the caller's
 * already-open record pointer -- Data Manager records are protected memory
 * even when you hold a raw pointer into one, so a direct fn_net_read_avail
 * straight into recordP would fault; DmWrite is the sanctioned way to
 * modify record bytes through that protection. */
Int16 FnNLB_DmReceive(UInt16 refNum, NetSocketRef socket, void *recordP,
                       UInt32 recordOffset, UInt16 rcvLen, UInt16 flags,
                       void *fromAddrP, UInt16 *fromLenP, Int32 timeout, Err *errP)
{
    FnNLGlobals *g = FnNLGetGlobals();
    UInt16       want;
    Int16        got;
    Err          werr;

    if (g == NULL) {
        if (errP != NULL) *errP = netErrNotOpen;
        return -1;
    }
    if (recordP == NULL) {
        if (errP != NULL) *errP = netErrParamErr;
        return -1;
    }

    want = rcvLen;
    if (want > sizeof(g->scratch))
        want = (UInt16)sizeof(g->scratch);

    got = FnNLB_Receive(refNum, socket, g->scratch, want, flags, fromAddrP, fromLenP, timeout, errP);
    if (got <= 0)
        return got;

    werr = DmWrite(recordP, recordOffset, g->scratch, (UInt32)got);
    if (werr != errNone) {
        if (errP != NULL) *errP = netErrInternal;
        return -1;
    }

    return got;
}

Int16 FnNLB_Select(UInt16 refNum, UInt16 width, NetFDSetType *readFDs,
                    NetFDSetType *writeFDs, NetFDSetType *exceptFDs,
                    Int32 timeout, Err *errP)
{
    FnNLGlobals  *g = FnNLGetGlobals();
    NetFDSetType  inRead  = (readFDs != NULL) ? *readFDs : 0;
    NetFDSetType  inWrite = (writeFDs != NULL) ? *writeFDs : 0;
    NetFDSetType  outRead, outWrite;
    Boolean       infinite = (timeout < 0);
    UInt32        deadline = 0;
    UInt16        count;

    (void)refNum;

    if (g == NULL || g->openCount == 0) {
        if (errP != NULL) *errP = netErrNotOpen;
        return -1;
    }
    if (width > netFDSetSize)
        width = netFDSetSize;

    if (!infinite)
        deadline = TimGetTicks() + (UInt32)timeout;

    for (;;) {
        UInt16 n;

        outRead = 0;
        outWrite = 0;
        count = 0;

        for (n = 0; n < width; n++) {
            if ((inRead & (1UL << n)) != 0) {
                Boolean ready = false;

                if (n == sysFileDescStdIn) {
                    ready = (Boolean)(EvtSysEventAvail(false) || EvtEventAvail());
                } else if (n >= 1 && n <= FNNL_MAX_SOCKETS && g->sockets[n - 1].inUse) {
                    fn_u16 avail = 0;
                    fn_u8  connected = 0, ferr = 0;
                    FnErr  fe = fn_net_status(&g->ctx, g->sockets[n - 1].unit, &avail, &connected, &ferr);
                    /* A failed STATUS also counts as ready, so the app's
                     * next Receive reports the error instead of this loop
                     * spinning forever on a dead link. */
                    ready = (Boolean)(fe != FN_OK || avail > 0 || connected == 0);
                }

                if (ready) {
                    outRead |= (1UL << n);
                    count++;
                }
            }

            if ((inWrite & (1UL << n)) != 0) {
                if (n >= 1 && n <= FNNL_MAX_SOCKETS &&
                    g->sockets[n - 1].inUse && g->sockets[n - 1].connected) {
                    outWrite |= (1UL << n);
                    count++;
                }
            }
        }

        if (count > 0)
            break;
        if (timeout == 0)
            break; /* one-shot poll, per BSD select() semantics */
        if (!infinite && (Int32)(TimGetTicks() - deadline) >= 0)
            break; /* timed out */

        SysTaskDelay(2);
        EvtResetAutoOffTimer();
    }

    if (readFDs != NULL) *readFDs = outRead;
    if (writeFDs != NULL) *writeFDs = outWrite;
    if (exceptFDs != NULL) *exceptFDs = 0;
    /* Real NetLib: timing out with nothing ready is not itself an error;
     * count==0 already communicates that. */
    if (errP != NULL) *errP = errNone;

    return (Int16)count;
}
