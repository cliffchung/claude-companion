#include "kh_rules.h"
#include <string.h>

kh_result_t kh_classify(kh_pin_t pin, const void *stored, size_t slen, const void *hash, size_t hlen)
{
    if (pin == KH_PIN_READ_FAILED) return KH_PIN_ERROR;
    if (pin == KH_PIN_NONE) return KH_UNKNOWN;
    if (!stored || !hash || slen != hlen || memcmp(stored, hash, hlen) != 0) return KH_CHANGED;
    return KH_OK;
}

bool kh_needs_answer(kh_result_t r)
{
    return r == KH_UNKNOWN || r == KH_CHANGED;
}

bool kh_accept(kh_result_t r, bool have_cb, bool cb_answer)
{
    if (r == KH_OK) return true;
    if (!kh_needs_answer(r)) return false;
    return have_cb && cb_answer;
}
