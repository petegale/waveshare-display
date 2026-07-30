#pragma once

#define LV_COLOR_DEPTH     16
#define LV_HOR_RES_MAX     800
#define LV_VER_RES_MAX     480

#define LV_MEM_CUSTOM      1
#define LV_MEM_CUSTOM_INCLUDE <esp_heap_caps.h>
#define LV_MEM_CUSTOM_ALLOC(size)     heap_caps_malloc((size), MALLOC_CAP_SPIRAM)
#define LV_MEM_CUSTOM_FREE(ptr)       heap_caps_free(ptr)
#define LV_MEM_CUSTOM_REALLOC(ptr, size) heap_caps_realloc((ptr), (size), MALLOC_CAP_SPIRAM)

#define LV_TICK_CUSTOM     1
#define LV_TICK_CUSTOM_INCLUDE "Arduino.h"
#define LV_TICK_CUSTOM_SYS_TIME_EXPR (millis())

#define LV_USE_ARC         1
#define LV_USE_LABEL       1
#define LV_USE_BAR         1
#define LV_USE_CHART       1
#define LV_USE_BTN         1

// Sizes actually referenced by display.cpp. Each enabled face costs
// flash, so keep this list in sync rather than enabling the whole range.
// 14 and 16 px are deliberately absent: below 20 px this panel's RGB stripe
// turns thin stems into red/white fringing, so those faces are not available
// to be reached for by accident.
#define LV_FONT_MONTSERRAT_20 1
#define LV_FONT_MONTSERRAT_24 1
#define LV_FONT_MONTSERRAT_28 1
#define LV_FONT_MONTSERRAT_48 1

#define LV_USE_LOG         0
#define LV_USE_ASSERT_NULL 1

#define LV_USE_GPU         0
#define LV_USE_FILESYSTEM  0
