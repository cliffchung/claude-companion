#include "cc_park.h"

void cc_park_set_radio(cc_park_t *p, bool on) { p->radio = on; }
void cc_park_guard_take(cc_park_t *p) { if (p->guard < UINT8_MAX) p->guard++; }
void cc_park_guard_drop(cc_park_t *p) { if (p->guard > 0) p->guard--; }
bool cc_park_on(const cc_park_t *p) { return p->radio || p->guard > 0; }
bool cc_park_keep_new_session(const cc_park_t *p) { return !cc_park_on(p); }
bool cc_park_abort_work(const cc_park_t *p) { return cc_park_on(p); }
bool cc_park_idle(bool have_session, bool in_connect) { return !have_session && !in_connect; }
