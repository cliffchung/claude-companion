#pragma once
#include <stdint.h>
#include <stdbool.h>
#ifdef __cplusplus
extern "C" {
#endif
#define SERVER_MAX 24
typedef struct {
    char name[48];
    char host[128];
    int  port;
    char user[64];
    char pass[128];      /* plain in RAM, encrypted in NVS */
    char keypath[160];
    bool use_key;
} server_t;

void server_store_init(void);            /* reads NVS only */
/* The CFG 23 migration (spec §10 step 3), once: the hosts an older build kept on the TF card (creds_on_sd) are
 * added to the NVS list (an entry whose name, or user, host and port, is already there is skipped) and saved.
 * The card must be mounted first (sd_mount); its file is never changed. Returns the count added, or -1 when
 * the card or its file can't be read (the NVS list is then unchanged). */
int  server_store_import_sd_once(void);
int  server_store_count(void);
const server_t *server_store_get(int i);
int  server_store_add(const server_t *s);       /* returns index */
bool server_store_update(int i, const server_t *s);
bool server_store_remove(int i);
void server_store_move(int from, int to);
#ifdef __cplusplus
}
#endif
