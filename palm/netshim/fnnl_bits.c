/* palm/netshim/fnnl_bits.c
 *
 * The seven NetLibBitXxx() bit-packing utilities (NetBitUtils.h). These are
 * documented as pure functions with no dependency on the link/socket state
 * -- the task spec calls for chaining straight through to the ROM Net.lib's
 * own implementation rather than reimplementing them, since the ROM's
 * globalsP for Net.lib is untouched by FnNLInstall() (we only ever replace
 * dispatchTblP). This also sidesteps a real quirk in the 3.5 SDK's own
 * NetBitUtils.h: NetLibBitPutIntV/NetLibBitGetIntV are annotated with
 * SYS_TRAP(netLibTrapBitPutUIntV)/SYS_TRAP(netLibTrapBitGetUIntV) (a
 * copy/paste bug -- they reuse the *UIntV* trap numbers, not their own
 * netLibTrapBitPutIntV/GetIntV), so any app built against that header
 * actually issues trap netLibTrapBitPutUIntV/BitGetUIntV for its
 * BitPutIntV/BitGetIntV calls. Chaining strictly by dispatch-table
 * position (not by "which function did the caller think it called") means
 * this shim reproduces the ROM's behavior byte for byte regardless: we
 * still implement all seven at their correct NetMgr.h-declared positions,
 * we just don't need to special-case anything for the quirk to work.
 */
#include "fnnl_priv.h"

/* Returns the ROM Net.lib's own handler for trapNum, by reading the saved
 * original dispatch table (g->origTbl) at the same relative index the OS
 * trap dispatcher itself would use: (trapNum - sysLibTrapBase) 16-bit
 * entries in, each holding a table-relative byte offset to the code. */
static void *FnNLOrigTrap(FnNLGlobals *g, UInt16 trapNum)
{
    Int16 *tbl;
    UInt16 idx;
    Int16  off;

    if (g == NULL || g->origTbl == NULL)
        return NULL;

    tbl = (Int16 *)g->origTbl;
    idx = (UInt16)(trapNum - sysLibTrapBase);
    off = tbl[idx];
    return (void *)((char *)tbl + off);
}

void FnNLB_BitMove(UInt16 libRefNum, UInt8 *dstP, UInt32 *dstBitOffsetP,
                    UInt8 *srcP, UInt32 *srcBitOffsetP, UInt32 numBits)
{
    FnNLGlobals *g = FnNLGetGlobals();
    void (*fn)(UInt16, UInt8 *, UInt32 *, UInt8 *, UInt32 *, UInt32);

    fn = (void (*)(UInt16, UInt8 *, UInt32 *, UInt8 *, UInt32 *, UInt32))
        FnNLOrigTrap(g, netLibTrapBitMove);
    if (fn != NULL)
        fn(libRefNum, dstP, dstBitOffsetP, srcP, srcBitOffsetP, numBits);
}

void FnNLB_BitPutFixed(UInt16 libRefNum, UInt8 *dstP, UInt32 *dstBitOffsetP,
                        UInt32 value, UInt16 numBits)
{
    FnNLGlobals *g = FnNLGetGlobals();
    void (*fn)(UInt16, UInt8 *, UInt32 *, UInt32, UInt16);

    fn = (void (*)(UInt16, UInt8 *, UInt32 *, UInt32, UInt16))
        FnNLOrigTrap(g, netLibTrapBitPutFixed);
    if (fn != NULL)
        fn(libRefNum, dstP, dstBitOffsetP, value, numBits);
}

UInt32 FnNLB_BitGetFixed(UInt16 libRefNum, UInt8 *srcP, UInt32 *srcBitOffsetP,
                          UInt16 numBits)
{
    FnNLGlobals *g = FnNLGetGlobals();
    UInt32 (*fn)(UInt16, UInt8 *, UInt32 *, UInt16);

    fn = (UInt32 (*)(UInt16, UInt8 *, UInt32 *, UInt16))
        FnNLOrigTrap(g, netLibTrapBitGetFixed);
    if (fn == NULL)
        return 0;
    return fn(libRefNum, srcP, srcBitOffsetP, numBits);
}

void FnNLB_BitPutUIntV(UInt16 libRefNum, UInt8 *dstP, UInt32 *dstBitOffsetP, UInt32 value)
{
    FnNLGlobals *g = FnNLGetGlobals();
    void (*fn)(UInt16, UInt8 *, UInt32 *, UInt32);

    fn = (void (*)(UInt16, UInt8 *, UInt32 *, UInt32))
        FnNLOrigTrap(g, netLibTrapBitPutUIntV);
    if (fn != NULL)
        fn(libRefNum, dstP, dstBitOffsetP, value);
}

UInt32 FnNLB_BitGetUIntV(UInt16 libRefNum, UInt8 *srcP, UInt32 *srcBitOffsetP)
{
    FnNLGlobals *g = FnNLGetGlobals();
    UInt32 (*fn)(UInt16, UInt8 *, UInt32 *);

    fn = (UInt32 (*)(UInt16, UInt8 *, UInt32 *))
        FnNLOrigTrap(g, netLibTrapBitGetUIntV);
    if (fn == NULL)
        return 0;
    return fn(libRefNum, srcP, srcBitOffsetP);
}

void FnNLB_BitPutIntV(UInt16 libRefNum, UInt8 *dstP, UInt32 *dstBitOffsetP, Int32 value)
{
    FnNLGlobals *g = FnNLGetGlobals();
    void (*fn)(UInt16, UInt8 *, UInt32 *, Int32);

    fn = (void (*)(UInt16, UInt8 *, UInt32 *, Int32))
        FnNLOrigTrap(g, netLibTrapBitPutIntV);
    if (fn != NULL)
        fn(libRefNum, dstP, dstBitOffsetP, value);
}

Int32 FnNLB_BitGetIntV(UInt16 libRefNum, UInt8 *srcP, UInt32 *srcBitOffsetP)
{
    FnNLGlobals *g = FnNLGetGlobals();
    Int32 (*fn)(UInt16, UInt8 *, UInt32 *);

    fn = (Int32 (*)(UInt16, UInt8 *, UInt32 *))
        FnNLOrigTrap(g, netLibTrapBitGetIntV);
    if (fn == NULL)
        return 0;
    return fn(libRefNum, srcP, srcBitOffsetP);
}
