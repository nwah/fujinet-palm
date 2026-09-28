/* palm/netshim/fnnl_if.c
 *
 * Table B entries for the global/interface settings calls: SettingGet/Set,
 * IFGet, IFSettingGet/Set. Everything here describes a single synthetic
 * "interface" (creator netIFCreatorPPP, instance 0 -- so third-party code
 * that enumerates interfaces expecting to find A PPP interface, which is
 * what every existing Net.lib app on this ROM already assumes, finds
 * exactly one, reporting itself up and always-connected once the FujiBus
 * link is open).
 */
#include "fnnl_priv.h"

Err FnNLB_SettingGet(UInt16 refNum, UInt16 setting, void *valueP, UInt16 *valueLenP)
{
    (void)refNum;

    if (valueP == NULL || valueLenP == NULL)
        return netErrParamErr;

    switch (setting) {
        case netSettingPrimaryDNS: {
            UInt32 v = ((UInt32)FNNL_FAKE_HOST_A << 24) | ((UInt32)FNNL_FAKE_HOST_B << 16) |
                       ((UInt32)FNNL_FAKE_HOST_C << 8) | 1;
            if (*valueLenP < sizeof(UInt32)) return netErrInvalidSettingSize;
            *(UInt32 *)valueP = v;
            *valueLenP = sizeof(UInt32);
            return errNone;
        }
        case netSettingSecondaryDNS: {
            UInt32 v = ((UInt32)FNNL_FAKE_HOST_A << 24) | ((UInt32)FNNL_FAKE_HOST_B << 16) |
                       ((UInt32)FNNL_FAKE_HOST_C << 8) | 2;
            if (*valueLenP < sizeof(UInt32)) return netErrInvalidSettingSize;
            *(UInt32 *)valueP = v;
            *valueLenP = sizeof(UInt32);
            return errNone;
        }
        case netSettingHostName: {
            UInt16 n = (UInt16)(StrLen("visor") + 1);
            if (*valueLenP < n) return netErrInvalidSettingSize;
            StrCopy((Char *)valueP, "visor");
            *valueLenP = n;
            return errNone;
        }
        default:
            return netErrUnknownSetting;
    }
}

Err FnNLB_SettingSet(UInt16 refNum, UInt16 setting, void *valueP, UInt16 valueLen)
{
    (void)refNum;
    (void)setting;
    (void)valueP;
    (void)valueLen;
    return errNone;
}

Err FnNLB_IFGet(UInt16 refNum, UInt16 index, UInt32 *ifCreatorP, UInt16 *ifInstanceP)
{
    (void)refNum;

    if (index != 0)
        return netErrInvalidInterface;

    if (ifCreatorP != NULL) *ifCreatorP = netIFCreatorPPP;
    if (ifInstanceP != NULL) *ifInstanceP = 0;
    return errNone;
}

Err FnNLB_IFSettingGet(UInt16 refNum, UInt32 ifCreator, UInt16 ifInstance,
                        UInt16 setting, void *valueP, UInt16 *valueLenP)
{
    FnNLGlobals *g = FnNLGetGlobals();

    (void)refNum;
    (void)ifCreator;
    (void)ifInstance;

    if (valueP == NULL || valueLenP == NULL)
        return netErrParamErr;

    switch (setting) {
        case netIFSettingUp: {
            UInt8 v = (UInt8)((g != NULL && g->linkOpen) ? 1 : 0);
            if (*valueLenP < sizeof(UInt8)) return netErrInvalidSettingSize;
            *(UInt8 *)valueP = v;
            *valueLenP = sizeof(UInt8);
            return errNone;
        }
        case netIFSettingName: {
            UInt16 n = (UInt16)(StrLen("FujiNet") + 1);
            if (*valueLenP < n) return netErrInvalidSettingSize;
            StrCopy((Char *)valueP, "FujiNet");
            *valueLenP = n;
            return errNone;
        }
        case netIFSettingActualIPAddr:
        case netIFSettingReqIPAddr: {
            UInt32 v = ((UInt32)10 << 24) | 2; /* 10.0.0.2 */
            if (*valueLenP < sizeof(UInt32)) return netErrInvalidSettingSize;
            *(UInt32 *)valueP = v;
            *valueLenP = sizeof(UInt32);
            return errNone;
        }
        default:
            return netErrUnknownSetting;
    }
}

Err FnNLB_IFSettingSet(UInt16 refNum, UInt32 ifCreator, UInt16 ifInstance,
                        UInt16 setting, void *valueP, UInt16 valueLen)
{
    (void)refNum;
    (void)ifCreator;
    (void)ifInstance;
    (void)setting;
    (void)valueP;
    (void)valueLen;
    return errNone;
}
