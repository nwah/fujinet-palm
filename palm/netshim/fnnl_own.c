/* palm/netshim/fnnl_own.c
 *
 * The library's entry point (fnnl_start, called by the OS the moment the
 * code resource is loaded -- see entry.s's `start` trampoline at absolute
 * offset 0) and the three custom control calls exposed under our own name
 * "FujiNet NetLib" (table A): FnNLInstall/FnNLRemove/FnNLGetStatus. Also
 * the shared no-op used for table A's standard Open/Close/Sleep/Wake slots.
 */
#include "fnnl_priv.h"

/* Table A (our own dispatch table) and table B (the Net.lib emulation
 * table) live in entry.s as raw 16-bit-offset arrays; declaring them as
 * incomplete array types here just gives us their addresses to hand to the
 * OS / splice into Net.lib's SysLibTblEntry. */
/* Declared as functions, not arrays: gcc addresses extern DATA through A5,
 * which a system library doesn't have, but takes a function's address
 * PC-relative. Neither is ever called. */
extern void fnnl_own_table(void);
extern void fnnl_net_table(void);

/* Table A's Open/Close/Sleep/Wake: no per-call state to track (Install is
 * what actually does work), so all four just report success. */
Err FnNLB_OwnNoop(void)
{
    return errNone;
}

/* Library entry point. Called directly by the OS loader/SysLibLoad the
 * moment fnnetlib.prc's 'libr' 0 code resource is loaded -- entryP is
 * OUR OWN SysLibTblEntry (not Net.lib's), refNum is our own refNum. We
 * always keep table A installed; globalsP is left NULL because this
 * library's real state lives in the dynamically-allocated block reached
 * via FtrGet(fnNLCreator, 0, ...) instead (see fnnl_glob.c) -- that block
 * must be reachable from table B's trap handlers too, which run under
 * Net.lib's refNum/SysLibTblEntry, not this one. */
Err fnnl_start(UInt16 refNum, SysLibTblEntryPtr entryP)
{
    (void)refNum;

    entryP->dispatchTblP = (MemPtr *)&fnnl_own_table;
    entryP->globalsP = NULL;

    return errNone;
}

Err FnNLInstall(UInt16 refNum)
{
    FnNLGlobals       *g;
    UInt16             netRefNum;
    UInt16             openCt;
    Err                err;
    SysLibTblEntryPtr  entry;

    (void)refNum;

    g = FnNLGetOrCreateGlobals();
    if (g == NULL)
        return netErrOutOfMemory;

    if (g->installed)
        return errNone; /* idempotent */

    err = SysLibFind("Net.lib", &netRefNum);
    if (err != errNone)
        return err;

    /* Refuse to yank the table out from under an app that already has
     * Net.lib open -- its socket/interface state would be orphaned. If the
     * open-count query itself fails, Net.lib has presumably never been
     * opened on this ROM yet, so proceed. */
    openCt = 0;
    err = NetLibOpenCount(netRefNum, &openCt);
    if (err == errNone && openCt > 0)
        return netErrStillOpen;

    entry = SysLibTblEntry(netRefNum);
    if (entry == NULL)
        return netErrInternal;

    g->origTbl = entry->dispatchTblP;
    entry->dispatchTblP = (MemPtr *)&fnnl_net_table;
    g->installed = true;

    return errNone;
}

Err FnNLRemove(UInt16 refNum)
{
    FnNLGlobals       *g = FnNLGetGlobals();
    UInt16             netRefNum;
    Err                err;
    SysLibTblEntryPtr  entry;

    (void)refNum;

    if (g == NULL || !g->installed)
        return errNone; /* idempotent */

    FnNLShutdownLink(g);

    err = SysLibFind("Net.lib", &netRefNum);
    if (err != errNone)
        return err;

    entry = SysLibTblEntry(netRefNum);
    if (entry == NULL)
        return netErrInternal;

    entry->dispatchTblP = g->origTbl;
    g->installed = false;

    return errNone;
}

Err FnNLDisconnect(UInt16 refNum)
{
    FnNLGlobals *g = FnNLGetGlobals();

    (void)refNum;
    if (g != NULL)
        FnNLShutdownLink(g);
    return errNone;
}

Err FnNLGetStatus(UInt16 refNum, FnNLStatusType *statusP)
{
    FnNLGlobals *g = FnNLGetGlobals();
    UInt16       i;

    (void)refNum;

    if (statusP == NULL)
        return netErrParamErr;

    MemSet(statusP, sizeof(*statusP), 0);

    if (g == NULL)
        return errNone; /* never installed: all-zero status is accurate */

    statusP->installed = (UInt16)(g->installed ? 1 : 0);
    statusP->openCount = g->openCount;
    statusP->linkOpen  = (UInt16)(g->linkOpen ? 1 : 0);
    StrNCopy(statusP->linkLibName, g->port.libName, sizeof(statusP->linkLibName) - 1);
    statusP->linkLibName[sizeof(statusP->linkLibName) - 1] = '\0';
    statusP->lastErr = g->lastErr;

    statusP->socketsInUse = 0;
    for (i = 0; i < FNNL_MAX_SOCKETS; i++) {
        if (g->sockets[i].inUse)
            statusP->socketsInUse++;
    }

    statusP->fakeHostCount = 0;
    for (i = 0; i < FNNL_MAX_FAKE_HOSTS; i++) {
        if (g->fakeHosts[i].name[0] != '\0')
            statusP->fakeHostCount++;
    }

    return errNone;
}
