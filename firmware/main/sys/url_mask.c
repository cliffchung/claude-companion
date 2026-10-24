#include "url_mask.h"
#include <stdio.h>
#include <string.h>

void url_mask(const char *u, char *out, size_t cap)
{
    if (!out || !cap) return;
    if (!u || !u[0]) { snprintf(out, cap, "none"); return; }
    const char *sch = strstr(u, "://");
    const char *auth = sch ? sch + 3 : u;                  /* the authority: [userinfo@]host[:port] */
    size_t alen = strcspn(auth, "/?#");
    const char *at = memchr(auth, '@', alen);
    const char *host = at ? at + 1 : auth;
    size_t hlen = alen - (size_t)(host - auth);
    const char *q = strchr(u, '?');
    if (!at && !q) { snprintf(out, cap, "%s", u); return; }
    if (q) snprintf(out, cap, "%.*s%s%.*s/\xe2\x80\xa6?<masked>", (int)(auth - u), u, at ? "<masked>@" : "", (int)hlen, host);
    else snprintf(out, cap, "%.*s<masked>@%s", (int)(auth - u), u, host);
}
