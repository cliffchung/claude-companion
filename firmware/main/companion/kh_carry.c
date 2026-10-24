#include "kh_carry.h"

khc_t kh_carry_decide(bool lan_pinned, bool tailnet_pinned, bool keys_equal)
{
    if (lan_pinned) return KHC_NORMAL;
    if (!tailnet_pinned) return KHC_UNKNOWN;
    return keys_equal ? KHC_PIN_SILENT : KHC_REFUSE;
}

kha_t kh_carry_action(kh_result_t lan, bool tailnet_addr, bool forget_pending, khs_t src)
{
    if (lan != KH_UNKNOWN) return KHA_NORMAL;
    if (tailnet_addr || forget_pending) return KHA_PIN_NEW;
    if (src == KHS_ERROR) return KHA_PIN_ERROR;
    switch (kh_carry_decide(false, src != KHS_NONE, src == KHS_EQUAL)) {
    case KHC_PIN_SILENT: return KHA_PIN_SILENT;
    case KHC_REFUSE:     return KHA_REFUSE_CHANGED;
    default:             return KHA_REFUSE_UNKNOWN;
    }
}
