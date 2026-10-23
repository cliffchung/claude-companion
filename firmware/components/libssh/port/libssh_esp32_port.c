/* Glue for using the LibSSH-ESP32 sources from a pure ESP-IDF project. */
#include "libssh_esp32_config.h"
#include "libssh/libssh.h"
#include "libssh/callbacks.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

/* Threading: more than one task runs a libssh session at a time (the terminal and the
 * companion link), each session touched by one task only. What they share is libssh's
 * global mbedtls state: the CTR-DRBG and entropy contexts. Those are locked by mbedtls
 * itself (CONFIG_MBEDTLS_THREADING_C + _PTHREAD; getrandom_mbedcrypto.c refuses to build
 * without it and also locks its strong-mode flag flip). libssh's own callbacks stay noop:
 * with MBEDTLS_THREADING_PTHREAD, crypto_thread_init() has nothing to install, and libssh's
 * only other global lock guards ssh_init(), which the once-guard below serializes. */
int libssh_esp32_init(void)
{
    static SemaphoreHandle_t s_once;
    static int s_rc = -1;
    static portMUX_TYPE s_mux = portMUX_INITIALIZER_UNLOCKED;
    if (!s_once) {
        SemaphoreHandle_t m = xSemaphoreCreateMutex();
        taskENTER_CRITICAL(&s_mux);
        if (!s_once) { s_once = m; m = NULL; }
        taskEXIT_CRITICAL(&s_mux);
        if (m) vSemaphoreDelete(m);
    }
    xSemaphoreTake(s_once, portMAX_DELAY);
    if (s_rc != 0) {
        ssh_threads_set_callbacks(ssh_threads_get_noop());
        s_rc = ssh_init();
    }
    xSemaphoreGive(s_once);
    return s_rc;
}
