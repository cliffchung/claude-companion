#pragma once
/* The host form's and the SSH client's rules for a server entry (Plan 4 Task 9, spec §5.5, B6).
   Pure (no ESP-IDF, no LVGL), so the host tests cover them. */
#include <stdbool.h>
#ifdef __cplusplus
extern "C" {
#endif

typedef enum { HF_OK, HF_NO_ADDRESS, HF_NO_USER, HF_BAD_PORT, HF_IS_HOST } hf_err_t;
/* The form's Save check. address and user are required (after trimming spaces); port is 1..65535 in decimal
   digits only. Unless is_host_entry, an address that cc_same_host matches host_address (the Host's entry, "" or
   NULL: none) is refused: the Host is never a Terminal host. *port_out (optional) gets the port on HF_OK. */
hf_err_t hf_validate(const char *address, const char *port, const char *user, bool is_host_entry,
                     const char *host_address, int *port_out);

typedef enum { HF_AUTH_DEVICE, HF_AUTH_PASSWORD, HF_AUTH_SD_FILE } hf_auth_t;
/* "Sign in with": a key with an empty or "device" path is the device key; a key with any other path is the
   read-only "Key file on SD card"; no key is Password. */
hf_auth_t hf_auth_of(bool use_key, const char *keypath);

/* A tailnet host: 100.64.0.0/10 or *.ts.net (ts_host_needs_tailscale, one rule; ruling C16). */
bool hf_tailnet_host(const char *host);

typedef enum { HF_KEY_DEVICE, HF_KEY_FILE, HF_KEY_FILE_MISSING } hf_key_src_t;
/* B6: which key a key-auth entry signs in with. An empty or "device" path is the device key; any other path is
   that file, or FILE_MISSING when it doesn't exist. Never the device key for a file path. */
hf_key_src_t hf_key_source(const char *keypath, bool file_exists);
/* B6: a key path the SSH client mounts the SD card for before it connects. */
bool hf_key_on_sd(const char *keypath);
/* B6: the connect error when a key file is missing (the SSH client's state message; the Terminal row shows it). */
#define HF_KEY_FILE_MISSING_MSG "Key file not found on SD card"

#ifdef __cplusplus
}
#endif
