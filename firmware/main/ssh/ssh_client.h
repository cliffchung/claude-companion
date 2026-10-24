#pragma once
#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include "libssh/libssh.h"
#include "ssh/kh_rules.h"
#ifdef __cplusplus
extern "C" {
#endif

typedef enum { SSH_ST_IDLE, SSH_ST_CONNECTING, SSH_ST_HOSTKEY, SSH_ST_AUTH, SSH_ST_CONNECTED, SSH_ST_CLOSED, SSH_ST_ERROR } ssh_state_t;

typedef struct {
    char host[128];
    int port;
    char user[64];
    char pass[128];
    char keypath[160];
    bool use_key;
    int cols, rows;
    char cmd[256];      /* "" = login shell; otherwise a pty exec of this command */
} ssh_params_t;

typedef struct ssh_client ssh_client_t;
typedef void (*ssh_data_cb_t)(const uint8_t *data, size_t len, void *user);
typedef void (*ssh_state_cb_t)(ssh_state_t st, const char *msg, void *user);
/* Called from the SSH task when the host key is unknown/changed. Must block
 * until the user answers; return true to trust. */
typedef bool (*ssh_hostkey_cb_t)(const char *host, int port, const char *fingerprint, bool changed, void *user);

ssh_client_t *ssh_client_start(const ssh_params_t *p, ssh_data_cb_t data_cb, ssh_state_cb_t state_cb,
                               ssh_hostkey_cb_t hostkey_cb, void *user);
void ssh_client_send(ssh_client_t *c, const uint8_t *data, size_t len);
void ssh_client_resize(ssh_client_t *c, int cols, int rows);
void ssh_client_stop(ssh_client_t *c);        /* async; object freed by the task */
ssh_state_t ssh_client_state(ssh_client_t *c);
const char *ssh_client_error(ssh_client_t *c);
/* Command mode: the remote exit status once the state is CLOSED, or -1 if unknown. */
int ssh_client_exit_status(ssh_client_t *c);
/* Final review C1: registers the live clients' closer with the close-before-down guard (sys/link_guard.h):
   every path that takes the link down first closes each open SSH Terminal and take-over session and waits
   (bounded) for its disconnect. Its state then ends as SSH_ST_ERROR "the network is going down". Called
   once at boot, before any client starts. */
void ssh_client_guard_init(void);
/* known hosts (NVS "kh"): key = hash of host:port, value = SHA256 of the server key */
void ssh_known_hosts_clear(void);
/* kh_result_t is in kh_rules.h. KH_PIN_ERROR (the pin is unreadable) never re-pins. */
/* Compares the connected session's server key with the pin for host:port; fp_out gets "SHA256:..." (optional). */
kh_result_t ssh_known_host_check(ssh_session session, const char *host, int port, char *fp_out, size_t fp_cap);
/* The connected session's server key, SHA256 (32 bytes); false if unreadable. */
bool ssh_server_key_hash(ssh_session session, uint8_t out[32]);
/* The pin for host:port (32-byte SHA256): found (out set), none, or unreadable. */
kh_pin_t ssh_known_host_hash(const char *host, int port, uint8_t out[32]);
/* Pins the connected session's server key for host:port. */
bool ssh_known_host_trust(ssh_session session, const char *host, int port);
/* The pinned fingerprint for host:port as "SHA256:..." (as ssh-keygen -l prints it); false
   when there is no pin or it can't be read. */
bool ssh_known_host_fingerprint(const char *host, int port, char *fp_out, size_t fp_cap);
/* Erases the pin for host:port; true if there was one. */
bool ssh_known_host_forget(const char *host, int port);
/* Debug: overwrites the pin for host:port with a wrong hash, so the next connect sees a
   changed key (`companion hostkey corrupt`). */
bool ssh_known_host_set_debug(const char *host, int port);

#ifdef __cplusplus
}
#endif
