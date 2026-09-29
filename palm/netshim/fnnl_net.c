/* palm/netshim/fnnl_net.c
 *
 * Table B (the "Net.lib" emulation dispatch table) entries for library
 * lifecycle: Open/Close/Sleep/Wake, OpenCount/FinishCloseWait/
 * OpenIfCloseWait/HandlePowerOff/ConnectionRefresh, plus the two address
 * conversion routines (AddrINToA/AddrAToIN) and the generic zero/param-err
 * stubs shared by all the traps table B implements only as harmless no-ops
 * (see fnnl_priv.h's header comment on why sharing one code address across
 * several dispatch-table entries is fine).
 *
 * Every function here takes Net.lib's refNum (found via SysLibFind), never
 * this shim's own -- the OS dispatcher resolved the call through WHICHEVER
 * SysLibTblEntry the caller's refNum names, which after FnNLInstall() is
 * Net.lib's entry with our table spliced in; refNum itself is untouched
 * plumbing, not looked at here except to pass through to chain-through
 * calls (fnnl_bits.c).
 */
#include "fnnl_priv.h"

/* ---- shared trivial stubs -------------------------------------------- */

/* Used for every table B trap the spec says to answer with plain success:
 * Sleep, Wake, PrefsGet, PrefsSet, PrefsAppend, DrvrWake, IFAttach,
 * IFDetach, IFUp, IFDown, IFMediaUp, IFMediaDown, HandlePowerOff,
 * TracePrintF, TracePutS, ScriptExecute, ScriptExecuteV32, and
 * InterfacePtr (whose "NULL" return is also all-bits-zero, i.e. D0=0). None
 * of these look at their arguments, so one zero-arg function can stand in
 * for all of them regardless of each one's real signature -- the caller
 * pushed the args and the caller pops them; this stub only touches D0. */
Err FnNLB_ReturnZero(void)
{
    return 0;
}

/* Used for every table B trap the spec leaves as "not implemented yet":
 * Master and the nine NetLibConfigXxx/OpenConfig calls. */
Err FnNLB_ReturnParamErr(void)
{
    return netErrParamErr;
}

/* ---- lifecycle --------------------------------------------------------- */

Err FnNLB_Open(UInt16 refNum, UInt16 *netIFErrsP)
{
    FnNLGlobals *g = FnNLGetOrCreateGlobals();
    Err          err;

    (void)refNum;

    if (g == NULL)
        return netErrOutOfMemory;

    if (netIFErrsP != NULL)
        *netIFErrsP = 0;

    if (g->openCount == 0) {
        char   libName[32];
        UInt32 baud;

        FnNLReadLinkPref(libName, &baud);

        err = fn_ser_open(&g->port, libName, baud);
        if (err != errNone) {
            g->lastErr = err;
            return FnNLMapSerErr(err);
        }

        if (g->port.rxBuf != NULL)
            MemPtrSetOwner(g->port.rxBuf, 0);

        fn_ser_transport(&g->port, &g->transport);
        fn_init(&g->ctx, &g->transport);
        g->linkOpen = true;
    }

    g->openCount++;
    return errNone;
}

Err FnNLB_Close(UInt16 refNum, UInt16 immediate)
{
    FnNLGlobals *g = FnNLGetGlobals();

    (void)refNum;
    (void)immediate;

    if (g == NULL || g->openCount == 0)
        return netErrNotOpen;

    g->openCount--;
    if (g->openCount == 0)
        FnNLShutdownLink(g);

    return errNone;
}

void FnNLShutdownLink(FnNLGlobals *g)
{
    UInt16 i;

    for (i = 0; i < FNNL_MAX_SOCKETS; i++) {
        if (g->sockets[i].inUse) {
            if (g->linkOpen && g->sockets[i].connected)
                fn_net_close(&g->ctx, g->sockets[i].unit);
            g->sockets[i].inUse = false;
        }
    }
    if (g->linkOpen)
        fn_ser_close(&g->port);
    g->linkOpen = false;
    g->openCount = 0;
}

Err FnNLB_OpenCount(UInt16 refNum, UInt16 *countP)
{
    FnNLGlobals *g = FnNLGetGlobals();

    (void)refNum;
    if (countP != NULL)
        *countP = (g != NULL) ? g->openCount : 0;
    return errNone;
}

/* Real NetLib docs: "forces the library to complete a close if it's
 * currently in the close-wait state." This shim closes synchronously in
 * FnNLB_Close (no close-wait state ever exists), so there is never
 * anything to finish -- always report "already closed". */
Err FnNLB_FinishCloseWait(UInt16 refNum)
{
    (void)refNum;
    return errNone;
}

/* Real NetLib docs: "for use by the Network preference panel only... if
 * it's not in the close wait state, it returns an error code." Since this
 * shim never enters a close-wait state (see FinishCloseWait above), that
 * error case is the ONLY case -- always refuse. Spec text left the exact
 * choice to us; netErrNotOpen matches what real NetLib returns for this
 * same "not in close-wait" condition. */
Err FnNLB_OpenIfCloseWait(UInt16 refNum)
{
    (void)refNum;
    return netErrNotOpen;
}

Err FnNLB_ConnectionRefresh(UInt16 refNum, Boolean refresh, UInt8 *allInterfacesUpP, UInt16 *netIFErrP)
{
    FnNLGlobals *g = FnNLGetGlobals();

    (void)refNum;
    (void)refresh;

    if (allInterfacesUpP != NULL)
        *allInterfacesUpP = (UInt8)((g != NULL && g->linkOpen) ? 1 : 0);
    if (netIFErrP != NULL)
        *netIFErrP = 0;

    return errNone;
}

/* ---- address conversion ------------------------------------------------ */

Char *FnNLB_AddrINToA(UInt16 refNum, NetIPAddr inet, Char *spaceP)
{
    (void)refNum;
    if (spaceP != NULL)
        FnNLAddrToDotted(inet, spaceP);
    return spaceP;
}

NetIPAddr FnNLB_AddrAToIN(UInt16 refNum, Char *a)
{
    NetIPAddr addr;

    (void)refNum;
    /* Not a dotted quad: return -1 (INADDR_NONE) like inet_addr. Apps such
     * as Mocha Telnet only fall back to GetHostByName on -1; returning 0
     * made them connect to 0.0.0.0. */
    if (a == NULL || !FnNLParseDotted(a, &addr))
        return (NetIPAddr)0xFFFFFFFFUL;
    return addr;
}
