#pragma once
#include "lvgl.h"
#include <stdbool.h>
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif

/* Sessions (spec §5.1): the home screen. A 440 px list of the Host's Claude Code sessions on the
 * left, the selected session's detail on the right (its log tail, an answer field, Send and Open).
 * Built once per visit and updated in place by a 300 ms tick; the selection and the drafts follow
 * the session (sessions_model), never the row position. */
lv_obj_t *ui_sessions_create(void *arg);
/* The centre of the selected row on screen (power sim touch); false off Sessions or with no row shown. */
bool ui_sessions_selected_center(int32_t *x, int32_t *y);
/* Settings › Host › Forget host key: runs cc_link_forget_host_key on the Sessions worker. A forget
   done in time toasts "New host key pinned: SHA256:…" from the worker's result, on any screen; one
   finished later by a reconnect is toasted by the Sessions tick (P4-3). LVGL task; false if the
   worker can't take it now. */
bool ui_sessions_forget_host_key(void);
/* Settings › Host › Trust this key (polish 1): runs cc_link_trust_host_key(fp) on the Sessions worker, toasted as a
   forget is. LVGL task; false if the worker can't take it now. */
bool ui_sessions_trust_host_key(const char *fp);
/* Settings › Host › Voice input · Check (P5-5): runs cc_link_voice_check on the Sessions worker and toasts
   "Voice input is ready on the Host" or the matching error through ui_async. LVGL task; false when a check is
   already pending or the worker can't take it now. */
bool ui_sessions_voice_check(void);
/* Plan 6 (P6-16): Settings › About's firmware check and Install, as jobs on the worker (ui_settings_fw.c's bodies).
   False when the job couldn't be queued. */
bool ui_sessions_fw_check(void);
bool ui_sessions_fw_start(void);
bool ui_sessions_fw_go_back(void);   /* Previous version › Go back (fix M1) */
/* The voice meter's 100 ms timer (P5-4): non-NULL only while recording with Sessions shown (the preview's timer check). */
lv_timer_t *ui_sessions_voice_meter(void);
/* Why the Host link is down, in Host words with no "(retry in Ns)": "No Host set up", "Can't reach <name>. Is it
   awake?", "The Host has a new host key. Check and trust it in Settings › Host." or cc_link's failure up to its
   parenthetical. The Sessions banner and Settings › Host › Link use it. */
void ui_sessions_link_down_text(char *out, size_t cap);
#define FORGET_TOAST_MS 6000   /* long enough to read the fingerprint */
/* Debug (`companion takeover <id>`): selects the row with this id and opens it as Ctrl+O does, only
   for a session named companion-test-*. LVGL task. NOT_SHOWING unless Sessions is the active screen,
   its rows are live and the link is connected; NO_CARD if there is no such row. */
enum { UI_SESS_TEST_OK = 0, UI_SESS_TEST_NO_CARD, UI_SESS_TEST_NOT_SHOWING };
int ui_sessions_open_test_session(const char *id);
/* The Ctrl+K shortcuts overlay (spec §5.13): any key or a touch closes it. */
void ui_sessions_keys_overlay(bool show);
bool ui_sessions_keys_overlay_shown(void);   /* up on the current Sessions screen */

#ifdef __cplusplus
}
#endif
