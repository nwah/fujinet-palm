/* palm/netshim/fnnetlib.h
 *
 * Public interface to the "FujiNet NetLib" system library (fnnetlib.prc,
 * type 'libr', creator 'FNNL'). This library does two things:
 *
 *  1. Under its OWN name ("FujiNet NetLib", found via SysLibFind/SysLibLoad
 *     with creator 'FNNL'), it exposes three custom control calls
 *     (FnNLInstall/FnNLRemove/FnNLGetStatus) used by FujiConfig/NetTest to
 *     turn the shim on and off and inspect its state.
 *
 *  2. Once FnNLInstall() is called, it splices a SECOND dispatch table --
 *     one that answers to the name "Net.lib" -- into the ROM Net.lib's
 *     SysLibTblEntry, so that any existing app's SysLibFind("Net.lib") /
 *     NetLibOpen() / NetLibXxx() calls run OUR code instead of the ROM's
 *     PPP-based stack, routed over a FujiBus link instead. See fnnl_net.c
 *     and friends for that half; nothing in this header is needed by an app
 *     that only ever calls the standard NetLibXxx() API -- that app just
 *     needs <NetMgr.h> as normal. This header is only for code that wants
 *     to install/remove/inspect the shim itself (see palm/apps/nettest).
 *
 * Trap numbering: our own dispatch table follows the same convention every
 * Palm OS shared library uses (see LibTraps.h / SystemMgr.h): entry 0 of
 * the table is the offset to a NUL-terminated name string, entries 1-4 are
 * the standard Open/Close/Sleep/Wake traps, and entries 5+ (sysLibTrapCustom
 * and up) are library-specific. SYS_TRAP() below is the same macro every
 * other Palm OS API uses (see PalmTypes.h); it just tells the compiler
 * which library-relative trap number to embed after the "trap #15"
 * instruction so the OS dispatcher can look the function up in whichever
 * table is currently installed under this refNum.
 */
#ifndef FNNETLIB_H
#define FNNETLIB_H

#include <PalmOS.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Type/creator of the fnnetlib.prc database itself. */
#define fnNLDbType   'libr'
#define fnNLCreator  'FNNL'

/* Name this library answers to via SysLibFind (our own control calls, NOT
 * the emulated "Net.lib" name -- see fnnl_net.c's own table). */
#define fnNLLibName  "FujiNet NetLib"

/* Our custom traps, starting right after the four standard library traps
 * (Open/Close/Sleep/Wake) every SysLibTblEntry-based library gets for free. */
#define fnNLTrapInstall    (sysLibTrapCustom + 0)
#define fnNLTrapRemove     (sysLibTrapCustom + 1)
#define fnNLTrapGetStatus  (sysLibTrapCustom + 2)
#define fnNLTrapDisconnect (sysLibTrapCustom + 3)

/* Snapshot of the shim's internal state, for FnNLGetStatus(). */
typedef struct FnNLStatusType {
    UInt16  installed;      /* 1 if the Net.lib dispatch table is currently swapped in */
    UInt16  openCount;      /* our emulated NetLibOpen() nesting count */
    UInt16  linkOpen;       /* 1 if the serial link to the FujiNet device is open */
    char    linkLibName[32];/* which Ser* library the link is using ("" if never opened) */
    Err     lastErr;        /* last Palm OS / FujiBus error observed by the link */
    UInt16  socketsInUse;   /* number of the 8 emulated socket slots currently allocated */
    UInt16  fakeHostCount;  /* number of entries populated in the synthetic 198.18.0.x host table */
} FnNLStatusType;

/* Allocates the globals block on first call (if not already allocated) and
 * registers it as feature (fnNLCreator, 0). Locates the ROM's real Net.lib
 * via SysLibFind("Net.lib"); if Net.lib is already open by someone
 * (NetLibOpenCount() > 0) this refuses with netErrStillOpen rather than
 * yank the table out from under an in-progress session. Otherwise saves the
 * ROM's current dispatchTblP and replaces it with our own Net.lib-emulation
 * table. Idempotent: calling this again while already installed just
 * returns errNone. */
Err FnNLInstall(UInt16 refNum)
    SYS_TRAP(fnNLTrapInstall);

/* Disconnects (see FnNLDisconnect), then restores the ROM's original
 * Net.lib dispatch table. Idempotent: calling this while not installed
 * just returns errNone. */
Err FnNLRemove(UInt16 refNum)
    SYS_TRAP(fnNLTrapRemove);

/* Closes every socket and the serial link and zeroes the open count, like
 * "Disconnect" in the Network preferences panel. Many apps exit without
 * NetLibClose, which would otherwise hold the serial port indefinitely.
 * Only safe from a foreground app other than the one using the network,
 * which Palm OS's single-tasking guarantees for any caller that isn't that
 * app. A no-op if the shim was never installed. */
Err FnNLDisconnect(UInt16 refNum)
    SYS_TRAP(fnNLTrapDisconnect);

/* Fills *statusP with the shim's current state. */
Err FnNLGetStatus(UInt16 refNum, FnNLStatusType *statusP)
    SYS_TRAP(fnNLTrapGetStatus);

#ifdef __cplusplus
}
#endif

#endif /* FNNETLIB_H */
