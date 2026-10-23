#pragma once
#include <stddef.h>
#include "esp_err.h"
#include "libssh/libssh.h"
#ifdef __cplusplus
extern "C" {
#endif
/* The Tab5's own SSH identity (ed25519), kept in NVS namespace "devkey". */

/* Load the device key from NVS, generating and storing one on first use. Idempotent. */
esp_err_t device_key_ensure(void);
/* Returns a new ssh_key (private); caller frees with ssh_key_free. */
esp_err_t device_key_get(ssh_key *out);
/* "ssh-ed25519 AAAA... claude-companion@<tailscale node name or 'tab5'>" */
esp_err_t device_key_pubkey_line(char *out, size_t cap);
/* Replace the key with a fresh one. */
esp_err_t device_key_regenerate(void);
/* Register the `sshkey` console command. */
void device_key_console_init(void);
#ifdef __cplusplus
}
#endif
