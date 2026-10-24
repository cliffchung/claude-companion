#pragma once
/* How the console prints a URL that may carry a secret (the OTA manifest URL). Pure C. */
#include <stddef.h>
#ifdef __cplusplus
extern "C" {
#endif
/* "none" for an empty URL. Userinfo ("user:token@") prints as "<masked>@"; a query prints as
   "<scheme>://<host>/…?<masked>"; a URL with neither prints whole. */
void url_mask(const char *url, char *out, size_t cap);
#ifdef __cplusplus
}
#endif
