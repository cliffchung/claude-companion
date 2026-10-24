#pragma once
/* `key log` line formatting (Task 10's real-press check). Pure: no ESP-IDF headers, so it
 * also builds in firmware/test/host. A character typed without Ctrl, Meta or Alt is masked
 * as cp='*': the log never holds what was typed (passwords, remote prompts). */
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "input/keyboard.h"

#define KEY_LOG_LINE_MAX    96
#define KEY_LOG_AUTO_OFF_MS 300000   /* `key log on` turns itself off after 5 min */

/* key: type=<char|special> cp=<'c', '*' or hex> key=<name> mods=<ctrl,shift,alt,meta|none> */
void key_log_format(const key_event_t *ev, bool swallowed, char *out, size_t cap);
bool key_log_expired(int64_t started_ms, int64_t now_ms);
