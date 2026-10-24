#pragma once
/* The host form's and the SSH client's rules for a server entry (Plan 4 Task 9, spec §5.5, B6).
   Pure (no ESP-IDF, no LVGL), so the host tests cover them. */
#include <stdbool.h>
#ifdef __cplusplus
extern "C" {
#endif

typedef enum { HF_OK, HF_NO_ADDRESS, HF_NO_USER, HF_BAD_PORT, HF_IS_HOST, HF_NAME_TAKEN } hf_err_t;
/* The form's Save check. address and user are required (after trimming spaces); port is 1..65535 in decimal
   digits only. Unless is_host_entry, an address that cc_same_host matches host_address (the Host's entry, "" or
   NULL: none) is refused: the Host is never a Terminal host. *port_out (optional) gets the port on HF_OK. */
hf_err_t hf_validate(const char *address, const char *port, const char *user, bool is_host_entry,
                     const char *host_address, int *port_out);

/* C1 (hard rule 1): every address the link knows the Host by: its entry's host, that name's tailnet IP, the LAN
   fallback, the carry / mac_ts_ip tailnet address, the last verified and pinned addresses, the last LAN address and
   the host cache (cc_link_host_addresses fills it). Empty and repeated addresses are skipped; it never overflows. */
#define HF_HOST_ADDRS_MAX 12
typedef struct { char a[HF_HOST_ADDRS_MAX][128]; int n; } hf_host_addrs_t;
void hf_host_addrs_init(hf_host_addrs_t *h);
void hf_host_addrs_add(hf_host_addrs_t *h, const char *addr);
/* addr names the Host: cc_same_host with any of h's addresses (NULL h: no Host). */
bool hf_is_host(const char *addr, const hf_host_addrs_t *h);
/* hf_validate against every known Host address (h may be NULL). */
hf_err_t hf_validate_addrs(const char *address, const char *port, const char *user, bool is_host_entry,
                           const hf_host_addrs_t *h, int *port_out);
/* I1: name is already another entry's (case-insensitive; names[self] is the entry being edited, -1 for a new one).
   The Host is chosen by name, so two entries with one name could swap which entry is the Host. */
bool hf_name_taken(const char *name, const char *const *names, int n, int self);

typedef enum { HF_AUTH_DEVICE, HF_AUTH_PASSWORD, HF_AUTH_SD_FILE } hf_auth_t;
/* "Sign in with": a key with an empty or "device" path is the device key; a key with any other path is the
   read-only "Key file on SD card"; no key is Password. */
hf_auth_t hf_auth_of(bool use_key, const char *keypath);

/* A tailnet host: 100.64.0.0/10 or *.ts.net (ts_host_needs_tailscale, one rule; ruling C16). */
bool hf_tailnet_host(const char *host);

typedef enum { HF_KEY_DEVICE, HF_KEY_FILE, HF_KEY_FILE_MISSING, HF_KEY_BAD_PATH, HF_KEY_NONE } hf_key_src_t;
/* B6: which key a key-auth entry signs in with. An empty or "device" path is the device key; a path under /sdcard/
   with no ".." part is that file, or FILE_MISSING when it doesn't exist; any other path is BAD_PATH (M9). Never the
   device key for a file path. */
hf_key_src_t hf_key_source(const char *keypath, bool file_exists);
/* authenticate's plan (ssh_client.c; the seam its decision is tested through, M11): a key entry signs in with key
   (NONE for a password entry) and never tries a password or keyboard-interactive answer, so a key file's passphrase
   is never sent as a password (M10). */
typedef struct { hf_key_src_t key; bool try_password; } hf_auth_plan_t;
hf_auth_plan_t hf_auth_plan(bool use_key, const char *keypath, bool file_exists);
/* B6: a key path the SSH client mounts the SD card for before it connects. */
bool hf_key_on_sd(const char *keypath);
/* B6: the connect error when a key file is missing (the SSH client's state message; the Terminal row shows it). */
#define HF_KEY_FILE_MISSING_MSG "Key file not found on SD card"
#define HF_KEY_BAD_PATH_MSG "The key file must be on the SD card"
#define HF_KEY_REFUSED_MSG "The host refused the key"

#ifdef __cplusplus
}
#endif
