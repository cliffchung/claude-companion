#include "i18n.h"

#define X(id, en) en,
static const char *const tbl_en[STR_COUNT] = { STR_TABLE(X) };
#undef X

lang_t i18n_get_lang(void) { return LANG_EN; }

const char *tr(str_id_t id)
{
    return id < STR_COUNT ? tbl_en[id] : "";
}
