#pragma once
#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#ifdef __cplusplus
extern "C" {
#endif

#define WIFI_MAX_SAVED 12
typedef struct { char ssid[33]; char pass[65]; } wifi_saved_t;
typedef struct { char ssid[33]; int8_t rssi; uint8_t auth; uint8_t channel; } wifi_scan_item_t;

typedef enum { WIFI_EV_CONNECTED, WIFI_EV_DISCONNECTED, WIFI_EV_CONNECT_FAILED, WIFI_EV_SCAN_DONE, WIFI_EV_GOT_IP6 } wifi_ev_t;
typedef void (*wifi_ev_cb_t)(wifi_ev_t ev, void *user);

void wifi_mgr_init(void);                  /* start driver, auto-connect to last used */
bool wifi_mgr_is_connected(void);
bool wifi_mgr_is_connecting(void);     /* association in progress */
bool wifi_mgr_is_started(void);
const char *wifi_mgr_ssid(void);
void wifi_mgr_get_ip4(char *buf, size_t len);
void wifi_mgr_get_ip6(char *buf, size_t len);    /* first global/link-local */
int  wifi_mgr_rssi(void);
int  wifi_mgr_channel(void);
void wifi_mgr_connect(const char *ssid, const char *pass, bool save);
void wifi_mgr_disconnect(void);
bool wifi_mgr_scan_start(void);
int  wifi_mgr_scan_results(wifi_scan_item_t *out, int max);
int  wifi_mgr_saved_count(void);
const wifi_saved_t *wifi_mgr_saved(int i);
void wifi_mgr_forget(int i);
void wifi_mgr_set_cb(wifi_ev_cb_t cb, void *user);          /* UI screen slot */
void wifi_mgr_set_global_cb(wifi_ev_cb_t cb, void *user);   /* app-wide slot */
/* Serialization for backup */
char *wifi_mgr_export_json(void);           /* caller frees */
bool wifi_mgr_import_json(const char *json);
const char *wifi_mgr_last_error(void);

/* ---- Plan 2 Task 8: background reconnect, the parked radio, modem power save ----
 * A drop that the user didn't ask for is retried by the `wifi_rc` task after pr_backoff_s(attempt)
 * seconds (1, 2, 4, 8, 16, then 30 s for ever) until an IP, a park or a user disconnect. */
/* Parks the radio for Radio off: esp_wifi_stop() (an RPC to the C6). If the stop fails, or an earlier
   unpark's esp_wifi_start() failed, it parks with esp_wifi_disconnect() + WIFI_PS_MAX_MODEM instead
   and logs `park mode: disconnect` (sticky until reboot). While parked a drop never reconnects and
   STA_START never auto-connects. True when the radio is parked by either method. Blocks (RPC). */
bool wifi_mgr_park(void);
/* Clears parked and brings the radio back: esp_wifi_start() (its STA_START joins the last network),
   or esp_wifi_connect() in disconnect mode. Blocks (RPC). */
void wifi_mgr_unpark(void);
bool wifi_mgr_parked(void);
const char *wifi_mgr_park_mode(void);       /* "stop" or "disconnect" (the mode the next park uses) */
/* WIFI_PS_MAX_MODEM (max) or WIFI_PS_MIN_MODEM; applied on the wifi_rc task, so any task may call it. */
void wifi_mgr_set_ps_max(bool max);
/* Plan 3 Task 3: WIFI_PS_NONE until released (a check-in boot's few seconds: no beacon-interval waits on
   DHCP, mDNS and SSH); set_ps_max's wish is kept meanwhile and applied when it is released. */
void wifi_mgr_ps_hold_none(bool on);
/* Debug (`wifi test drop`): esp_wifi_disconnect() without the user flag, as a real drop. */
void wifi_mgr_test_drop(void);
/* "reconnect: idle" | "reconnect: attempt <n>, next in <s> s" | "parked (<mode>)", for the console */
void wifi_mgr_reconnect_text(char *buf, size_t cap);

/* ---- Plan 3 Task 3 ---- */
/* The optional static IPv4 address (Settings > Network > Wi-Fi > Advanced), off by default. Addresses in
   network byte order (lwIP's ip4_addr_t.addr). Applied from the next join (STA_START). */
typedef struct { uint8_t on, pad[3]; uint32_t ip, mask, gw, dns; } wifi_static_ip_t;
bool wifi_mgr_static_ip_get(wifi_static_ip_t *out);
bool wifi_mgr_static_ip_set(const wifi_static_ip_t *in);   /* NULL or on = 0 turns it off; false if invalid */

#ifdef __cplusplus
}
#endif
