#include "kh_carry.h"

khc_t kh_carry_decide(bool lan_pinned, bool tailnet_pinned, bool keys_equal)
{
    if (lan_pinned) return KHC_NORMAL;
    if (!tailnet_pinned) return KHC_UNKNOWN;
    return keys_equal ? KHC_PIN_SILENT : KHC_REFUSE;
}
