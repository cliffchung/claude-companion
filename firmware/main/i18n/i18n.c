#include "i18n.h"

#define X(id, en) en,
static const char *const tbl_en[STR_COUNT] = { STR_TABLE(X) };
#undef X

lang_t i18n_get_lang(void) { return LANG_EN; }

str_id_t i18n_touch_text(str_id_t keys, bool keyboard)
{
    if (keyboard) return keys;
    switch (keys) {
    case STR_HOSTS_LEAVE_NOTE:    return STR_HOSTS_LEAVE_NOTE_TOUCH;
    case STR_TERM_OPENING_HINT:   return STR_TERM_OPENING_HINT_TOUCH;
    case STR_TERM_SSH_HINT:       return STR_TERM_SSH_HINT_TOUCH;
    case STR_VOICE_HINT:          return STR_VOICE_HINT_TOUCH;
    case STR_VOICE_NOT_AT_PROMPT: return STR_VOICE_NOT_AT_PROMPT_TOUCH;
    case STR_VOICE_NOT_READY:     return STR_VOICE_NOT_READY_TOUCH;
    case STR_VOICE_REVIEW:        return STR_VOICE_REVIEW_TOUCH;
    case STR_VOICE_TOO_SHORT:     return STR_VOICE_TOO_SHORT_TOUCH;
    default:                      return keys;
    }
}

const char *tr(str_id_t id)
{
    return id < STR_COUNT ? tbl_en[id] : "";
}
