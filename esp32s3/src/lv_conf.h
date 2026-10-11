// LVGL configuration, everything not set here uses the LVGL defaults (lv_conf_internal.h)

#ifndef LV_CONF_H
#define LV_CONF_H

#define LV_COLOR_DEPTH          16

#define LV_USE_STDLIB_MALLOC    LV_STDLIB_CLIB
#define LV_USE_STDLIB_STRING    LV_STDLIB_CLIB
#define LV_USE_STDLIB_SPRINTF   LV_STDLIB_CLIB

#define LV_USE_OS               LV_OS_NONE
#define LV_DEF_REFR_PERIOD      20
#define LV_DPI_DEF              130

#define LV_USE_LOG              0
#define LV_USE_ASSERT_NULL      1
#define LV_USE_ASSERT_MALLOC    1

#define LV_FONT_MONTSERRAT_12   1
#define LV_FONT_MONTSERRAT_14   1
#define LV_FONT_MONTSERRAT_16   1
#define LV_FONT_MONTSERRAT_20   1
#define LV_FONT_MONTSERRAT_24   1
#define LV_FONT_MONTSERRAT_28   1
#define LV_FONT_MONTSERRAT_36   1
#define LV_FONT_DEFAULT         &lv_font_montserrat_14

#define LV_USE_CANVAS           1
#define LV_USE_THEME_DEFAULT    1
#define LV_THEME_DEFAULT_DARK   1

#define LV_BUILD_EXAMPLES       0
#define LV_BUILD_DEMOS          0

#endif
