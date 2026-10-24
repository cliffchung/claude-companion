#pragma once
/* The host form's and the SSH client's rules for a server entry (Plan 4 Task 9, spec §5.5, B6).
   Pure (no ESP-IDF, no LVGL), so the host tests cover them. */
#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>
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
#define HF_HOST_LABELS_MAX 4
typedef struct {
    char a[HF_HOST_ADDRS_MAX][128]; int n;            /* addresses: cc_same_host */
    char label[HF_HOST_LABELS_MAX][64]; int nl;       /* first labels: only a *.ts.net name's, or a bare name (M-new-5) */
} hf_host_addrs_t;
/* The first DNS label of a dotted name (leading blanks skipped): false for an IPv4 address, a name with no dot, or a
   label that doesn't fit. */
bool hf_first_label(const char *host, char *out, size_t cap);
void hf_host_addrs_init(hf_host_addrs_t *h);
void hf_host_addrs_add(hf_host_addrs_t *h, const char *addr);
/* addr and, when it is a dotted name, its first DNS label: "studio.local" also matches the peer's MagicDNS name
   "studio.<tailnet>.ts.net" and the bare "studio", but never "studio.example.com" (M-new-5). */
void hf_host_addrs_add_name(hf_host_addrs_t *h, const char *host);
/* addr names the Host: cc_same_host with any of h's addresses (NULL h: no Host). */
bool hf_is_host(const char *addr, const hf_host_addrs_t *h);
/* hf_validate against every known Host address (h may be NULL). */
hf_err_t hf_validate_addrs(const char *address, const char *port, const char *user, bool is_host_entry,
                           const hf_host_addrs_t *h, int *port_out);
/* I1: name is already another entry's (case-insensitive; names[self] is the entry being edited, -1 for a new one).
   The Host is chosen by name, so two entries with one name could swap which entry is the Host. */
bool hf_name_taken(const char *name, const char *const *names, int n, int self);

/* P4C-10: the Host is the server that presents a key the companion link has pinned. The link keeps every key it
   ever authenticated on (SHA256, 32 bytes) in a FIFO of HF_HOSTKEYS_MAX (NVS "cc"/"hostkeys"); it survives Forget
   and Host switches. The SSH Terminal refuses an interactive shell on any of them, after key exchange and before any
   authentication. */
#define HF_HOSTKEYS_MAX 8
/* fp is one of list's n keys (an all-zero fp is none). */
bool hf_hostkey_is_host(const uint8_t fp[32], const uint8_t (*list)[32], int n);
/* Appends fp (moved to the newest when already there; the oldest goes past HF_HOSTKEYS_MAX); returns the new n.
   An all-zero fp is ignored. */
int hf_hostkeys_add(uint8_t (*list)[32], int n, const uint8_t fp[32]);

/* I-new-2: the deny-list read: no list yet, a list, or it couldn't be read (open failure, corruption). */
typedef enum { HF_DENY_NONE, HF_DENY_FOUND, HF_DENY_ERROR } hf_deny_read_t;
typedef enum { HF_GATE_ALLOW, HF_GATE_REFUSE_HOST, HF_GATE_REFUSE_UNCHECKED } hf_gate_t;
/* The shell gate: a non-interactive session (the take-over's pty exec) is never gated. An interactive one is refused
   when the server's key (NULL: unreadable) is one of the Host's, and refused unchecked when either side can't be
   read. */
hf_gate_t hf_shell_gate(bool interactive, hf_deny_read_t rd, const uint8_t key[32], const uint8_t (*list)[32], int n);
/* An add writes only over a list it read (never over an unread one, which would erase it). */
bool hf_hostkeys_may_write(hf_deny_read_t rd);

/* The per-connection steps ssh_task runs after the key exchange, in order: an interactive session checks the
   deny-list first, before the known-hosts check and before any authentication. Returns n (0 if cap is too small). */
typedef enum { HF_STEP_DENY_CHECK, HF_STEP_VERIFY_HOST, HF_STEP_AUTH } hf_step_t;
int hf_conn_steps(bool interactive, hf_step_t *out, int cap);

/* Key-type hardening: the SSH_OPTIONS_HOSTKEYS value for an interactive session, libssh's "^" prefix form (0.11
   options.c ssh_options_set_algo -> kex.c ssh_prefix_default_algos): the algorithms for the key type the link last met
   (ssh_key_type_to_char) go first, and libssh puts the rest of its own default list after them without repeats, so
   it is a reorder only. "^rsa-sha2-512,rsa-sha2-256" for "ssh-rsa", "^<type>" for ed25519 and the ECDSA curves.
   False (set no option) for an unknown type or a buffer too small. */
bool hf_hostkeys_pref(const char *key_type, char *out, size_t cap);

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

#ifdef __cplusplus
}
#endif
