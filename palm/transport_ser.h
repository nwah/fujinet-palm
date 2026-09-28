/* palm/transport_ser.h
 *
 * FnTransport binding onto the Palm OS *old* Serial Manager (Ser* API,
 * SerialMgrOld.h), reached through one of the named shared libraries a
 * Handspring Visor exposes (see Handspring app note AN-09):
 *
 *   "USB Library"     - USB cradle bridge (creator 'HsUs'; our current dev
 *                        link). Baud is presumably ignored but we pass one
 *                        anyway.
 *   "BuiltIn SerLib"   - always the cradle UART. Creator code unknown; only
 *                        SysLibFind-by-name is used for it.
 *   "Serial Library"   - default, HsPrefSet-redirectable library. Creator
 *                        code unknown; SysLibFind-by-name only.
 *
 * Targets Palm OS 3.1 (DragonBall EZ, m68k). Only the OLD Serial Manager
 * exists on this device -- there is no New Serial Manager (Srm*) to fall
 * back to.
 *
 * TODO(serial cradle): in a serial (non-USB) cradle, Handspring's keyboard
 * daemon holds the UART. HsExtKeyboardEnable(false) (HsExt.h, not present in
 * this SDK) must be called before SerOpen and re-enabled after SerClose.
 * Not needed for the USB dev link used right now; left as a TODO for when
 * "BuiltIn SerLib" is exercised on real hardware.
 */
#ifndef PALM_TRANSPORT_SER_H
#define PALM_TRANSPORT_SER_H

#include <PalmOS.h>
#include "fn_transport.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    UInt16  refNum;         /* library reference number once open */
    Boolean open;
    MemPtr  rxBuf;          /* NULL if the bigger receive buffer could not be allocated */
    UInt32  baud;
    Err     lastErr;        /* last Ser- or Sys-call error observed, for diagnostics/logging */
    char    libName[32];    /* copy of the name passed to fn_ser_open */
} FnSerPort;

/* Opens one of "USB Library", "BuiltIn SerLib", or "Serial Library" by name
 * via SysLibFind, applies 115200/8N1/no-flow-control settings (the baud
 * actually requested is `baud`), and installs a ~4KB receive buffer.
 *
 * On any failure, p->open is left false and p->lastErr / the return value
 * carry the Palm OS Err code. p is fully initialized (zeroed) regardless of
 * success. Safe to call fn_ser_close() on a port that failed to open.
 */
Err fn_ser_open(FnSerPort *p, const char *libName, UInt32 baud);

/* Restores the default receive buffer, closes the library, and frees the
 * receive buffer allocated by fn_ser_open. Safe to call on an already-closed
 * or never-opened (zeroed) port. */
void fn_ser_close(FnSerPort *p);

/* Fills t with a vtable bound to p (t->ctx = p). p must outlive t. */
void fn_ser_transport(FnSerPort *p, FnTransport *t);

#ifdef __cplusplus
}
#endif

#endif /* PALM_TRANSPORT_SER_H */
