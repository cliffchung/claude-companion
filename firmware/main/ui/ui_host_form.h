#pragma once
/* The host form (spec §5.5): Add host / Edit host, from Terminal, Settings › Host › Address and the Host picker's
   "Add host…". The rules it checks are servers/host_rules.h. */
#include "lvgl.h"
#include "servers/host_rules.h"
#ifdef __cplusplus
extern "C" {
#endif

typedef enum { HF_FROM_TERMINAL, HF_FROM_SETTINGS_HOST, HF_FROM_HOST_PICKER } hf_origin_t;
/* index: the server entry to edit, -1 for a new host (prefilled from prefill_name / prefill_host when set). The
   caller keeps the arg static (the nav stack holds the pointer) and zeroes it before each push. The form records the
   entry it opened (opened_*) on its first create and finds it again by that identity on any re-create (M8). */
typedef struct {
    int index; hf_origin_t origin; char prefill_name[48], prefill_host[128];
    bool opened; char opened_name[48], opened_host[128]; int opened_port;   /* the form's, not the caller's */
} hf_arg_t;

/* arg is an hf_arg_t *. Header "← Terminal" (from Terminal) or "← Settings", "Add host" or "Edit host", and a primary
   Save. Saving from HF_FROM_HOST_PICKER makes the new entry the Host (cc_link_set_server, P4-6); saving the Host's
   entry from Settings tells the link (cc_link_set_server with its name). */
lv_obj_t *ui_host_form_create(void *arg);

#ifdef __cplusplus
}
#endif
