| palm/netshim/entry.s
|
| Everything that must be hand-assembled rather than emitted by the C
| compiler: the code resource's entry trampoline (which the OS calls at
| absolute offset 0 of the 'libr' 0000 resource -- see build-prc's output
| format, verified separately) and both Palm OS shared-library dispatch
| tables.
|
| Dispatch table format (every Palm OS shared library uses this; see
| LibTraps.h / SystemMgr.h's SysLibTblEntryType.dispatchTblP): an array of
| signed 16-bit values, each a BYTE offset from the start of the table to
| some piece of code or data. Entry 0 is the offset to a NUL-terminated
| name string (matched by SysLibFind). Entry n (n>=1) is the offset of the
| code for trap number (sysLibTrapName + n) -- sysLibTrapName=0xA800 is
| entry 0's own "trap number" in this scheme, sysLibTrapOpen=0xA801 is
| entry 1, ..., sysLibTrapCustom=0xA805 is entry 5, and every library's
| custom traps count up from there. The OS trap dispatcher (trap #15)
| reads the caller's refNum off the stack, follows SysLibTblEntry(refNum)
| ->dispatchTblP, computes this same index from the trap number baked into
| the caller's "trap #15; dc.w trapNum" instruction pair, and JMPs (not
| JSRs) to table_start + table[index] -- so the target sees the ORIGINAL
| caller's stack (including its return address), and an ordinary C
| function compiled with a normal prologue/epilogue is a perfectly good
| jump target as long as it's within +-32KB of the table (true here: the
| whole library is a few KB).
|
| Using `dc.w label - table` lets the assembler/linker compute each byte
| offset for us; the ORDER these entries are written in is what encodes
| which trap number each one answers -- position k from the table start
| (k = (label-table)/2 folded back to an index) must exactly match trap
| number sysLibTrapName+k, so every dc.w line below is commented with the
| trap it corresponds to and they must not be reordered or skipped.

	.text

| Library entry point. The OS loader calls the very first bytes of the
| 'libr' 0000 code resource directly (not through the trap dispatcher --
| there is no dispatch table yet at this point, since setting one up is
| THIS call's job). A short branch to the real C entry point is simpler and
| more robust than relying on link order to put a C function first, and the
| task spec explicitly allows "the entry or a branch to it" here.
	.globl start
start:
	bra.w fnnl_start

|===========================================================================
| Table A: our own dispatch table, found via SysLibFind("FujiNet NetLib")
| or SysLibLoad('libr','FnNL',&ref). Standard Open/Close/Sleep/Wake are
| harmless no-ops (FnNLInstall/FnNLRemove do the real work); the three
| custom traps are this library's actual public API (see fnnetlib.h).
|===========================================================================
	.even
	.globl fnnl_own_table
fnnl_own_table:
	dc.w	fnnl_own_name - fnnl_own_table		| 0: sysLibTrapName   (name string)
	dc.w	FnNLB_OwnNoop - fnnl_own_table		| 1: sysLibTrapOpen
	dc.w	FnNLB_OwnNoop - fnnl_own_table		| 2: sysLibTrapClose
	dc.w	FnNLB_OwnNoop - fnnl_own_table		| 3: sysLibTrapSleep
	dc.w	FnNLB_OwnNoop - fnnl_own_table		| 4: sysLibTrapWake
	dc.w	FnNLInstall - fnnl_own_table		| 5: sysLibTrapCustom+0 = fnNLTrapInstall
	dc.w	FnNLRemove - fnnl_own_table			| 6: sysLibTrapCustom+1 = fnNLTrapRemove
	dc.w	FnNLGetStatus - fnnl_own_table		| 7: sysLibTrapCustom+2 = fnNLTrapGetStatus

fnnl_own_name:
	.ascii "FujiNet NetLib\0"

|===========================================================================
| Table B: the Net.lib EMULATION table. FnNLInstall() splices this into the
| ROM Net.lib's own SysLibTblEntry (found via SysLibFind("Net.lib")), so any
| existing app's SysLibFind("Net.lib")/NetLibOpen()/NetLibXxx() calls land
| here instead of in the ROM's PPP stack, under NET.LIB'S OWN REFNUM (never
| this library's) -- every FnNLB_* function below reaches shim state via
| FtrGet('FnNL',0,...), never via a globalsP, since the globalsP that comes
| with THIS refNum still belongs to the untouched ROM code (see fnnl_bits.c,
| which chains straight through to it for the Bit* utilities).
|
| Order matches NetMgr.h's own trap enum exactly (sysLibTrapCustom+0
| .. +63, i.e. AddrINToA .. ScriptExecute) -- see NetMgr.h's
| netLibTrapXxx list; this must not be reordered.
|===========================================================================
| Word-align: the name string above has an odd length, and a 68000 takes an
| address error on a word read from an odd address.
	.even
	.globl fnnl_net_table
fnnl_net_table:
	dc.w	fnnl_net_name - fnnl_net_table			| 0: sysLibTrapName (name string, "Net.lib")
	dc.w	FnNLB_Open - fnnl_net_table				| 1: sysLibTrapOpen
	dc.w	FnNLB_Close - fnnl_net_table			| 2: sysLibTrapClose
	dc.w	FnNLB_ReturnZero - fnnl_net_table		| 3: sysLibTrapSleep
	dc.w	FnNLB_ReturnZero - fnnl_net_table		| 4: sysLibTrapWake
	dc.w	FnNLB_AddrINToA - fnnl_net_table		| 5 (+0):  netLibTrapAddrINToA
	dc.w	FnNLB_AddrAToIN - fnnl_net_table		| 6 (+1):  netLibTrapAddrAToIN
	dc.w	FnNLB_SocketOpen - fnnl_net_table		| 7 (+2):  netLibTrapSocketOpen
	dc.w	FnNLB_SocketClose - fnnl_net_table		| 8 (+3):  netLibTrapSocketClose
	dc.w	FnNLB_SocketOptionSet - fnnl_net_table	| 9 (+4):  netLibTrapSocketOptionSet
	dc.w	FnNLB_SocketOptionGet - fnnl_net_table	| 10 (+5): netLibTrapSocketOptionGet
	dc.w	FnNLB_SocketBind - fnnl_net_table		| 11 (+6): netLibTrapSocketBind
	dc.w	FnNLB_SocketConnect - fnnl_net_table	| 12 (+7): netLibTrapSocketConnect
	dc.w	FnNLB_SocketListen - fnnl_net_table		| 13 (+8): netLibTrapSocketListen
	dc.w	FnNLB_SocketAccept - fnnl_net_table		| 14 (+9): netLibTrapSocketAccept
	dc.w	FnNLB_SocketShutdown - fnnl_net_table	| 15 (+10): netLibTrapSocketShutdown
	dc.w	FnNLB_SendPB - fnnl_net_table			| 16 (+11): netLibTrapSendPB
	dc.w	FnNLB_Send - fnnl_net_table				| 17 (+12): netLibTrapSend
	dc.w	FnNLB_ReceivePB - fnnl_net_table		| 18 (+13): netLibTrapReceivePB
	dc.w	FnNLB_Receive - fnnl_net_table			| 19 (+14): netLibTrapReceive
	dc.w	FnNLB_DmReceive - fnnl_net_table		| 20 (+15): netLibTrapDmReceive
	dc.w	FnNLB_Select - fnnl_net_table			| 21 (+16): netLibTrapSelect
	dc.w	FnNLB_ReturnZero - fnnl_net_table		| 22 (+17): netLibTrapPrefsGet
	dc.w	FnNLB_ReturnZero - fnnl_net_table		| 23 (+18): netLibTrapPrefsSet
	dc.w	FnNLB_ReturnZero - fnnl_net_table		| 24 (+19): netLibTrapDrvrWake
	dc.w	fnnl_return_null - fnnl_net_table		| 25 (+20): netLibTrapInterfacePtr
	dc.w	FnNLB_ReturnParamErr - fnnl_net_table	| 26 (+21): netLibTrapMaster
	dc.w	FnNLB_GetHostByName - fnnl_net_table	| 27 (+22): netLibTrapGetHostByName
	dc.w	FnNLB_SettingGet - fnnl_net_table		| 28 (+23): netLibTrapSettingGet
	dc.w	FnNLB_SettingSet - fnnl_net_table		| 29 (+24): netLibTrapSettingSet
	dc.w	FnNLB_ReturnZero - fnnl_net_table		| 30 (+25): netLibTrapIFAttach
	dc.w	FnNLB_ReturnZero - fnnl_net_table		| 31 (+26): netLibTrapIFDetach
	dc.w	FnNLB_IFGet - fnnl_net_table			| 32 (+27): netLibTrapIFGet
	dc.w	FnNLB_IFSettingGet - fnnl_net_table		| 33 (+28): netLibTrapIFSettingGet
	dc.w	FnNLB_IFSettingSet - fnnl_net_table		| 34 (+29): netLibTrapIFSettingSet
	dc.w	FnNLB_ReturnZero - fnnl_net_table		| 35 (+30): netLibTrapIFUp
	dc.w	FnNLB_ReturnZero - fnnl_net_table		| 36 (+31): netLibTrapIFDown
	dc.w	FnNLB_ReturnZero - fnnl_net_table		| 37 (+32): netLibTrapIFMediaUp
	dc.w	FnNLB_ReturnZero - fnnl_net_table		| 38 (+33): netLibTrapScriptExecuteV32
	dc.w	FnNLB_GetHostByAddr - fnnl_net_table	| 39 (+34): netLibTrapGetHostByAddr
	dc.w	FnNLB_GetServByName - fnnl_net_table	| 40 (+35): netLibTrapGetServByName
	dc.w	FnNLB_SocketAddr - fnnl_net_table		| 41 (+36): netLibTrapSocketAddr
	dc.w	FnNLB_FinishCloseWait - fnnl_net_table	| 42 (+37): netLibTrapFinishCloseWait
	dc.w	FnNLB_GetMailExchangeByName - fnnl_net_table | 43 (+38): netLibTrapGetMailExchangeByName
	dc.w	FnNLB_ReturnZero - fnnl_net_table		| 44 (+39): netLibTrapPrefsAppend
	dc.w	FnNLB_ReturnZero - fnnl_net_table		| 45 (+40): netLibTrapIFMediaDown
	dc.w	FnNLB_OpenCount - fnnl_net_table		| 46 (+41): netLibTrapOpenCount
	dc.w	FnNLB_ReturnZero - fnnl_net_table		| 47 (+42): netLibTrapTracePrintF
	dc.w	FnNLB_ReturnZero - fnnl_net_table		| 48 (+43): netLibTrapTracePutS
	dc.w	FnNLB_OpenIfCloseWait - fnnl_net_table	| 49 (+44): netLibTrapOpenIfCloseWait
	dc.w	FnNLB_ReturnZero - fnnl_net_table		| 50 (+45): netLibTrapHandlePowerOff
	dc.w	FnNLB_ConnectionRefresh - fnnl_net_table | 51 (+46): netLibTrapConnectionRefresh
	dc.w	FnNLB_BitMove - fnnl_net_table			| 52 (+47): netLibTrapBitMove
	dc.w	FnNLB_BitPutFixed - fnnl_net_table		| 53 (+48): netLibTrapBitPutFixed
	dc.w	FnNLB_BitGetFixed - fnnl_net_table		| 54 (+49): netLibTrapBitGetFixed
	dc.w	FnNLB_BitPutUIntV - fnnl_net_table		| 55 (+50): netLibTrapBitPutUIntV
	dc.w	FnNLB_BitGetUIntV - fnnl_net_table		| 56 (+51): netLibTrapBitGetUIntV
	dc.w	FnNLB_BitPutIntV - fnnl_net_table		| 57 (+52): netLibTrapBitPutIntV
	dc.w	FnNLB_BitGetIntV - fnnl_net_table		| 58 (+53): netLibTrapBitGetIntV
	dc.w	FnNLB_ReturnParamErr - fnnl_net_table	| 59 (+54): netLibOpenConfig
	dc.w	FnNLB_ReturnParamErr - fnnl_net_table	| 60 (+55): netLibConfigMakeActive
	dc.w	FnNLB_ReturnParamErr - fnnl_net_table	| 61 (+56): netLibConfigList
	dc.w	FnNLB_ReturnParamErr - fnnl_net_table	| 62 (+57): netLibConfigIndexFromName
	dc.w	FnNLB_ReturnParamErr - fnnl_net_table	| 63 (+58): netLibConfigDelete
	dc.w	FnNLB_ReturnParamErr - fnnl_net_table	| 64 (+59): netLibConfigSaveAs
	dc.w	FnNLB_ReturnParamErr - fnnl_net_table	| 65 (+60): netLibConfigRename
	dc.w	FnNLB_ReturnParamErr - fnnl_net_table	| 66 (+61): netLibConfigAliasSet
	dc.w	FnNLB_ReturnParamErr - fnnl_net_table	| 67 (+62): netLibConfigAliasGet
	dc.w	FnNLB_ReturnZero - fnnl_net_table		| 68 (+63): netLibTrapScriptExecute

fnnl_net_name:
	.ascii "Net.lib\0"

| InterfacePtr returns a pointer, which the Palm trap ABI passes back in A0,
| so a C function returning 0 in D0 would leave garbage there.
	.even
fnnl_return_null:
	moveq	#0,%d0
	suba.l	%a0,%a0
	rts
