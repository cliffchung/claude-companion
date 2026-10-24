#pragma once
#define LV_COLOR_FORMAT_DEFAULT LV_COLOR_FORMAT_XRGB8888
#define LV_USE_OS LV_OS_NONE
#define LV_MEM_SIZE (32 * 1024 * 1024)
/* as the firmware (sdkconfig.defaults): no transitions or press growth in the default theme */
#define LV_THEME_DEFAULT_TRANSITION_TIME 0
#define LV_THEME_DEFAULT_GROW 0
#define LV_USE_SNAPSHOT 1
#define LV_USE_LOG 1
#define LV_LOG_LEVEL LV_LOG_LEVEL_WARN

#define LV_FONT_FMT_TXT_LARGE 1
#define LV_USE_FONT_COMPRESSED 1
