#pragma once
typedef struct { char version[32]; char project_name[32]; char time[16]; char date[16]; } esp_app_desc_t;
static inline const esp_app_desc_t *esp_app_get_description(void)
{
    static const esp_app_desc_t d = { "preview", "claude_companion", "12:00:00", "Oct 24 2026" };
    return &d;
}
