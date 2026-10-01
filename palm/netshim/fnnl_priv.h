/* palm/netshim/fnnl_priv.h
 *
 * Internal-only declarations shared by every .c file in this library: the
 * globals block layout, the accessor for it (found via FtrGet(fnNLCreator,
 * 0, ...) so it's reachable no matter which refNum -- ours or the ROM
 * Net.lib's original ("real") one -- a given call came in on), and small
 * shared helpers (error mapping, address/host formatting).
 *
 * IMPORTANT (see docs/... and the top-level task spec): this library must
 * have ZERO writable static/global data of its own -- it runs with no A5
 * globals block at all, since build-prc -L system libraries get
 * entryP->globalsP == NULL here (we manage our OWN globals block
 * dynamically instead, via MemPtrNew + FtrSet, see fnnl_glob.c). Do not add
 * a `static` (non-const) variable to ANY file in this directory -- it would
 * silently become an A5-relative reference with no A5 register set up,
 * which corrupts memory at some unrelated address on this device. Local
 * (stack) variables are fine; so are `static const` tables that contain no
 * pointers (see fnnl_dns.c's service table for why "no pointers" matters:
 * a const array of scalar/char[] structs is addressed PC-relative and needs
 * no relocation, but a const array of `char *` needs the linker to patch in
 * absolute addresses at load time, which nothing does for a ROM-resident
 * system library).
 */
#ifndef FNNL_PRIV_H
#define FNNL_PRIV_H

#include <PalmOS.h>
#include <NetMgr.h>

#include "fnnetlib.h"
#include "fujibus.h"
#include "fn_net.h"
#include "transport_ser.h"

#ifdef __cplusplus
extern "C" {
#endif

#define FNNL_MAX_SOCKETS    8
#define FNNL_MAX_FAKE_HOSTS 16

/* Fake "DNS" addresses handed out for names FujiNet will resolve itself at
 * connect time (198.18.0.0/15 is IANA-reserved for benchmarking/private use
 * and unroutable, so it can't collide with anything real; see fnnl_dns.c). */
#define FNNL_FAKE_HOST_A   198
#define FNNL_FAKE_HOST_B   18
#define FNNL_FAKE_HOST_C   0

typedef struct FnNLSocket {
    Boolean               inUse;
    UInt8                 type;         /* NetSocketTypeEnum; only netSocketTypeStream works today */
    Boolean               nonBlocking;
    Boolean               connected;
    NetSocketAddrINType   remote;
    UInt8                 unit;         /* FujiBus net unit, 1-8 -- always slot index + 1 */
} FnNLSocket;

typedef struct FnNLFakeHost {
    char name[64];      /* "" == empty slot */
} FnNLFakeHost;

/* The whole shim's mutable state, in one MemPtrNew'd block (owner 0, so it
 * outlives whichever app happened to be running when it was allocated).
 * Reached from any trap handler via FnNLGetGlobals(), never through
 * SysLibTblEntry(refNum)->globalsP (we leave that NULL, see fnnl_glob.c). */
typedef struct FnNLGlobals {
    MemPtr  *origTbl;                       /* ROM Net.lib's dispatch table, saved by FnNLInstall */
    Boolean  installed;                     /* table B (Net.lib emulation) currently spliced in */
    UInt16   openCount;                     /* emulated NetLibOpen() nesting count */

    FnSerPort   port;
    FnTransport transport;
    FnCtx       ctx;
    Boolean     linkOpen;
    Err         lastErr;

    FnNLSocket    sockets[FNNL_MAX_SOCKETS];
    FnNLFakeHost  fakeHosts[FNNL_MAX_FAKE_HOSTS];
    UInt16        fakeHostNext;             /* next slot to (re)use, round-robin once full */

    char    scratch[600];                   /* devicespec / misc scratch, reused per-call only */
} FnNLGlobals;

/* Returns the existing globals block, or NULL if FnNLInstall() has never
 * successfully allocated one. Never allocates. */
FnNLGlobals *FnNLGetGlobals(void);

/* Returns the globals block, allocating+registering (MemPtrNew, owner 0,
 * zeroed, FtrSet(fnNLCreator,0,...)) it on first call. Returns NULL only if
 * the allocation itself fails. Used only by FnNLInstall(). */
FnNLGlobals *FnNLGetOrCreateGlobals(void);

/* Closes all sockets and the serial link and zeroes the open count. */
void FnNLShutdownLink(FnNLGlobals *g);

/* Maps an FnErr (FujiBus/core-library result) to the closest NetMgr.h
 * netErrXxx code. */
Err FnNLMapFnErr(FnErr e);

/* Maps a Palm OS Err from fn_ser_open()/SerXxx() to the closest
 * netErrXxx code, for reporting link-open failures through NetLibOpen(). */
Err FnNLMapSerErr(Err e);

/* Looks up name (case-insensitive) in the fake host table, adding it
 * (evicting the oldest entry if full) if not already present. Always
 * succeeds (a fresh entry is always available, by eviction). Returns the
 * fake IP address 198.18.0.(slot+1). */
NetIPAddr FnNLFakeHostLookup(FnNLGlobals *g, const Char *name);

/* Reverse of the above: slot index for a 198.18.0.x address already in the
 * table, or -1 if addr isn't one of ours or that slot is empty. */
Int16 FnNLFakeHostReverse(FnNLGlobals *g, NetIPAddr addr);

/* Writes "a.b.c.d" (NUL-terminated) into buf (must be >= 16 bytes). */
void FnNLAddrToDotted(NetIPAddr addr, Char *buf);

/* Parses a strict dotted-quad "a.b.c.d" (each part 0-255, no leading
 * whitespace) into *outP. Returns true on success; on failure *outP is
 * left unmodified. */
Boolean FnNLParseDotted(const Char *s, NetIPAddr *outP);

/* Resolves a NetSocketAddrINType's address to a hostname string suitable
 * for a FujiBus "N:TCP://host:port/" devicespec: the fake host table's
 * name if addr is one of our synthetic 198.18.0.x addresses, else the
 * dotted-quad form. Writes into buf (>= 64 bytes). */
void FnNLAddrToHostStr(FnNLGlobals *g, NetIPAddr addr, Char *buf);

/* Reads the shared "FujiNet link" unsaved preference (creator 'FNCF', id 0;
 * see palm/apps/fujiconfig and ../fujinet-lib-palmos's
 * include/fujinet-palmos.h for the format this must match byte-for-byte)
 * and fills *libNameP (a buffer of at least 32 bytes) / *baudP. Falls back
 * to "Serial Library" / 115200 if the pref is missing, the wrong version,
 * or short. */
void FnNLReadLinkPref(Char *libNameBuf, UInt32 *baudP);

/* Common socket-ref validation used by every table B socket call (per the
 * task spec: "bad sock ref -> -1, netErrParamErr; library not open ->
 * netErrNotOpen"). Returns the slot, or NULL with *errP set (if errP
 * non-NULL) on any problem. Socket refs are exactly (slot index + 1), 1-8;
 * see fnnl_sock.c's FnNLB_SocketOpen for why (never sysFileDescStdIn==0,
 * always a safe NetFDSetType bit position). */
FnNLSocket *FnNLSlot(FnNLGlobals *g, NetSocketRef sock, Err *errP);

#ifdef __cplusplus
}
#endif

#endif /* FNNL_PRIV_H */
