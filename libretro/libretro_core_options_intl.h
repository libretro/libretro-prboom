#ifndef LIBRETRO_CORE_OPTIONS_INTL_H__
#define LIBRETRO_CORE_OPTIONS_INTL_H__

#if defined(_MSC_VER) && (_MSC_VER >= 1500 && _MSC_VER < 1900)
/* https://support.microsoft.com/en-us/kb/980263 */
#pragma execution_character_set("utf-8")
#pragma warning(disable:4566)
#endif

#include <libretro.h>

#ifdef __cplusplus
extern "C" {
#endif

/*
 ********************************
 * Core Option Definitions
 ********************************
*/
/* RETRO_LANGUAGE_AR */

#define PRBOOM_RESOLUTION_LABEL_AR "الدقة الداخلية (إعادة التشغيل مطلوبة)"
#define PRBOOM_RESOLUTION_INFO_0_AR NULL
#define OPTION_VAL_320X200_AR NULL
#define OPTION_VAL_640X400_AR NULL
#define OPTION_VAL_960X600_AR NULL
#define OPTION_VAL_1280X800_AR NULL
#define OPTION_VAL_1600X1000_AR NULL
#define OPTION_VAL_1920X1200_AR NULL
#define OPTION_VAL_2240X1400_AR NULL
#define OPTION_VAL_2560X1600_AR NULL
#define PRBOOM_MOUSE_ON_LABEL_AR NULL
#define PRBOOM_MOUSE_ON_INFO_0_AR NULL
#define PRBOOM_FIND_RECURSIVE_ON_LABEL_AR NULL
#define PRBOOM_FIND_RECURSIVE_ON_INFO_0_AR NULL
#define PRBOOM_RUMBLE_LABEL_AR NULL
#define PRBOOM_RUMBLE_INFO_0_AR NULL
#define PRBOOM_ANALOG_DEADZONE_LABEL_AR NULL
#define PRBOOM_ANALOG_DEADZONE_INFO_0_AR NULL
#define PRBOOM_PURGE_LIMIT_LABEL_AR NULL
#define PRBOOM_PURGE_LIMIT_INFO_0_AR NULL
#define OPTION_VAL_8_AR NULL
#define OPTION_VAL_12_AR NULL
#define OPTION_VAL_16_AR NULL
#define OPTION_VAL_24_AR NULL
#define OPTION_VAL_32_AR NULL
#define OPTION_VAL_48_AR NULL
#define OPTION_VAL_64_AR NULL
#define OPTION_VAL_128_AR NULL
#define OPTION_VAL_256_AR NULL

struct retro_core_option_v2_category option_cats_ar[] = {
   { NULL, NULL, NULL },
};
struct retro_core_option_v2_definition option_defs_ar[] = {
   {
      "prboom-resolution",
      PRBOOM_RESOLUTION_LABEL_AR,
      NULL,
      PRBOOM_RESOLUTION_INFO_0_AR,
      NULL,
      NULL,
      {
         { "320x200",   OPTION_VAL_320X200_AR },
         { "640x400",   OPTION_VAL_640X400_AR },
         { "960x600",   OPTION_VAL_960X600_AR },
         { "1280x800",  OPTION_VAL_1280X800_AR },
         { "1600x1000", OPTION_VAL_1600X1000_AR },
         { "1920x1200", OPTION_VAL_1920X1200_AR },
         { "2240x1400", OPTION_VAL_2240X1400_AR },
         { "2560x1600", OPTION_VAL_2560X1600_AR },
         { NULL, NULL },
      },
      "320x200"
   },
   {
      "prboom-mouse_on",
      PRBOOM_MOUSE_ON_LABEL_AR,
      NULL,
      PRBOOM_MOUSE_ON_INFO_0_AR,
      NULL,
      NULL,
      {
         { "disabled", NULL },
         { "enabled",  NULL },
         { NULL, NULL },
      },
      "disabled"
   },
   {
      "prboom-find_recursive_on",
      PRBOOM_FIND_RECURSIVE_ON_LABEL_AR,
      NULL,
      PRBOOM_FIND_RECURSIVE_ON_INFO_0_AR,
      NULL,
      NULL,
      {
         { "disabled", NULL },
         { "enabled",  NULL },
         { NULL, NULL },
      },
      "enabled"
   },
   {
      "prboom-rumble",
      PRBOOM_RUMBLE_LABEL_AR,
      NULL,
      PRBOOM_RUMBLE_INFO_0_AR,
      NULL,
      NULL,
      {
         { "disabled", NULL },
         { "enabled",  NULL },
         { NULL, NULL },
      },
      "disabled"
   },
   {
      "prboom-analog_deadzone",
      PRBOOM_ANALOG_DEADZONE_LABEL_AR,
      NULL,
      PRBOOM_ANALOG_DEADZONE_INFO_0_AR,
      NULL,
      NULL,
      {
         { "0",  NULL },
         { "5",  NULL },
         { "10", NULL },
         { "15", NULL },
         { "20", NULL },
         { "25", NULL },
         { "30", NULL },
         { NULL, NULL },
      },
      "15"
   },
#if defined(MEMORY_LOW)
   {
      "prboom-purge_limit",
      PRBOOM_PURGE_LIMIT_LABEL_AR,
      NULL,
      PRBOOM_PURGE_LIMIT_INFO_0_AR,
      NULL,
      NULL,
      {
         { "8",   OPTION_VAL_8_AR },
         { "12",  OPTION_VAL_12_AR },
         { "16",  OPTION_VAL_16_AR },
         { "24",  OPTION_VAL_24_AR },
         { "32",  OPTION_VAL_32_AR },
         { "48",  OPTION_VAL_48_AR },
         { "64",  OPTION_VAL_64_AR },
         { "128", OPTION_VAL_128_AR },
         { "256", OPTION_VAL_256_AR },
         { NULL, NULL },
      },
      "16"
   },
#endif
   { NULL, NULL, NULL, NULL, NULL, NULL, {{0}}, NULL },
};
struct retro_core_options_v2 options_ar = {
   option_cats_ar,
   option_defs_ar
};

/* RETRO_LANGUAGE_AST */

#define PRBOOM_RESOLUTION_LABEL_AST NULL
#define PRBOOM_RESOLUTION_INFO_0_AST NULL
#define OPTION_VAL_320X200_AST NULL
#define OPTION_VAL_640X400_AST NULL
#define OPTION_VAL_960X600_AST NULL
#define OPTION_VAL_1280X800_AST NULL
#define OPTION_VAL_1600X1000_AST NULL
#define OPTION_VAL_1920X1200_AST NULL
#define OPTION_VAL_2240X1400_AST NULL
#define OPTION_VAL_2560X1600_AST NULL
#define PRBOOM_MOUSE_ON_LABEL_AST NULL
#define PRBOOM_MOUSE_ON_INFO_0_AST NULL
#define PRBOOM_FIND_RECURSIVE_ON_LABEL_AST NULL
#define PRBOOM_FIND_RECURSIVE_ON_INFO_0_AST NULL
#define PRBOOM_RUMBLE_LABEL_AST "Efeutos del vibrador"
#define PRBOOM_RUMBLE_INFO_0_AST NULL
#define PRBOOM_ANALOG_DEADZONE_LABEL_AST NULL
#define PRBOOM_ANALOG_DEADZONE_INFO_0_AST NULL
#define PRBOOM_PURGE_LIMIT_LABEL_AST "Tamañu de la caché"
#define PRBOOM_PURGE_LIMIT_INFO_0_AST NULL
#define OPTION_VAL_8_AST NULL
#define OPTION_VAL_12_AST NULL
#define OPTION_VAL_16_AST NULL
#define OPTION_VAL_24_AST NULL
#define OPTION_VAL_32_AST NULL
#define OPTION_VAL_48_AST NULL
#define OPTION_VAL_64_AST NULL
#define OPTION_VAL_128_AST NULL
#define OPTION_VAL_256_AST NULL

struct retro_core_option_v2_category option_cats_ast[] = {
   { NULL, NULL, NULL },
};
struct retro_core_option_v2_definition option_defs_ast[] = {
   {
      "prboom-resolution",
      PRBOOM_RESOLUTION_LABEL_AST,
      NULL,
      PRBOOM_RESOLUTION_INFO_0_AST,
      NULL,
      NULL,
      {
         { "320x200",   OPTION_VAL_320X200_AST },
         { "640x400",   OPTION_VAL_640X400_AST },
         { "960x600",   OPTION_VAL_960X600_AST },
         { "1280x800",  OPTION_VAL_1280X800_AST },
         { "1600x1000", OPTION_VAL_1600X1000_AST },
         { "1920x1200", OPTION_VAL_1920X1200_AST },
         { "2240x1400", OPTION_VAL_2240X1400_AST },
         { "2560x1600", OPTION_VAL_2560X1600_AST },
         { NULL, NULL },
      },
      "320x200"
   },
   {
      "prboom-mouse_on",
      PRBOOM_MOUSE_ON_LABEL_AST,
      NULL,
      PRBOOM_MOUSE_ON_INFO_0_AST,
      NULL,
      NULL,
      {
         { "disabled", NULL },
         { "enabled",  NULL },
         { NULL, NULL },
      },
      "disabled"
   },
   {
      "prboom-find_recursive_on",
      PRBOOM_FIND_RECURSIVE_ON_LABEL_AST,
      NULL,
      PRBOOM_FIND_RECURSIVE_ON_INFO_0_AST,
      NULL,
      NULL,
      {
         { "disabled", NULL },
         { "enabled",  NULL },
         { NULL, NULL },
      },
      "enabled"
   },
   {
      "prboom-rumble",
      PRBOOM_RUMBLE_LABEL_AST,
      NULL,
      PRBOOM_RUMBLE_INFO_0_AST,
      NULL,
      NULL,
      {
         { "disabled", NULL },
         { "enabled",  NULL },
         { NULL, NULL },
      },
      "disabled"
   },
   {
      "prboom-analog_deadzone",
      PRBOOM_ANALOG_DEADZONE_LABEL_AST,
      NULL,
      PRBOOM_ANALOG_DEADZONE_INFO_0_AST,
      NULL,
      NULL,
      {
         { "0",  NULL },
         { "5",  NULL },
         { "10", NULL },
         { "15", NULL },
         { "20", NULL },
         { "25", NULL },
         { "30", NULL },
         { NULL, NULL },
      },
      "15"
   },
#if defined(MEMORY_LOW)
   {
      "prboom-purge_limit",
      PRBOOM_PURGE_LIMIT_LABEL_AST,
      NULL,
      PRBOOM_PURGE_LIMIT_INFO_0_AST,
      NULL,
      NULL,
      {
         { "8",   OPTION_VAL_8_AST },
         { "12",  OPTION_VAL_12_AST },
         { "16",  OPTION_VAL_16_AST },
         { "24",  OPTION_VAL_24_AST },
         { "32",  OPTION_VAL_32_AST },
         { "48",  OPTION_VAL_48_AST },
         { "64",  OPTION_VAL_64_AST },
         { "128", OPTION_VAL_128_AST },
         { "256", OPTION_VAL_256_AST },
         { NULL, NULL },
      },
      "16"
   },
#endif
   { NULL, NULL, NULL, NULL, NULL, NULL, {{0}}, NULL },
};
struct retro_core_options_v2 options_ast = {
   option_cats_ast,
   option_defs_ast
};

/* RETRO_LANGUAGE_CA */

#define PRBOOM_RESOLUTION_LABEL_CA NULL
#define PRBOOM_RESOLUTION_INFO_0_CA NULL
#define OPTION_VAL_320X200_CA NULL
#define OPTION_VAL_640X400_CA NULL
#define OPTION_VAL_960X600_CA NULL
#define OPTION_VAL_1280X800_CA NULL
#define OPTION_VAL_1600X1000_CA NULL
#define OPTION_VAL_1920X1200_CA NULL
#define OPTION_VAL_2240X1400_CA NULL
#define OPTION_VAL_2560X1600_CA NULL
#define PRBOOM_MOUSE_ON_LABEL_CA NULL
#define PRBOOM_MOUSE_ON_INFO_0_CA NULL
#define PRBOOM_FIND_RECURSIVE_ON_LABEL_CA NULL
#define PRBOOM_FIND_RECURSIVE_ON_INFO_0_CA NULL
#define PRBOOM_RUMBLE_LABEL_CA NULL
#define PRBOOM_RUMBLE_INFO_0_CA NULL
#define PRBOOM_ANALOG_DEADZONE_LABEL_CA NULL
#define PRBOOM_ANALOG_DEADZONE_INFO_0_CA NULL
#define PRBOOM_PURGE_LIMIT_LABEL_CA NULL
#define PRBOOM_PURGE_LIMIT_INFO_0_CA NULL
#define OPTION_VAL_8_CA NULL
#define OPTION_VAL_12_CA NULL
#define OPTION_VAL_16_CA NULL
#define OPTION_VAL_24_CA NULL
#define OPTION_VAL_32_CA NULL
#define OPTION_VAL_48_CA NULL
#define OPTION_VAL_64_CA NULL
#define OPTION_VAL_128_CA NULL
#define OPTION_VAL_256_CA NULL

struct retro_core_option_v2_category option_cats_ca[] = {
   { NULL, NULL, NULL },
};
struct retro_core_option_v2_definition option_defs_ca[] = {
   {
      "prboom-resolution",
      PRBOOM_RESOLUTION_LABEL_CA,
      NULL,
      PRBOOM_RESOLUTION_INFO_0_CA,
      NULL,
      NULL,
      {
         { "320x200",   OPTION_VAL_320X200_CA },
         { "640x400",   OPTION_VAL_640X400_CA },
         { "960x600",   OPTION_VAL_960X600_CA },
         { "1280x800",  OPTION_VAL_1280X800_CA },
         { "1600x1000", OPTION_VAL_1600X1000_CA },
         { "1920x1200", OPTION_VAL_1920X1200_CA },
         { "2240x1400", OPTION_VAL_2240X1400_CA },
         { "2560x1600", OPTION_VAL_2560X1600_CA },
         { NULL, NULL },
      },
      "320x200"
   },
   {
      "prboom-mouse_on",
      PRBOOM_MOUSE_ON_LABEL_CA,
      NULL,
      PRBOOM_MOUSE_ON_INFO_0_CA,
      NULL,
      NULL,
      {
         { "disabled", NULL },
         { "enabled",  NULL },
         { NULL, NULL },
      },
      "disabled"
   },
   {
      "prboom-find_recursive_on",
      PRBOOM_FIND_RECURSIVE_ON_LABEL_CA,
      NULL,
      PRBOOM_FIND_RECURSIVE_ON_INFO_0_CA,
      NULL,
      NULL,
      {
         { "disabled", NULL },
         { "enabled",  NULL },
         { NULL, NULL },
      },
      "enabled"
   },
   {
      "prboom-rumble",
      PRBOOM_RUMBLE_LABEL_CA,
      NULL,
      PRBOOM_RUMBLE_INFO_0_CA,
      NULL,
      NULL,
      {
         { "disabled", NULL },
         { "enabled",  NULL },
         { NULL, NULL },
      },
      "disabled"
   },
   {
      "prboom-analog_deadzone",
      PRBOOM_ANALOG_DEADZONE_LABEL_CA,
      NULL,
      PRBOOM_ANALOG_DEADZONE_INFO_0_CA,
      NULL,
      NULL,
      {
         { "0",  NULL },
         { "5",  NULL },
         { "10", NULL },
         { "15", NULL },
         { "20", NULL },
         { "25", NULL },
         { "30", NULL },
         { NULL, NULL },
      },
      "15"
   },
#if defined(MEMORY_LOW)
   {
      "prboom-purge_limit",
      PRBOOM_PURGE_LIMIT_LABEL_CA,
      NULL,
      PRBOOM_PURGE_LIMIT_INFO_0_CA,
      NULL,
      NULL,
      {
         { "8",   OPTION_VAL_8_CA },
         { "12",  OPTION_VAL_12_CA },
         { "16",  OPTION_VAL_16_CA },
         { "24",  OPTION_VAL_24_CA },
         { "32",  OPTION_VAL_32_CA },
         { "48",  OPTION_VAL_48_CA },
         { "64",  OPTION_VAL_64_CA },
         { "128", OPTION_VAL_128_CA },
         { "256", OPTION_VAL_256_CA },
         { NULL, NULL },
      },
      "16"
   },
#endif
   { NULL, NULL, NULL, NULL, NULL, NULL, {{0}}, NULL },
};
struct retro_core_options_v2 options_ca = {
   option_cats_ca,
   option_defs_ca
};

/* RETRO_LANGUAGE_CHS */

#define PRBOOM_RESOLUTION_LABEL_CHS "内部分辨率(需要重启)"
#define PRBOOM_RESOLUTION_INFO_0_CHS "配置分辨率。"
#define OPTION_VAL_320X200_CHS NULL
#define OPTION_VAL_640X400_CHS NULL
#define OPTION_VAL_960X600_CHS NULL
#define OPTION_VAL_1280X800_CHS NULL
#define OPTION_VAL_1600X1000_CHS NULL
#define OPTION_VAL_1920X1200_CHS NULL
#define OPTION_VAL_2240X1400_CHS NULL
#define OPTION_VAL_2560X1600_CHS NULL
#define PRBOOM_MOUSE_ON_LABEL_CHS "当使用游戏手柄时鼠标激活"
#define PRBOOM_MOUSE_ON_INFO_0_CHS "允许您使用鼠标输入，即使用户 1 的设备类型未设置为 'RetroKeyboard/Mouse'。"
#define PRBOOM_FIND_RECURSIVE_ON_LABEL_CHS NULL
#define PRBOOM_FIND_RECURSIVE_ON_INFO_0_CHS NULL
#define PRBOOM_RUMBLE_LABEL_CHS NULL
#define PRBOOM_RUMBLE_INFO_0_CHS NULL
#define PRBOOM_ANALOG_DEADZONE_LABEL_CHS NULL
#define PRBOOM_ANALOG_DEADZONE_INFO_0_CHS NULL
#define PRBOOM_PURGE_LIMIT_LABEL_CHS "缓存大小"
#define PRBOOM_PURGE_LIMIT_INFO_0_CHS "设置用于缓存游戏数据的存储空间大小限制。设置过小的值可能会在浏览大地图时导致卡顿。"
#define OPTION_VAL_8_CHS NULL
#define OPTION_VAL_12_CHS NULL
#define OPTION_VAL_16_CHS NULL
#define OPTION_VAL_24_CHS NULL
#define OPTION_VAL_32_CHS NULL
#define OPTION_VAL_48_CHS NULL
#define OPTION_VAL_64_CHS NULL
#define OPTION_VAL_128_CHS NULL
#define OPTION_VAL_256_CHS NULL

struct retro_core_option_v2_category option_cats_chs[] = {
   { NULL, NULL, NULL },
};
struct retro_core_option_v2_definition option_defs_chs[] = {
   {
      "prboom-resolution",
      PRBOOM_RESOLUTION_LABEL_CHS,
      NULL,
      PRBOOM_RESOLUTION_INFO_0_CHS,
      NULL,
      NULL,
      {
         { "320x200",   OPTION_VAL_320X200_CHS },
         { "640x400",   OPTION_VAL_640X400_CHS },
         { "960x600",   OPTION_VAL_960X600_CHS },
         { "1280x800",  OPTION_VAL_1280X800_CHS },
         { "1600x1000", OPTION_VAL_1600X1000_CHS },
         { "1920x1200", OPTION_VAL_1920X1200_CHS },
         { "2240x1400", OPTION_VAL_2240X1400_CHS },
         { "2560x1600", OPTION_VAL_2560X1600_CHS },
         { NULL, NULL },
      },
      "320x200"
   },
   {
      "prboom-mouse_on",
      PRBOOM_MOUSE_ON_LABEL_CHS,
      NULL,
      PRBOOM_MOUSE_ON_INFO_0_CHS,
      NULL,
      NULL,
      {
         { "disabled", NULL },
         { "enabled",  NULL },
         { NULL, NULL },
      },
      "disabled"
   },
   {
      "prboom-find_recursive_on",
      PRBOOM_FIND_RECURSIVE_ON_LABEL_CHS,
      NULL,
      PRBOOM_FIND_RECURSIVE_ON_INFO_0_CHS,
      NULL,
      NULL,
      {
         { "disabled", NULL },
         { "enabled",  NULL },
         { NULL, NULL },
      },
      "enabled"
   },
   {
      "prboom-rumble",
      PRBOOM_RUMBLE_LABEL_CHS,
      NULL,
      PRBOOM_RUMBLE_INFO_0_CHS,
      NULL,
      NULL,
      {
         { "disabled", NULL },
         { "enabled",  NULL },
         { NULL, NULL },
      },
      "disabled"
   },
   {
      "prboom-analog_deadzone",
      PRBOOM_ANALOG_DEADZONE_LABEL_CHS,
      NULL,
      PRBOOM_ANALOG_DEADZONE_INFO_0_CHS,
      NULL,
      NULL,
      {
         { "0",  NULL },
         { "5",  NULL },
         { "10", NULL },
         { "15", NULL },
         { "20", NULL },
         { "25", NULL },
         { "30", NULL },
         { NULL, NULL },
      },
      "15"
   },
#if defined(MEMORY_LOW)
   {
      "prboom-purge_limit",
      PRBOOM_PURGE_LIMIT_LABEL_CHS,
      NULL,
      PRBOOM_PURGE_LIMIT_INFO_0_CHS,
      NULL,
      NULL,
      {
         { "8",   OPTION_VAL_8_CHS },
         { "12",  OPTION_VAL_12_CHS },
         { "16",  OPTION_VAL_16_CHS },
         { "24",  OPTION_VAL_24_CHS },
         { "32",  OPTION_VAL_32_CHS },
         { "48",  OPTION_VAL_48_CHS },
         { "64",  OPTION_VAL_64_CHS },
         { "128", OPTION_VAL_128_CHS },
         { "256", OPTION_VAL_256_CHS },
         { NULL, NULL },
      },
      "16"
   },
#endif
   { NULL, NULL, NULL, NULL, NULL, NULL, {{0}}, NULL },
};
struct retro_core_options_v2 options_chs = {
   option_cats_chs,
   option_defs_chs
};

/* RETRO_LANGUAGE_CHT */

#define PRBOOM_RESOLUTION_LABEL_CHT NULL
#define PRBOOM_RESOLUTION_INFO_0_CHT NULL
#define OPTION_VAL_320X200_CHT NULL
#define OPTION_VAL_640X400_CHT NULL
#define OPTION_VAL_960X600_CHT NULL
#define OPTION_VAL_1280X800_CHT NULL
#define OPTION_VAL_1600X1000_CHT NULL
#define OPTION_VAL_1920X1200_CHT NULL
#define OPTION_VAL_2240X1400_CHT NULL
#define OPTION_VAL_2560X1600_CHT NULL
#define PRBOOM_MOUSE_ON_LABEL_CHT NULL
#define PRBOOM_MOUSE_ON_INFO_0_CHT NULL
#define PRBOOM_FIND_RECURSIVE_ON_LABEL_CHT NULL
#define PRBOOM_FIND_RECURSIVE_ON_INFO_0_CHT NULL
#define PRBOOM_RUMBLE_LABEL_CHT NULL
#define PRBOOM_RUMBLE_INFO_0_CHT NULL
#define PRBOOM_ANALOG_DEADZONE_LABEL_CHT NULL
#define PRBOOM_ANALOG_DEADZONE_INFO_0_CHT NULL
#define PRBOOM_PURGE_LIMIT_LABEL_CHT NULL
#define PRBOOM_PURGE_LIMIT_INFO_0_CHT NULL
#define OPTION_VAL_8_CHT NULL
#define OPTION_VAL_12_CHT NULL
#define OPTION_VAL_16_CHT NULL
#define OPTION_VAL_24_CHT NULL
#define OPTION_VAL_32_CHT NULL
#define OPTION_VAL_48_CHT NULL
#define OPTION_VAL_64_CHT NULL
#define OPTION_VAL_128_CHT NULL
#define OPTION_VAL_256_CHT NULL

struct retro_core_option_v2_category option_cats_cht[] = {
   { NULL, NULL, NULL },
};
struct retro_core_option_v2_definition option_defs_cht[] = {
   {
      "prboom-resolution",
      PRBOOM_RESOLUTION_LABEL_CHT,
      NULL,
      PRBOOM_RESOLUTION_INFO_0_CHT,
      NULL,
      NULL,
      {
         { "320x200",   OPTION_VAL_320X200_CHT },
         { "640x400",   OPTION_VAL_640X400_CHT },
         { "960x600",   OPTION_VAL_960X600_CHT },
         { "1280x800",  OPTION_VAL_1280X800_CHT },
         { "1600x1000", OPTION_VAL_1600X1000_CHT },
         { "1920x1200", OPTION_VAL_1920X1200_CHT },
         { "2240x1400", OPTION_VAL_2240X1400_CHT },
         { "2560x1600", OPTION_VAL_2560X1600_CHT },
         { NULL, NULL },
      },
      "320x200"
   },
   {
      "prboom-mouse_on",
      PRBOOM_MOUSE_ON_LABEL_CHT,
      NULL,
      PRBOOM_MOUSE_ON_INFO_0_CHT,
      NULL,
      NULL,
      {
         { "disabled", NULL },
         { "enabled",  NULL },
         { NULL, NULL },
      },
      "disabled"
   },
   {
      "prboom-find_recursive_on",
      PRBOOM_FIND_RECURSIVE_ON_LABEL_CHT,
      NULL,
      PRBOOM_FIND_RECURSIVE_ON_INFO_0_CHT,
      NULL,
      NULL,
      {
         { "disabled", NULL },
         { "enabled",  NULL },
         { NULL, NULL },
      },
      "enabled"
   },
   {
      "prboom-rumble",
      PRBOOM_RUMBLE_LABEL_CHT,
      NULL,
      PRBOOM_RUMBLE_INFO_0_CHT,
      NULL,
      NULL,
      {
         { "disabled", NULL },
         { "enabled",  NULL },
         { NULL, NULL },
      },
      "disabled"
   },
   {
      "prboom-analog_deadzone",
      PRBOOM_ANALOG_DEADZONE_LABEL_CHT,
      NULL,
      PRBOOM_ANALOG_DEADZONE_INFO_0_CHT,
      NULL,
      NULL,
      {
         { "0",  NULL },
         { "5",  NULL },
         { "10", NULL },
         { "15", NULL },
         { "20", NULL },
         { "25", NULL },
         { "30", NULL },
         { NULL, NULL },
      },
      "15"
   },
#if defined(MEMORY_LOW)
   {
      "prboom-purge_limit",
      PRBOOM_PURGE_LIMIT_LABEL_CHT,
      NULL,
      PRBOOM_PURGE_LIMIT_INFO_0_CHT,
      NULL,
      NULL,
      {
         { "8",   OPTION_VAL_8_CHT },
         { "12",  OPTION_VAL_12_CHT },
         { "16",  OPTION_VAL_16_CHT },
         { "24",  OPTION_VAL_24_CHT },
         { "32",  OPTION_VAL_32_CHT },
         { "48",  OPTION_VAL_48_CHT },
         { "64",  OPTION_VAL_64_CHT },
         { "128", OPTION_VAL_128_CHT },
         { "256", OPTION_VAL_256_CHT },
         { NULL, NULL },
      },
      "16"
   },
#endif
   { NULL, NULL, NULL, NULL, NULL, NULL, {{0}}, NULL },
};
struct retro_core_options_v2 options_cht = {
   option_cats_cht,
   option_defs_cht
};

/* RETRO_LANGUAGE_CS */

#define PRBOOM_RESOLUTION_LABEL_CS NULL
#define PRBOOM_RESOLUTION_INFO_0_CS NULL
#define OPTION_VAL_320X200_CS NULL
#define OPTION_VAL_640X400_CS NULL
#define OPTION_VAL_960X600_CS NULL
#define OPTION_VAL_1280X800_CS NULL
#define OPTION_VAL_1600X1000_CS NULL
#define OPTION_VAL_1920X1200_CS NULL
#define OPTION_VAL_2240X1400_CS NULL
#define OPTION_VAL_2560X1600_CS NULL
#define PRBOOM_MOUSE_ON_LABEL_CS NULL
#define PRBOOM_MOUSE_ON_INFO_0_CS NULL
#define PRBOOM_FIND_RECURSIVE_ON_LABEL_CS NULL
#define PRBOOM_FIND_RECURSIVE_ON_INFO_0_CS NULL
#define PRBOOM_RUMBLE_LABEL_CS NULL
#define PRBOOM_RUMBLE_INFO_0_CS NULL
#define PRBOOM_ANALOG_DEADZONE_LABEL_CS NULL
#define PRBOOM_ANALOG_DEADZONE_INFO_0_CS NULL
#define PRBOOM_PURGE_LIMIT_LABEL_CS NULL
#define PRBOOM_PURGE_LIMIT_INFO_0_CS NULL
#define OPTION_VAL_8_CS NULL
#define OPTION_VAL_12_CS NULL
#define OPTION_VAL_16_CS NULL
#define OPTION_VAL_24_CS NULL
#define OPTION_VAL_32_CS NULL
#define OPTION_VAL_48_CS NULL
#define OPTION_VAL_64_CS NULL
#define OPTION_VAL_128_CS NULL
#define OPTION_VAL_256_CS NULL

struct retro_core_option_v2_category option_cats_cs[] = {
   { NULL, NULL, NULL },
};
struct retro_core_option_v2_definition option_defs_cs[] = {
   {
      "prboom-resolution",
      PRBOOM_RESOLUTION_LABEL_CS,
      NULL,
      PRBOOM_RESOLUTION_INFO_0_CS,
      NULL,
      NULL,
      {
         { "320x200",   OPTION_VAL_320X200_CS },
         { "640x400",   OPTION_VAL_640X400_CS },
         { "960x600",   OPTION_VAL_960X600_CS },
         { "1280x800",  OPTION_VAL_1280X800_CS },
         { "1600x1000", OPTION_VAL_1600X1000_CS },
         { "1920x1200", OPTION_VAL_1920X1200_CS },
         { "2240x1400", OPTION_VAL_2240X1400_CS },
         { "2560x1600", OPTION_VAL_2560X1600_CS },
         { NULL, NULL },
      },
      "320x200"
   },
   {
      "prboom-mouse_on",
      PRBOOM_MOUSE_ON_LABEL_CS,
      NULL,
      PRBOOM_MOUSE_ON_INFO_0_CS,
      NULL,
      NULL,
      {
         { "disabled", NULL },
         { "enabled",  NULL },
         { NULL, NULL },
      },
      "disabled"
   },
   {
      "prboom-find_recursive_on",
      PRBOOM_FIND_RECURSIVE_ON_LABEL_CS,
      NULL,
      PRBOOM_FIND_RECURSIVE_ON_INFO_0_CS,
      NULL,
      NULL,
      {
         { "disabled", NULL },
         { "enabled",  NULL },
         { NULL, NULL },
      },
      "enabled"
   },
   {
      "prboom-rumble",
      PRBOOM_RUMBLE_LABEL_CS,
      NULL,
      PRBOOM_RUMBLE_INFO_0_CS,
      NULL,
      NULL,
      {
         { "disabled", NULL },
         { "enabled",  NULL },
         { NULL, NULL },
      },
      "disabled"
   },
   {
      "prboom-analog_deadzone",
      PRBOOM_ANALOG_DEADZONE_LABEL_CS,
      NULL,
      PRBOOM_ANALOG_DEADZONE_INFO_0_CS,
      NULL,
      NULL,
      {
         { "0",  NULL },
         { "5",  NULL },
         { "10", NULL },
         { "15", NULL },
         { "20", NULL },
         { "25", NULL },
         { "30", NULL },
         { NULL, NULL },
      },
      "15"
   },
#if defined(MEMORY_LOW)
   {
      "prboom-purge_limit",
      PRBOOM_PURGE_LIMIT_LABEL_CS,
      NULL,
      PRBOOM_PURGE_LIMIT_INFO_0_CS,
      NULL,
      NULL,
      {
         { "8",   OPTION_VAL_8_CS },
         { "12",  OPTION_VAL_12_CS },
         { "16",  OPTION_VAL_16_CS },
         { "24",  OPTION_VAL_24_CS },
         { "32",  OPTION_VAL_32_CS },
         { "48",  OPTION_VAL_48_CS },
         { "64",  OPTION_VAL_64_CS },
         { "128", OPTION_VAL_128_CS },
         { "256", OPTION_VAL_256_CS },
         { NULL, NULL },
      },
      "16"
   },
#endif
   { NULL, NULL, NULL, NULL, NULL, NULL, {{0}}, NULL },
};
struct retro_core_options_v2 options_cs = {
   option_cats_cs,
   option_defs_cs
};

/* RETRO_LANGUAGE_CY */

#define PRBOOM_RESOLUTION_LABEL_CY NULL
#define PRBOOM_RESOLUTION_INFO_0_CY NULL
#define OPTION_VAL_320X200_CY NULL
#define OPTION_VAL_640X400_CY NULL
#define OPTION_VAL_960X600_CY NULL
#define OPTION_VAL_1280X800_CY NULL
#define OPTION_VAL_1600X1000_CY NULL
#define OPTION_VAL_1920X1200_CY NULL
#define OPTION_VAL_2240X1400_CY NULL
#define OPTION_VAL_2560X1600_CY NULL
#define PRBOOM_MOUSE_ON_LABEL_CY NULL
#define PRBOOM_MOUSE_ON_INFO_0_CY NULL
#define PRBOOM_FIND_RECURSIVE_ON_LABEL_CY NULL
#define PRBOOM_FIND_RECURSIVE_ON_INFO_0_CY NULL
#define PRBOOM_RUMBLE_LABEL_CY NULL
#define PRBOOM_RUMBLE_INFO_0_CY NULL
#define PRBOOM_ANALOG_DEADZONE_LABEL_CY NULL
#define PRBOOM_ANALOG_DEADZONE_INFO_0_CY NULL
#define PRBOOM_PURGE_LIMIT_LABEL_CY NULL
#define PRBOOM_PURGE_LIMIT_INFO_0_CY NULL
#define OPTION_VAL_8_CY NULL
#define OPTION_VAL_12_CY NULL
#define OPTION_VAL_16_CY NULL
#define OPTION_VAL_24_CY NULL
#define OPTION_VAL_32_CY NULL
#define OPTION_VAL_48_CY NULL
#define OPTION_VAL_64_CY NULL
#define OPTION_VAL_128_CY NULL
#define OPTION_VAL_256_CY NULL

struct retro_core_option_v2_category option_cats_cy[] = {
   { NULL, NULL, NULL },
};
struct retro_core_option_v2_definition option_defs_cy[] = {
   {
      "prboom-resolution",
      PRBOOM_RESOLUTION_LABEL_CY,
      NULL,
      PRBOOM_RESOLUTION_INFO_0_CY,
      NULL,
      NULL,
      {
         { "320x200",   OPTION_VAL_320X200_CY },
         { "640x400",   OPTION_VAL_640X400_CY },
         { "960x600",   OPTION_VAL_960X600_CY },
         { "1280x800",  OPTION_VAL_1280X800_CY },
         { "1600x1000", OPTION_VAL_1600X1000_CY },
         { "1920x1200", OPTION_VAL_1920X1200_CY },
         { "2240x1400", OPTION_VAL_2240X1400_CY },
         { "2560x1600", OPTION_VAL_2560X1600_CY },
         { NULL, NULL },
      },
      "320x200"
   },
   {
      "prboom-mouse_on",
      PRBOOM_MOUSE_ON_LABEL_CY,
      NULL,
      PRBOOM_MOUSE_ON_INFO_0_CY,
      NULL,
      NULL,
      {
         { "disabled", NULL },
         { "enabled",  NULL },
         { NULL, NULL },
      },
      "disabled"
   },
   {
      "prboom-find_recursive_on",
      PRBOOM_FIND_RECURSIVE_ON_LABEL_CY,
      NULL,
      PRBOOM_FIND_RECURSIVE_ON_INFO_0_CY,
      NULL,
      NULL,
      {
         { "disabled", NULL },
         { "enabled",  NULL },
         { NULL, NULL },
      },
      "enabled"
   },
   {
      "prboom-rumble",
      PRBOOM_RUMBLE_LABEL_CY,
      NULL,
      PRBOOM_RUMBLE_INFO_0_CY,
      NULL,
      NULL,
      {
         { "disabled", NULL },
         { "enabled",  NULL },
         { NULL, NULL },
      },
      "disabled"
   },
   {
      "prboom-analog_deadzone",
      PRBOOM_ANALOG_DEADZONE_LABEL_CY,
      NULL,
      PRBOOM_ANALOG_DEADZONE_INFO_0_CY,
      NULL,
      NULL,
      {
         { "0",  NULL },
         { "5",  NULL },
         { "10", NULL },
         { "15", NULL },
         { "20", NULL },
         { "25", NULL },
         { "30", NULL },
         { NULL, NULL },
      },
      "15"
   },
#if defined(MEMORY_LOW)
   {
      "prboom-purge_limit",
      PRBOOM_PURGE_LIMIT_LABEL_CY,
      NULL,
      PRBOOM_PURGE_LIMIT_INFO_0_CY,
      NULL,
      NULL,
      {
         { "8",   OPTION_VAL_8_CY },
         { "12",  OPTION_VAL_12_CY },
         { "16",  OPTION_VAL_16_CY },
         { "24",  OPTION_VAL_24_CY },
         { "32",  OPTION_VAL_32_CY },
         { "48",  OPTION_VAL_48_CY },
         { "64",  OPTION_VAL_64_CY },
         { "128", OPTION_VAL_128_CY },
         { "256", OPTION_VAL_256_CY },
         { NULL, NULL },
      },
      "16"
   },
#endif
   { NULL, NULL, NULL, NULL, NULL, NULL, {{0}}, NULL },
};
struct retro_core_options_v2 options_cy = {
   option_cats_cy,
   option_defs_cy
};

/* RETRO_LANGUAGE_DA */

#define PRBOOM_RESOLUTION_LABEL_DA NULL
#define PRBOOM_RESOLUTION_INFO_0_DA NULL
#define OPTION_VAL_320X200_DA NULL
#define OPTION_VAL_640X400_DA NULL
#define OPTION_VAL_960X600_DA NULL
#define OPTION_VAL_1280X800_DA NULL
#define OPTION_VAL_1600X1000_DA NULL
#define OPTION_VAL_1920X1200_DA NULL
#define OPTION_VAL_2240X1400_DA NULL
#define OPTION_VAL_2560X1600_DA NULL
#define PRBOOM_MOUSE_ON_LABEL_DA NULL
#define PRBOOM_MOUSE_ON_INFO_0_DA NULL
#define PRBOOM_FIND_RECURSIVE_ON_LABEL_DA NULL
#define PRBOOM_FIND_RECURSIVE_ON_INFO_0_DA NULL
#define PRBOOM_RUMBLE_LABEL_DA NULL
#define PRBOOM_RUMBLE_INFO_0_DA NULL
#define PRBOOM_ANALOG_DEADZONE_LABEL_DA NULL
#define PRBOOM_ANALOG_DEADZONE_INFO_0_DA NULL
#define PRBOOM_PURGE_LIMIT_LABEL_DA NULL
#define PRBOOM_PURGE_LIMIT_INFO_0_DA NULL
#define OPTION_VAL_8_DA NULL
#define OPTION_VAL_12_DA NULL
#define OPTION_VAL_16_DA NULL
#define OPTION_VAL_24_DA NULL
#define OPTION_VAL_32_DA NULL
#define OPTION_VAL_48_DA NULL
#define OPTION_VAL_64_DA NULL
#define OPTION_VAL_128_DA NULL
#define OPTION_VAL_256_DA NULL

struct retro_core_option_v2_category option_cats_da[] = {
   { NULL, NULL, NULL },
};
struct retro_core_option_v2_definition option_defs_da[] = {
   {
      "prboom-resolution",
      PRBOOM_RESOLUTION_LABEL_DA,
      NULL,
      PRBOOM_RESOLUTION_INFO_0_DA,
      NULL,
      NULL,
      {
         { "320x200",   OPTION_VAL_320X200_DA },
         { "640x400",   OPTION_VAL_640X400_DA },
         { "960x600",   OPTION_VAL_960X600_DA },
         { "1280x800",  OPTION_VAL_1280X800_DA },
         { "1600x1000", OPTION_VAL_1600X1000_DA },
         { "1920x1200", OPTION_VAL_1920X1200_DA },
         { "2240x1400", OPTION_VAL_2240X1400_DA },
         { "2560x1600", OPTION_VAL_2560X1600_DA },
         { NULL, NULL },
      },
      "320x200"
   },
   {
      "prboom-mouse_on",
      PRBOOM_MOUSE_ON_LABEL_DA,
      NULL,
      PRBOOM_MOUSE_ON_INFO_0_DA,
      NULL,
      NULL,
      {
         { "disabled", NULL },
         { "enabled",  NULL },
         { NULL, NULL },
      },
      "disabled"
   },
   {
      "prboom-find_recursive_on",
      PRBOOM_FIND_RECURSIVE_ON_LABEL_DA,
      NULL,
      PRBOOM_FIND_RECURSIVE_ON_INFO_0_DA,
      NULL,
      NULL,
      {
         { "disabled", NULL },
         { "enabled",  NULL },
         { NULL, NULL },
      },
      "enabled"
   },
   {
      "prboom-rumble",
      PRBOOM_RUMBLE_LABEL_DA,
      NULL,
      PRBOOM_RUMBLE_INFO_0_DA,
      NULL,
      NULL,
      {
         { "disabled", NULL },
         { "enabled",  NULL },
         { NULL, NULL },
      },
      "disabled"
   },
   {
      "prboom-analog_deadzone",
      PRBOOM_ANALOG_DEADZONE_LABEL_DA,
      NULL,
      PRBOOM_ANALOG_DEADZONE_INFO_0_DA,
      NULL,
      NULL,
      {
         { "0",  NULL },
         { "5",  NULL },
         { "10", NULL },
         { "15", NULL },
         { "20", NULL },
         { "25", NULL },
         { "30", NULL },
         { NULL, NULL },
      },
      "15"
   },
#if defined(MEMORY_LOW)
   {
      "prboom-purge_limit",
      PRBOOM_PURGE_LIMIT_LABEL_DA,
      NULL,
      PRBOOM_PURGE_LIMIT_INFO_0_DA,
      NULL,
      NULL,
      {
         { "8",   OPTION_VAL_8_DA },
         { "12",  OPTION_VAL_12_DA },
         { "16",  OPTION_VAL_16_DA },
         { "24",  OPTION_VAL_24_DA },
         { "32",  OPTION_VAL_32_DA },
         { "48",  OPTION_VAL_48_DA },
         { "64",  OPTION_VAL_64_DA },
         { "128", OPTION_VAL_128_DA },
         { "256", OPTION_VAL_256_DA },
         { NULL, NULL },
      },
      "16"
   },
#endif
   { NULL, NULL, NULL, NULL, NULL, NULL, {{0}}, NULL },
};
struct retro_core_options_v2 options_da = {
   option_cats_da,
   option_defs_da
};

/* RETRO_LANGUAGE_DE */

#define PRBOOM_RESOLUTION_LABEL_DE "Interne Auflösung (Neustart erforderlich)"
#define PRBOOM_RESOLUTION_INFO_0_DE NULL
#define OPTION_VAL_320X200_DE "320 x 200"
#define OPTION_VAL_640X400_DE "640 x 400"
#define OPTION_VAL_960X600_DE "960 x 600"
#define OPTION_VAL_1280X800_DE "1280 x 800"
#define OPTION_VAL_1600X1000_DE "1600 x 1000"
#define OPTION_VAL_1920X1200_DE "1920 x 1200"
#define OPTION_VAL_2240X1400_DE "2240 x 1400"
#define OPTION_VAL_2560X1600_DE "2560 x 1600"
#define PRBOOM_MOUSE_ON_LABEL_DE NULL
#define PRBOOM_MOUSE_ON_INFO_0_DE NULL
#define PRBOOM_FIND_RECURSIVE_ON_LABEL_DE NULL
#define PRBOOM_FIND_RECURSIVE_ON_INFO_0_DE NULL
#define PRBOOM_RUMBLE_LABEL_DE "Rumpel-Effekte"
#define PRBOOM_RUMBLE_INFO_0_DE NULL
#define PRBOOM_ANALOG_DEADZONE_LABEL_DE NULL
#define PRBOOM_ANALOG_DEADZONE_INFO_0_DE NULL
#define PRBOOM_PURGE_LIMIT_LABEL_DE NULL
#define PRBOOM_PURGE_LIMIT_INFO_0_DE NULL
#define OPTION_VAL_8_DE NULL
#define OPTION_VAL_12_DE NULL
#define OPTION_VAL_16_DE NULL
#define OPTION_VAL_24_DE NULL
#define OPTION_VAL_32_DE NULL
#define OPTION_VAL_48_DE NULL
#define OPTION_VAL_64_DE NULL
#define OPTION_VAL_128_DE NULL
#define OPTION_VAL_256_DE NULL

struct retro_core_option_v2_category option_cats_de[] = {
   { NULL, NULL, NULL },
};
struct retro_core_option_v2_definition option_defs_de[] = {
   {
      "prboom-resolution",
      PRBOOM_RESOLUTION_LABEL_DE,
      NULL,
      PRBOOM_RESOLUTION_INFO_0_DE,
      NULL,
      NULL,
      {
         { "320x200",   OPTION_VAL_320X200_DE },
         { "640x400",   OPTION_VAL_640X400_DE },
         { "960x600",   OPTION_VAL_960X600_DE },
         { "1280x800",  OPTION_VAL_1280X800_DE },
         { "1600x1000", OPTION_VAL_1600X1000_DE },
         { "1920x1200", OPTION_VAL_1920X1200_DE },
         { "2240x1400", OPTION_VAL_2240X1400_DE },
         { "2560x1600", OPTION_VAL_2560X1600_DE },
         { NULL, NULL },
      },
      "320x200"
   },
   {
      "prboom-mouse_on",
      PRBOOM_MOUSE_ON_LABEL_DE,
      NULL,
      PRBOOM_MOUSE_ON_INFO_0_DE,
      NULL,
      NULL,
      {
         { "disabled", NULL },
         { "enabled",  NULL },
         { NULL, NULL },
      },
      "disabled"
   },
   {
      "prboom-find_recursive_on",
      PRBOOM_FIND_RECURSIVE_ON_LABEL_DE,
      NULL,
      PRBOOM_FIND_RECURSIVE_ON_INFO_0_DE,
      NULL,
      NULL,
      {
         { "disabled", NULL },
         { "enabled",  NULL },
         { NULL, NULL },
      },
      "enabled"
   },
   {
      "prboom-rumble",
      PRBOOM_RUMBLE_LABEL_DE,
      NULL,
      PRBOOM_RUMBLE_INFO_0_DE,
      NULL,
      NULL,
      {
         { "disabled", NULL },
         { "enabled",  NULL },
         { NULL, NULL },
      },
      "disabled"
   },
   {
      "prboom-analog_deadzone",
      PRBOOM_ANALOG_DEADZONE_LABEL_DE,
      NULL,
      PRBOOM_ANALOG_DEADZONE_INFO_0_DE,
      NULL,
      NULL,
      {
         { "0",  NULL },
         { "5",  NULL },
         { "10", NULL },
         { "15", NULL },
         { "20", NULL },
         { "25", NULL },
         { "30", NULL },
         { NULL, NULL },
      },
      "15"
   },
#if defined(MEMORY_LOW)
   {
      "prboom-purge_limit",
      PRBOOM_PURGE_LIMIT_LABEL_DE,
      NULL,
      PRBOOM_PURGE_LIMIT_INFO_0_DE,
      NULL,
      NULL,
      {
         { "8",   OPTION_VAL_8_DE },
         { "12",  OPTION_VAL_12_DE },
         { "16",  OPTION_VAL_16_DE },
         { "24",  OPTION_VAL_24_DE },
         { "32",  OPTION_VAL_32_DE },
         { "48",  OPTION_VAL_48_DE },
         { "64",  OPTION_VAL_64_DE },
         { "128", OPTION_VAL_128_DE },
         { "256", OPTION_VAL_256_DE },
         { NULL, NULL },
      },
      "16"
   },
#endif
   { NULL, NULL, NULL, NULL, NULL, NULL, {{0}}, NULL },
};
struct retro_core_options_v2 options_de = {
   option_cats_de,
   option_defs_de
};

/* RETRO_LANGUAGE_EL */

#define PRBOOM_RESOLUTION_LABEL_EL "Εσωτερική ανάλυση (Απαιτείται Επανεκκίνηση)"
#define PRBOOM_RESOLUTION_INFO_0_EL NULL
#define OPTION_VAL_320X200_EL NULL
#define OPTION_VAL_640X400_EL NULL
#define OPTION_VAL_960X600_EL NULL
#define OPTION_VAL_1280X800_EL NULL
#define OPTION_VAL_1600X1000_EL NULL
#define OPTION_VAL_1920X1200_EL NULL
#define OPTION_VAL_2240X1400_EL NULL
#define OPTION_VAL_2560X1600_EL NULL
#define PRBOOM_MOUSE_ON_LABEL_EL NULL
#define PRBOOM_MOUSE_ON_INFO_0_EL NULL
#define PRBOOM_FIND_RECURSIVE_ON_LABEL_EL NULL
#define PRBOOM_FIND_RECURSIVE_ON_INFO_0_EL NULL
#define PRBOOM_RUMBLE_LABEL_EL NULL
#define PRBOOM_RUMBLE_INFO_0_EL NULL
#define PRBOOM_ANALOG_DEADZONE_LABEL_EL NULL
#define PRBOOM_ANALOG_DEADZONE_INFO_0_EL NULL
#define PRBOOM_PURGE_LIMIT_LABEL_EL NULL
#define PRBOOM_PURGE_LIMIT_INFO_0_EL NULL
#define OPTION_VAL_8_EL NULL
#define OPTION_VAL_12_EL NULL
#define OPTION_VAL_16_EL NULL
#define OPTION_VAL_24_EL NULL
#define OPTION_VAL_32_EL NULL
#define OPTION_VAL_48_EL NULL
#define OPTION_VAL_64_EL NULL
#define OPTION_VAL_128_EL NULL
#define OPTION_VAL_256_EL NULL

struct retro_core_option_v2_category option_cats_el[] = {
   { NULL, NULL, NULL },
};
struct retro_core_option_v2_definition option_defs_el[] = {
   {
      "prboom-resolution",
      PRBOOM_RESOLUTION_LABEL_EL,
      NULL,
      PRBOOM_RESOLUTION_INFO_0_EL,
      NULL,
      NULL,
      {
         { "320x200",   OPTION_VAL_320X200_EL },
         { "640x400",   OPTION_VAL_640X400_EL },
         { "960x600",   OPTION_VAL_960X600_EL },
         { "1280x800",  OPTION_VAL_1280X800_EL },
         { "1600x1000", OPTION_VAL_1600X1000_EL },
         { "1920x1200", OPTION_VAL_1920X1200_EL },
         { "2240x1400", OPTION_VAL_2240X1400_EL },
         { "2560x1600", OPTION_VAL_2560X1600_EL },
         { NULL, NULL },
      },
      "320x200"
   },
   {
      "prboom-mouse_on",
      PRBOOM_MOUSE_ON_LABEL_EL,
      NULL,
      PRBOOM_MOUSE_ON_INFO_0_EL,
      NULL,
      NULL,
      {
         { "disabled", NULL },
         { "enabled",  NULL },
         { NULL, NULL },
      },
      "disabled"
   },
   {
      "prboom-find_recursive_on",
      PRBOOM_FIND_RECURSIVE_ON_LABEL_EL,
      NULL,
      PRBOOM_FIND_RECURSIVE_ON_INFO_0_EL,
      NULL,
      NULL,
      {
         { "disabled", NULL },
         { "enabled",  NULL },
         { NULL, NULL },
      },
      "enabled"
   },
   {
      "prboom-rumble",
      PRBOOM_RUMBLE_LABEL_EL,
      NULL,
      PRBOOM_RUMBLE_INFO_0_EL,
      NULL,
      NULL,
      {
         { "disabled", NULL },
         { "enabled",  NULL },
         { NULL, NULL },
      },
      "disabled"
   },
   {
      "prboom-analog_deadzone",
      PRBOOM_ANALOG_DEADZONE_LABEL_EL,
      NULL,
      PRBOOM_ANALOG_DEADZONE_INFO_0_EL,
      NULL,
      NULL,
      {
         { "0",  NULL },
         { "5",  NULL },
         { "10", NULL },
         { "15", NULL },
         { "20", NULL },
         { "25", NULL },
         { "30", NULL },
         { NULL, NULL },
      },
      "15"
   },
#if defined(MEMORY_LOW)
   {
      "prboom-purge_limit",
      PRBOOM_PURGE_LIMIT_LABEL_EL,
      NULL,
      PRBOOM_PURGE_LIMIT_INFO_0_EL,
      NULL,
      NULL,
      {
         { "8",   OPTION_VAL_8_EL },
         { "12",  OPTION_VAL_12_EL },
         { "16",  OPTION_VAL_16_EL },
         { "24",  OPTION_VAL_24_EL },
         { "32",  OPTION_VAL_32_EL },
         { "48",  OPTION_VAL_48_EL },
         { "64",  OPTION_VAL_64_EL },
         { "128", OPTION_VAL_128_EL },
         { "256", OPTION_VAL_256_EL },
         { NULL, NULL },
      },
      "16"
   },
#endif
   { NULL, NULL, NULL, NULL, NULL, NULL, {{0}}, NULL },
};
struct retro_core_options_v2 options_el = {
   option_cats_el,
   option_defs_el
};

/* RETRO_LANGUAGE_EO */

#define PRBOOM_RESOLUTION_LABEL_EO NULL
#define PRBOOM_RESOLUTION_INFO_0_EO NULL
#define OPTION_VAL_320X200_EO NULL
#define OPTION_VAL_640X400_EO NULL
#define OPTION_VAL_960X600_EO NULL
#define OPTION_VAL_1280X800_EO NULL
#define OPTION_VAL_1600X1000_EO NULL
#define OPTION_VAL_1920X1200_EO NULL
#define OPTION_VAL_2240X1400_EO NULL
#define OPTION_VAL_2560X1600_EO NULL
#define PRBOOM_MOUSE_ON_LABEL_EO NULL
#define PRBOOM_MOUSE_ON_INFO_0_EO NULL
#define PRBOOM_FIND_RECURSIVE_ON_LABEL_EO NULL
#define PRBOOM_FIND_RECURSIVE_ON_INFO_0_EO NULL
#define PRBOOM_RUMBLE_LABEL_EO NULL
#define PRBOOM_RUMBLE_INFO_0_EO NULL
#define PRBOOM_ANALOG_DEADZONE_LABEL_EO NULL
#define PRBOOM_ANALOG_DEADZONE_INFO_0_EO NULL
#define PRBOOM_PURGE_LIMIT_LABEL_EO NULL
#define PRBOOM_PURGE_LIMIT_INFO_0_EO NULL
#define OPTION_VAL_8_EO NULL
#define OPTION_VAL_12_EO NULL
#define OPTION_VAL_16_EO NULL
#define OPTION_VAL_24_EO NULL
#define OPTION_VAL_32_EO NULL
#define OPTION_VAL_48_EO NULL
#define OPTION_VAL_64_EO NULL
#define OPTION_VAL_128_EO NULL
#define OPTION_VAL_256_EO NULL

struct retro_core_option_v2_category option_cats_eo[] = {
   { NULL, NULL, NULL },
};
struct retro_core_option_v2_definition option_defs_eo[] = {
   {
      "prboom-resolution",
      PRBOOM_RESOLUTION_LABEL_EO,
      NULL,
      PRBOOM_RESOLUTION_INFO_0_EO,
      NULL,
      NULL,
      {
         { "320x200",   OPTION_VAL_320X200_EO },
         { "640x400",   OPTION_VAL_640X400_EO },
         { "960x600",   OPTION_VAL_960X600_EO },
         { "1280x800",  OPTION_VAL_1280X800_EO },
         { "1600x1000", OPTION_VAL_1600X1000_EO },
         { "1920x1200", OPTION_VAL_1920X1200_EO },
         { "2240x1400", OPTION_VAL_2240X1400_EO },
         { "2560x1600", OPTION_VAL_2560X1600_EO },
         { NULL, NULL },
      },
      "320x200"
   },
   {
      "prboom-mouse_on",
      PRBOOM_MOUSE_ON_LABEL_EO,
      NULL,
      PRBOOM_MOUSE_ON_INFO_0_EO,
      NULL,
      NULL,
      {
         { "disabled", NULL },
         { "enabled",  NULL },
         { NULL, NULL },
      },
      "disabled"
   },
   {
      "prboom-find_recursive_on",
      PRBOOM_FIND_RECURSIVE_ON_LABEL_EO,
      NULL,
      PRBOOM_FIND_RECURSIVE_ON_INFO_0_EO,
      NULL,
      NULL,
      {
         { "disabled", NULL },
         { "enabled",  NULL },
         { NULL, NULL },
      },
      "enabled"
   },
   {
      "prboom-rumble",
      PRBOOM_RUMBLE_LABEL_EO,
      NULL,
      PRBOOM_RUMBLE_INFO_0_EO,
      NULL,
      NULL,
      {
         { "disabled", NULL },
         { "enabled",  NULL },
         { NULL, NULL },
      },
      "disabled"
   },
   {
      "prboom-analog_deadzone",
      PRBOOM_ANALOG_DEADZONE_LABEL_EO,
      NULL,
      PRBOOM_ANALOG_DEADZONE_INFO_0_EO,
      NULL,
      NULL,
      {
         { "0",  NULL },
         { "5",  NULL },
         { "10", NULL },
         { "15", NULL },
         { "20", NULL },
         { "25", NULL },
         { "30", NULL },
         { NULL, NULL },
      },
      "15"
   },
#if defined(MEMORY_LOW)
   {
      "prboom-purge_limit",
      PRBOOM_PURGE_LIMIT_LABEL_EO,
      NULL,
      PRBOOM_PURGE_LIMIT_INFO_0_EO,
      NULL,
      NULL,
      {
         { "8",   OPTION_VAL_8_EO },
         { "12",  OPTION_VAL_12_EO },
         { "16",  OPTION_VAL_16_EO },
         { "24",  OPTION_VAL_24_EO },
         { "32",  OPTION_VAL_32_EO },
         { "48",  OPTION_VAL_48_EO },
         { "64",  OPTION_VAL_64_EO },
         { "128", OPTION_VAL_128_EO },
         { "256", OPTION_VAL_256_EO },
         { NULL, NULL },
      },
      "16"
   },
#endif
   { NULL, NULL, NULL, NULL, NULL, NULL, {{0}}, NULL },
};
struct retro_core_options_v2 options_eo = {
   option_cats_eo,
   option_defs_eo
};

/* RETRO_LANGUAGE_ES */

#define PRBOOM_RESOLUTION_LABEL_ES "Resolución interna (es necesario reiniciar)"
#define PRBOOM_RESOLUTION_INFO_0_ES "Cambia la resolución."
#define OPTION_VAL_320X200_ES "320 × 200"
#define OPTION_VAL_640X400_ES "640 × 400"
#define OPTION_VAL_960X600_ES "960 × 600"
#define OPTION_VAL_1280X800_ES "1280 × 800"
#define OPTION_VAL_1600X1000_ES "1600 × 1000"
#define OPTION_VAL_1920X1200_ES "1920 × 1200"
#define OPTION_VAL_2240X1400_ES "2240 × 1400"
#define OPTION_VAL_2560X1600_ES "2560 × 1600"
#define PRBOOM_MOUSE_ON_LABEL_ES "Mantener el ratón activo al usar un mando"
#define PRBOOM_MOUSE_ON_INFO_0_ES "Permite usar los controles del ratón aunque el dispositivo de entrada del usuario 1 no esté configurado como «RetroKeyboard/Ratón»."
#define PRBOOM_FIND_RECURSIVE_ON_LABEL_ES "Buscar archivos IWAD en las carpetas principales"
#define PRBOOM_FIND_RECURSIVE_ON_INFO_0_ES "Busca archivos IWAD dentro de las carpetas principales. NOTA: Es necesario desactivar esta opción para poder ejecutar SIGIL."
#define PRBOOM_RUMBLE_LABEL_ES "Vibración"
#define PRBOOM_RUMBLE_INFO_0_ES "Activa la respuesta háptica al utilizar un mando compatible con vibración."
#define PRBOOM_ANALOG_DEADZONE_LABEL_ES "Zona muerta de los analógicos (%)"
#define PRBOOM_ANALOG_DEADZONE_INFO_0_ES "Establece la zona muerta de los sticks analógicos del mando en caso de seleccionar como dispositivo de entrada la opción «Mando moderno»."
#define PRBOOM_PURGE_LIMIT_LABEL_ES "Tamaño de la caché"
#define PRBOOM_PURGE_LIMIT_INFO_0_ES "Establece un límite al tamaño de la memoria asignada para almacenar los recursos del juego. Un valor muy bajo puede provocar tirones al jugar en mapas de grandes dimensiones."
#define OPTION_VAL_8_ES NULL
#define OPTION_VAL_12_ES NULL
#define OPTION_VAL_16_ES NULL
#define OPTION_VAL_24_ES NULL
#define OPTION_VAL_32_ES NULL
#define OPTION_VAL_48_ES NULL
#define OPTION_VAL_64_ES NULL
#define OPTION_VAL_128_ES NULL
#define OPTION_VAL_256_ES NULL

struct retro_core_option_v2_category option_cats_es[] = {
   { NULL, NULL, NULL },
};
struct retro_core_option_v2_definition option_defs_es[] = {
   {
      "prboom-resolution",
      PRBOOM_RESOLUTION_LABEL_ES,
      NULL,
      PRBOOM_RESOLUTION_INFO_0_ES,
      NULL,
      NULL,
      {
         { "320x200",   OPTION_VAL_320X200_ES },
         { "640x400",   OPTION_VAL_640X400_ES },
         { "960x600",   OPTION_VAL_960X600_ES },
         { "1280x800",  OPTION_VAL_1280X800_ES },
         { "1600x1000", OPTION_VAL_1600X1000_ES },
         { "1920x1200", OPTION_VAL_1920X1200_ES },
         { "2240x1400", OPTION_VAL_2240X1400_ES },
         { "2560x1600", OPTION_VAL_2560X1600_ES },
         { NULL, NULL },
      },
      "320x200"
   },
   {
      "prboom-mouse_on",
      PRBOOM_MOUSE_ON_LABEL_ES,
      NULL,
      PRBOOM_MOUSE_ON_INFO_0_ES,
      NULL,
      NULL,
      {
         { "disabled", NULL },
         { "enabled",  NULL },
         { NULL, NULL },
      },
      "disabled"
   },
   {
      "prboom-find_recursive_on",
      PRBOOM_FIND_RECURSIVE_ON_LABEL_ES,
      NULL,
      PRBOOM_FIND_RECURSIVE_ON_INFO_0_ES,
      NULL,
      NULL,
      {
         { "disabled", NULL },
         { "enabled",  NULL },
         { NULL, NULL },
      },
      "enabled"
   },
   {
      "prboom-rumble",
      PRBOOM_RUMBLE_LABEL_ES,
      NULL,
      PRBOOM_RUMBLE_INFO_0_ES,
      NULL,
      NULL,
      {
         { "disabled", NULL },
         { "enabled",  NULL },
         { NULL, NULL },
      },
      "disabled"
   },
   {
      "prboom-analog_deadzone",
      PRBOOM_ANALOG_DEADZONE_LABEL_ES,
      NULL,
      PRBOOM_ANALOG_DEADZONE_INFO_0_ES,
      NULL,
      NULL,
      {
         { "0",  NULL },
         { "5",  NULL },
         { "10", NULL },
         { "15", NULL },
         { "20", NULL },
         { "25", NULL },
         { "30", NULL },
         { NULL, NULL },
      },
      "15"
   },
#if defined(MEMORY_LOW)
   {
      "prboom-purge_limit",
      PRBOOM_PURGE_LIMIT_LABEL_ES,
      NULL,
      PRBOOM_PURGE_LIMIT_INFO_0_ES,
      NULL,
      NULL,
      {
         { "8",   OPTION_VAL_8_ES },
         { "12",  OPTION_VAL_12_ES },
         { "16",  OPTION_VAL_16_ES },
         { "24",  OPTION_VAL_24_ES },
         { "32",  OPTION_VAL_32_ES },
         { "48",  OPTION_VAL_48_ES },
         { "64",  OPTION_VAL_64_ES },
         { "128", OPTION_VAL_128_ES },
         { "256", OPTION_VAL_256_ES },
         { NULL, NULL },
      },
      "16"
   },
#endif
   { NULL, NULL, NULL, NULL, NULL, NULL, {{0}}, NULL },
};
struct retro_core_options_v2 options_es = {
   option_cats_es,
   option_defs_es
};

/* RETRO_LANGUAGE_FA */

#define PRBOOM_RESOLUTION_LABEL_FA NULL
#define PRBOOM_RESOLUTION_INFO_0_FA NULL
#define OPTION_VAL_320X200_FA NULL
#define OPTION_VAL_640X400_FA NULL
#define OPTION_VAL_960X600_FA NULL
#define OPTION_VAL_1280X800_FA NULL
#define OPTION_VAL_1600X1000_FA NULL
#define OPTION_VAL_1920X1200_FA NULL
#define OPTION_VAL_2240X1400_FA NULL
#define OPTION_VAL_2560X1600_FA NULL
#define PRBOOM_MOUSE_ON_LABEL_FA NULL
#define PRBOOM_MOUSE_ON_INFO_0_FA NULL
#define PRBOOM_FIND_RECURSIVE_ON_LABEL_FA NULL
#define PRBOOM_FIND_RECURSIVE_ON_INFO_0_FA NULL
#define PRBOOM_RUMBLE_LABEL_FA NULL
#define PRBOOM_RUMBLE_INFO_0_FA NULL
#define PRBOOM_ANALOG_DEADZONE_LABEL_FA NULL
#define PRBOOM_ANALOG_DEADZONE_INFO_0_FA NULL
#define PRBOOM_PURGE_LIMIT_LABEL_FA NULL
#define PRBOOM_PURGE_LIMIT_INFO_0_FA NULL
#define OPTION_VAL_8_FA NULL
#define OPTION_VAL_12_FA NULL
#define OPTION_VAL_16_FA NULL
#define OPTION_VAL_24_FA NULL
#define OPTION_VAL_32_FA NULL
#define OPTION_VAL_48_FA NULL
#define OPTION_VAL_64_FA NULL
#define OPTION_VAL_128_FA NULL
#define OPTION_VAL_256_FA NULL

struct retro_core_option_v2_category option_cats_fa[] = {
   { NULL, NULL, NULL },
};
struct retro_core_option_v2_definition option_defs_fa[] = {
   {
      "prboom-resolution",
      PRBOOM_RESOLUTION_LABEL_FA,
      NULL,
      PRBOOM_RESOLUTION_INFO_0_FA,
      NULL,
      NULL,
      {
         { "320x200",   OPTION_VAL_320X200_FA },
         { "640x400",   OPTION_VAL_640X400_FA },
         { "960x600",   OPTION_VAL_960X600_FA },
         { "1280x800",  OPTION_VAL_1280X800_FA },
         { "1600x1000", OPTION_VAL_1600X1000_FA },
         { "1920x1200", OPTION_VAL_1920X1200_FA },
         { "2240x1400", OPTION_VAL_2240X1400_FA },
         { "2560x1600", OPTION_VAL_2560X1600_FA },
         { NULL, NULL },
      },
      "320x200"
   },
   {
      "prboom-mouse_on",
      PRBOOM_MOUSE_ON_LABEL_FA,
      NULL,
      PRBOOM_MOUSE_ON_INFO_0_FA,
      NULL,
      NULL,
      {
         { "disabled", NULL },
         { "enabled",  NULL },
         { NULL, NULL },
      },
      "disabled"
   },
   {
      "prboom-find_recursive_on",
      PRBOOM_FIND_RECURSIVE_ON_LABEL_FA,
      NULL,
      PRBOOM_FIND_RECURSIVE_ON_INFO_0_FA,
      NULL,
      NULL,
      {
         { "disabled", NULL },
         { "enabled",  NULL },
         { NULL, NULL },
      },
      "enabled"
   },
   {
      "prboom-rumble",
      PRBOOM_RUMBLE_LABEL_FA,
      NULL,
      PRBOOM_RUMBLE_INFO_0_FA,
      NULL,
      NULL,
      {
         { "disabled", NULL },
         { "enabled",  NULL },
         { NULL, NULL },
      },
      "disabled"
   },
   {
      "prboom-analog_deadzone",
      PRBOOM_ANALOG_DEADZONE_LABEL_FA,
      NULL,
      PRBOOM_ANALOG_DEADZONE_INFO_0_FA,
      NULL,
      NULL,
      {
         { "0",  NULL },
         { "5",  NULL },
         { "10", NULL },
         { "15", NULL },
         { "20", NULL },
         { "25", NULL },
         { "30", NULL },
         { NULL, NULL },
      },
      "15"
   },
#if defined(MEMORY_LOW)
   {
      "prboom-purge_limit",
      PRBOOM_PURGE_LIMIT_LABEL_FA,
      NULL,
      PRBOOM_PURGE_LIMIT_INFO_0_FA,
      NULL,
      NULL,
      {
         { "8",   OPTION_VAL_8_FA },
         { "12",  OPTION_VAL_12_FA },
         { "16",  OPTION_VAL_16_FA },
         { "24",  OPTION_VAL_24_FA },
         { "32",  OPTION_VAL_32_FA },
         { "48",  OPTION_VAL_48_FA },
         { "64",  OPTION_VAL_64_FA },
         { "128", OPTION_VAL_128_FA },
         { "256", OPTION_VAL_256_FA },
         { NULL, NULL },
      },
      "16"
   },
#endif
   { NULL, NULL, NULL, NULL, NULL, NULL, {{0}}, NULL },
};
struct retro_core_options_v2 options_fa = {
   option_cats_fa,
   option_defs_fa
};

/* RETRO_LANGUAGE_FI */

#define PRBOOM_RESOLUTION_LABEL_FI "Sisäinen resoluutio (Uudelleenkäynnistys vaaditaan)"
#define PRBOOM_RESOLUTION_INFO_0_FI "Määritä resoluutio."
#define OPTION_VAL_320X200_FI NULL
#define OPTION_VAL_640X400_FI NULL
#define OPTION_VAL_960X600_FI NULL
#define OPTION_VAL_1280X800_FI NULL
#define OPTION_VAL_1600X1000_FI NULL
#define OPTION_VAL_1920X1200_FI NULL
#define OPTION_VAL_2240X1400_FI NULL
#define OPTION_VAL_2560X1600_FI NULL
#define PRBOOM_MOUSE_ON_LABEL_FI "Hiiri aktiivisena kun käytetään ohjainta"
#define PRBOOM_MOUSE_ON_INFO_0_FI "Sallii hiiren syötteet silloinkin, kun käyttäjän 1 laitetyyppi ei ole asetettuna \"RetroKeyboard/Mouse\"."
#define PRBOOM_FIND_RECURSIVE_ON_LABEL_FI "Etsi IWAD-tiedostoja ylemmän tason kansioista"
#define PRBOOM_FIND_RECURSIVE_ON_INFO_0_FI "Skannaa IWAD-tiedostoja ylemmän tason kansioista. HUOM: Tämä täytyy poistaa käytöstä, jos haluat ajaa SIGIL."
#define PRBOOM_RUMBLE_LABEL_FI "Tärinä tehosteet"
#define PRBOOM_RUMBLE_INFO_0_FI "Mahdollistaa haptisen palautteen, kun käytetään tärinällä varustettua peliohjainta."
#define PRBOOM_ANALOG_DEADZONE_LABEL_FI "Analoginen katvealue (Prosentteina)"
#define PRBOOM_ANALOG_DEADZONE_INFO_0_FI "Asettaa peliohjaimen analogisen katvealueen kun syöttölaitteen tyypiksi on asetettu \"Gamepad Modern\"."
#define PRBOOM_PURGE_LIMIT_LABEL_FI "Välimuistin koko"
#define PRBOOM_PURGE_LIMIT_INFO_0_FI "Asettaa rajan pelin komponenttien varastoinnille välimuistiin. Pienet arvot voivat aiheuttaa nykimistä, kun navigoidaan suuria kenttiä."
#define OPTION_VAL_8_FI "8 Mt"
#define OPTION_VAL_12_FI "12 Mt"
#define OPTION_VAL_16_FI "16 Mt"
#define OPTION_VAL_24_FI "24 Mt"
#define OPTION_VAL_32_FI "32 Mt"
#define OPTION_VAL_48_FI "48 Mt"
#define OPTION_VAL_64_FI "64 Mt"
#define OPTION_VAL_128_FI "128 Mt"
#define OPTION_VAL_256_FI "256 Mt"

struct retro_core_option_v2_category option_cats_fi[] = {
   { NULL, NULL, NULL },
};
struct retro_core_option_v2_definition option_defs_fi[] = {
   {
      "prboom-resolution",
      PRBOOM_RESOLUTION_LABEL_FI,
      NULL,
      PRBOOM_RESOLUTION_INFO_0_FI,
      NULL,
      NULL,
      {
         { "320x200",   OPTION_VAL_320X200_FI },
         { "640x400",   OPTION_VAL_640X400_FI },
         { "960x600",   OPTION_VAL_960X600_FI },
         { "1280x800",  OPTION_VAL_1280X800_FI },
         { "1600x1000", OPTION_VAL_1600X1000_FI },
         { "1920x1200", OPTION_VAL_1920X1200_FI },
         { "2240x1400", OPTION_VAL_2240X1400_FI },
         { "2560x1600", OPTION_VAL_2560X1600_FI },
         { NULL, NULL },
      },
      "320x200"
   },
   {
      "prboom-mouse_on",
      PRBOOM_MOUSE_ON_LABEL_FI,
      NULL,
      PRBOOM_MOUSE_ON_INFO_0_FI,
      NULL,
      NULL,
      {
         { "disabled", NULL },
         { "enabled",  NULL },
         { NULL, NULL },
      },
      "disabled"
   },
   {
      "prboom-find_recursive_on",
      PRBOOM_FIND_RECURSIVE_ON_LABEL_FI,
      NULL,
      PRBOOM_FIND_RECURSIVE_ON_INFO_0_FI,
      NULL,
      NULL,
      {
         { "disabled", NULL },
         { "enabled",  NULL },
         { NULL, NULL },
      },
      "enabled"
   },
   {
      "prboom-rumble",
      PRBOOM_RUMBLE_LABEL_FI,
      NULL,
      PRBOOM_RUMBLE_INFO_0_FI,
      NULL,
      NULL,
      {
         { "disabled", NULL },
         { "enabled",  NULL },
         { NULL, NULL },
      },
      "disabled"
   },
   {
      "prboom-analog_deadzone",
      PRBOOM_ANALOG_DEADZONE_LABEL_FI,
      NULL,
      PRBOOM_ANALOG_DEADZONE_INFO_0_FI,
      NULL,
      NULL,
      {
         { "0",  NULL },
         { "5",  NULL },
         { "10", NULL },
         { "15", NULL },
         { "20", NULL },
         { "25", NULL },
         { "30", NULL },
         { NULL, NULL },
      },
      "15"
   },
#if defined(MEMORY_LOW)
   {
      "prboom-purge_limit",
      PRBOOM_PURGE_LIMIT_LABEL_FI,
      NULL,
      PRBOOM_PURGE_LIMIT_INFO_0_FI,
      NULL,
      NULL,
      {
         { "8",   OPTION_VAL_8_FI },
         { "12",  OPTION_VAL_12_FI },
         { "16",  OPTION_VAL_16_FI },
         { "24",  OPTION_VAL_24_FI },
         { "32",  OPTION_VAL_32_FI },
         { "48",  OPTION_VAL_48_FI },
         { "64",  OPTION_VAL_64_FI },
         { "128", OPTION_VAL_128_FI },
         { "256", OPTION_VAL_256_FI },
         { NULL, NULL },
      },
      "16"
   },
#endif
   { NULL, NULL, NULL, NULL, NULL, NULL, {{0}}, NULL },
};
struct retro_core_options_v2 options_fi = {
   option_cats_fi,
   option_defs_fi
};

/* RETRO_LANGUAGE_FR */

#define PRBOOM_RESOLUTION_LABEL_FR "Résolution interne (Redémarrage requis)"
#define PRBOOM_RESOLUTION_INFO_0_FR "Configurer la résolution."
#define OPTION_VAL_320X200_FR NULL
#define OPTION_VAL_640X400_FR NULL
#define OPTION_VAL_960X600_FR NULL
#define OPTION_VAL_1280X800_FR NULL
#define OPTION_VAL_1600X1000_FR NULL
#define OPTION_VAL_1920X1200_FR NULL
#define OPTION_VAL_2240X1400_FR NULL
#define OPTION_VAL_2560X1600_FR NULL
#define PRBOOM_MOUSE_ON_LABEL_FR "Souris active lors de l'utilisation d'une manette"
#define PRBOOM_MOUSE_ON_INFO_0_FR "Permet d'utiliser des entrées de souris même lorsque le type de périphérique de l'Utilisateur 1 n'est pas réglé sur 'RetroClavier/Souris'."
#define PRBOOM_FIND_RECURSIVE_ON_LABEL_FR "Rechercher dans les dossiers parents pour les IWADs"
#define PRBOOM_FIND_RECURSIVE_ON_INFO_0_FR "Analyse les dossiers parents pour les IWADs. REMARQUE : Vous devez désactiver cette option si vous voulez lancer SIGIL."
#define PRBOOM_RUMBLE_LABEL_FR "Effets de vibration"
#define PRBOOM_RUMBLE_INFO_0_FR "Active le retour haptique lorsque vous utilisez une manette de jeu équipée pour la vibration."
#define PRBOOM_ANALOG_DEADZONE_LABEL_FR "Deadzone analogique (%)"
#define PRBOOM_ANALOG_DEADZONE_INFO_0_FR "Définit la zone morte des sticks analogiques de la manette lorsque le type de périphérique d’entrée est réglé sur 'Gamepad Modern'."
#define PRBOOM_PURGE_LIMIT_LABEL_FR "Taille du cache"
#define PRBOOM_PURGE_LIMIT_INFO_0_FR "Définit une limite sur la taille du pool de mémoire utilisé pour mettre en cache les ressources de jeu. De petites valeurs peuvent causer des saccades lors de la navigation dans de grandes cartes."
#define OPTION_VAL_8_FR "8 Mo"
#define OPTION_VAL_12_FR "12 Mo"
#define OPTION_VAL_16_FR "16 Mo"
#define OPTION_VAL_24_FR "24 Mo"
#define OPTION_VAL_32_FR "32 Mo"
#define OPTION_VAL_48_FR "48 Mo"
#define OPTION_VAL_64_FR "64 Mo"
#define OPTION_VAL_128_FR "128 Mo"
#define OPTION_VAL_256_FR "256 Mo"

struct retro_core_option_v2_category option_cats_fr[] = {
   { NULL, NULL, NULL },
};
struct retro_core_option_v2_definition option_defs_fr[] = {
   {
      "prboom-resolution",
      PRBOOM_RESOLUTION_LABEL_FR,
      NULL,
      PRBOOM_RESOLUTION_INFO_0_FR,
      NULL,
      NULL,
      {
         { "320x200",   OPTION_VAL_320X200_FR },
         { "640x400",   OPTION_VAL_640X400_FR },
         { "960x600",   OPTION_VAL_960X600_FR },
         { "1280x800",  OPTION_VAL_1280X800_FR },
         { "1600x1000", OPTION_VAL_1600X1000_FR },
         { "1920x1200", OPTION_VAL_1920X1200_FR },
         { "2240x1400", OPTION_VAL_2240X1400_FR },
         { "2560x1600", OPTION_VAL_2560X1600_FR },
         { NULL, NULL },
      },
      "320x200"
   },
   {
      "prboom-mouse_on",
      PRBOOM_MOUSE_ON_LABEL_FR,
      NULL,
      PRBOOM_MOUSE_ON_INFO_0_FR,
      NULL,
      NULL,
      {
         { "disabled", NULL },
         { "enabled",  NULL },
         { NULL, NULL },
      },
      "disabled"
   },
   {
      "prboom-find_recursive_on",
      PRBOOM_FIND_RECURSIVE_ON_LABEL_FR,
      NULL,
      PRBOOM_FIND_RECURSIVE_ON_INFO_0_FR,
      NULL,
      NULL,
      {
         { "disabled", NULL },
         { "enabled",  NULL },
         { NULL, NULL },
      },
      "enabled"
   },
   {
      "prboom-rumble",
      PRBOOM_RUMBLE_LABEL_FR,
      NULL,
      PRBOOM_RUMBLE_INFO_0_FR,
      NULL,
      NULL,
      {
         { "disabled", NULL },
         { "enabled",  NULL },
         { NULL, NULL },
      },
      "disabled"
   },
   {
      "prboom-analog_deadzone",
      PRBOOM_ANALOG_DEADZONE_LABEL_FR,
      NULL,
      PRBOOM_ANALOG_DEADZONE_INFO_0_FR,
      NULL,
      NULL,
      {
         { "0",  NULL },
         { "5",  NULL },
         { "10", NULL },
         { "15", NULL },
         { "20", NULL },
         { "25", NULL },
         { "30", NULL },
         { NULL, NULL },
      },
      "15"
   },
#if defined(MEMORY_LOW)
   {
      "prboom-purge_limit",
      PRBOOM_PURGE_LIMIT_LABEL_FR,
      NULL,
      PRBOOM_PURGE_LIMIT_INFO_0_FR,
      NULL,
      NULL,
      {
         { "8",   OPTION_VAL_8_FR },
         { "12",  OPTION_VAL_12_FR },
         { "16",  OPTION_VAL_16_FR },
         { "24",  OPTION_VAL_24_FR },
         { "32",  OPTION_VAL_32_FR },
         { "48",  OPTION_VAL_48_FR },
         { "64",  OPTION_VAL_64_FR },
         { "128", OPTION_VAL_128_FR },
         { "256", OPTION_VAL_256_FR },
         { NULL, NULL },
      },
      "16"
   },
#endif
   { NULL, NULL, NULL, NULL, NULL, NULL, {{0}}, NULL },
};
struct retro_core_options_v2 options_fr = {
   option_cats_fr,
   option_defs_fr
};

/* RETRO_LANGUAGE_GL */

#define PRBOOM_RESOLUTION_LABEL_GL NULL
#define PRBOOM_RESOLUTION_INFO_0_GL NULL
#define OPTION_VAL_320X200_GL NULL
#define OPTION_VAL_640X400_GL NULL
#define OPTION_VAL_960X600_GL NULL
#define OPTION_VAL_1280X800_GL NULL
#define OPTION_VAL_1600X1000_GL NULL
#define OPTION_VAL_1920X1200_GL NULL
#define OPTION_VAL_2240X1400_GL NULL
#define OPTION_VAL_2560X1600_GL NULL
#define PRBOOM_MOUSE_ON_LABEL_GL NULL
#define PRBOOM_MOUSE_ON_INFO_0_GL NULL
#define PRBOOM_FIND_RECURSIVE_ON_LABEL_GL NULL
#define PRBOOM_FIND_RECURSIVE_ON_INFO_0_GL NULL
#define PRBOOM_RUMBLE_LABEL_GL NULL
#define PRBOOM_RUMBLE_INFO_0_GL NULL
#define PRBOOM_ANALOG_DEADZONE_LABEL_GL NULL
#define PRBOOM_ANALOG_DEADZONE_INFO_0_GL NULL
#define PRBOOM_PURGE_LIMIT_LABEL_GL NULL
#define PRBOOM_PURGE_LIMIT_INFO_0_GL NULL
#define OPTION_VAL_8_GL NULL
#define OPTION_VAL_12_GL NULL
#define OPTION_VAL_16_GL NULL
#define OPTION_VAL_24_GL NULL
#define OPTION_VAL_32_GL NULL
#define OPTION_VAL_48_GL NULL
#define OPTION_VAL_64_GL NULL
#define OPTION_VAL_128_GL NULL
#define OPTION_VAL_256_GL NULL

struct retro_core_option_v2_category option_cats_gl[] = {
   { NULL, NULL, NULL },
};
struct retro_core_option_v2_definition option_defs_gl[] = {
   {
      "prboom-resolution",
      PRBOOM_RESOLUTION_LABEL_GL,
      NULL,
      PRBOOM_RESOLUTION_INFO_0_GL,
      NULL,
      NULL,
      {
         { "320x200",   OPTION_VAL_320X200_GL },
         { "640x400",   OPTION_VAL_640X400_GL },
         { "960x600",   OPTION_VAL_960X600_GL },
         { "1280x800",  OPTION_VAL_1280X800_GL },
         { "1600x1000", OPTION_VAL_1600X1000_GL },
         { "1920x1200", OPTION_VAL_1920X1200_GL },
         { "2240x1400", OPTION_VAL_2240X1400_GL },
         { "2560x1600", OPTION_VAL_2560X1600_GL },
         { NULL, NULL },
      },
      "320x200"
   },
   {
      "prboom-mouse_on",
      PRBOOM_MOUSE_ON_LABEL_GL,
      NULL,
      PRBOOM_MOUSE_ON_INFO_0_GL,
      NULL,
      NULL,
      {
         { "disabled", NULL },
         { "enabled",  NULL },
         { NULL, NULL },
      },
      "disabled"
   },
   {
      "prboom-find_recursive_on",
      PRBOOM_FIND_RECURSIVE_ON_LABEL_GL,
      NULL,
      PRBOOM_FIND_RECURSIVE_ON_INFO_0_GL,
      NULL,
      NULL,
      {
         { "disabled", NULL },
         { "enabled",  NULL },
         { NULL, NULL },
      },
      "enabled"
   },
   {
      "prboom-rumble",
      PRBOOM_RUMBLE_LABEL_GL,
      NULL,
      PRBOOM_RUMBLE_INFO_0_GL,
      NULL,
      NULL,
      {
         { "disabled", NULL },
         { "enabled",  NULL },
         { NULL, NULL },
      },
      "disabled"
   },
   {
      "prboom-analog_deadzone",
      PRBOOM_ANALOG_DEADZONE_LABEL_GL,
      NULL,
      PRBOOM_ANALOG_DEADZONE_INFO_0_GL,
      NULL,
      NULL,
      {
         { "0",  NULL },
         { "5",  NULL },
         { "10", NULL },
         { "15", NULL },
         { "20", NULL },
         { "25", NULL },
         { "30", NULL },
         { NULL, NULL },
      },
      "15"
   },
#if defined(MEMORY_LOW)
   {
      "prboom-purge_limit",
      PRBOOM_PURGE_LIMIT_LABEL_GL,
      NULL,
      PRBOOM_PURGE_LIMIT_INFO_0_GL,
      NULL,
      NULL,
      {
         { "8",   OPTION_VAL_8_GL },
         { "12",  OPTION_VAL_12_GL },
         { "16",  OPTION_VAL_16_GL },
         { "24",  OPTION_VAL_24_GL },
         { "32",  OPTION_VAL_32_GL },
         { "48",  OPTION_VAL_48_GL },
         { "64",  OPTION_VAL_64_GL },
         { "128", OPTION_VAL_128_GL },
         { "256", OPTION_VAL_256_GL },
         { NULL, NULL },
      },
      "16"
   },
#endif
   { NULL, NULL, NULL, NULL, NULL, NULL, {{0}}, NULL },
};
struct retro_core_options_v2 options_gl = {
   option_cats_gl,
   option_defs_gl
};

/* RETRO_LANGUAGE_HE */

#define PRBOOM_RESOLUTION_LABEL_HE NULL
#define PRBOOM_RESOLUTION_INFO_0_HE NULL
#define OPTION_VAL_320X200_HE NULL
#define OPTION_VAL_640X400_HE NULL
#define OPTION_VAL_960X600_HE NULL
#define OPTION_VAL_1280X800_HE NULL
#define OPTION_VAL_1600X1000_HE NULL
#define OPTION_VAL_1920X1200_HE NULL
#define OPTION_VAL_2240X1400_HE NULL
#define OPTION_VAL_2560X1600_HE NULL
#define PRBOOM_MOUSE_ON_LABEL_HE NULL
#define PRBOOM_MOUSE_ON_INFO_0_HE NULL
#define PRBOOM_FIND_RECURSIVE_ON_LABEL_HE NULL
#define PRBOOM_FIND_RECURSIVE_ON_INFO_0_HE NULL
#define PRBOOM_RUMBLE_LABEL_HE NULL
#define PRBOOM_RUMBLE_INFO_0_HE NULL
#define PRBOOM_ANALOG_DEADZONE_LABEL_HE NULL
#define PRBOOM_ANALOG_DEADZONE_INFO_0_HE NULL
#define PRBOOM_PURGE_LIMIT_LABEL_HE NULL
#define PRBOOM_PURGE_LIMIT_INFO_0_HE NULL
#define OPTION_VAL_8_HE NULL
#define OPTION_VAL_12_HE NULL
#define OPTION_VAL_16_HE NULL
#define OPTION_VAL_24_HE NULL
#define OPTION_VAL_32_HE NULL
#define OPTION_VAL_48_HE NULL
#define OPTION_VAL_64_HE NULL
#define OPTION_VAL_128_HE NULL
#define OPTION_VAL_256_HE NULL

struct retro_core_option_v2_category option_cats_he[] = {
   { NULL, NULL, NULL },
};
struct retro_core_option_v2_definition option_defs_he[] = {
   {
      "prboom-resolution",
      PRBOOM_RESOLUTION_LABEL_HE,
      NULL,
      PRBOOM_RESOLUTION_INFO_0_HE,
      NULL,
      NULL,
      {
         { "320x200",   OPTION_VAL_320X200_HE },
         { "640x400",   OPTION_VAL_640X400_HE },
         { "960x600",   OPTION_VAL_960X600_HE },
         { "1280x800",  OPTION_VAL_1280X800_HE },
         { "1600x1000", OPTION_VAL_1600X1000_HE },
         { "1920x1200", OPTION_VAL_1920X1200_HE },
         { "2240x1400", OPTION_VAL_2240X1400_HE },
         { "2560x1600", OPTION_VAL_2560X1600_HE },
         { NULL, NULL },
      },
      "320x200"
   },
   {
      "prboom-mouse_on",
      PRBOOM_MOUSE_ON_LABEL_HE,
      NULL,
      PRBOOM_MOUSE_ON_INFO_0_HE,
      NULL,
      NULL,
      {
         { "disabled", NULL },
         { "enabled",  NULL },
         { NULL, NULL },
      },
      "disabled"
   },
   {
      "prboom-find_recursive_on",
      PRBOOM_FIND_RECURSIVE_ON_LABEL_HE,
      NULL,
      PRBOOM_FIND_RECURSIVE_ON_INFO_0_HE,
      NULL,
      NULL,
      {
         { "disabled", NULL },
         { "enabled",  NULL },
         { NULL, NULL },
      },
      "enabled"
   },
   {
      "prboom-rumble",
      PRBOOM_RUMBLE_LABEL_HE,
      NULL,
      PRBOOM_RUMBLE_INFO_0_HE,
      NULL,
      NULL,
      {
         { "disabled", NULL },
         { "enabled",  NULL },
         { NULL, NULL },
      },
      "disabled"
   },
   {
      "prboom-analog_deadzone",
      PRBOOM_ANALOG_DEADZONE_LABEL_HE,
      NULL,
      PRBOOM_ANALOG_DEADZONE_INFO_0_HE,
      NULL,
      NULL,
      {
         { "0",  NULL },
         { "5",  NULL },
         { "10", NULL },
         { "15", NULL },
         { "20", NULL },
         { "25", NULL },
         { "30", NULL },
         { NULL, NULL },
      },
      "15"
   },
#if defined(MEMORY_LOW)
   {
      "prboom-purge_limit",
      PRBOOM_PURGE_LIMIT_LABEL_HE,
      NULL,
      PRBOOM_PURGE_LIMIT_INFO_0_HE,
      NULL,
      NULL,
      {
         { "8",   OPTION_VAL_8_HE },
         { "12",  OPTION_VAL_12_HE },
         { "16",  OPTION_VAL_16_HE },
         { "24",  OPTION_VAL_24_HE },
         { "32",  OPTION_VAL_32_HE },
         { "48",  OPTION_VAL_48_HE },
         { "64",  OPTION_VAL_64_HE },
         { "128", OPTION_VAL_128_HE },
         { "256", OPTION_VAL_256_HE },
         { NULL, NULL },
      },
      "16"
   },
#endif
   { NULL, NULL, NULL, NULL, NULL, NULL, {{0}}, NULL },
};
struct retro_core_options_v2 options_he = {
   option_cats_he,
   option_defs_he
};

/* RETRO_LANGUAGE_HU */

#define PRBOOM_RESOLUTION_LABEL_HU NULL
#define PRBOOM_RESOLUTION_INFO_0_HU NULL
#define OPTION_VAL_320X200_HU NULL
#define OPTION_VAL_640X400_HU NULL
#define OPTION_VAL_960X600_HU NULL
#define OPTION_VAL_1280X800_HU NULL
#define OPTION_VAL_1600X1000_HU NULL
#define OPTION_VAL_1920X1200_HU NULL
#define OPTION_VAL_2240X1400_HU NULL
#define OPTION_VAL_2560X1600_HU NULL
#define PRBOOM_MOUSE_ON_LABEL_HU NULL
#define PRBOOM_MOUSE_ON_INFO_0_HU NULL
#define PRBOOM_FIND_RECURSIVE_ON_LABEL_HU NULL
#define PRBOOM_FIND_RECURSIVE_ON_INFO_0_HU NULL
#define PRBOOM_RUMBLE_LABEL_HU NULL
#define PRBOOM_RUMBLE_INFO_0_HU NULL
#define PRBOOM_ANALOG_DEADZONE_LABEL_HU NULL
#define PRBOOM_ANALOG_DEADZONE_INFO_0_HU NULL
#define PRBOOM_PURGE_LIMIT_LABEL_HU NULL
#define PRBOOM_PURGE_LIMIT_INFO_0_HU NULL
#define OPTION_VAL_8_HU NULL
#define OPTION_VAL_12_HU NULL
#define OPTION_VAL_16_HU NULL
#define OPTION_VAL_24_HU NULL
#define OPTION_VAL_32_HU NULL
#define OPTION_VAL_48_HU NULL
#define OPTION_VAL_64_HU NULL
#define OPTION_VAL_128_HU NULL
#define OPTION_VAL_256_HU NULL

struct retro_core_option_v2_category option_cats_hu[] = {
   { NULL, NULL, NULL },
};
struct retro_core_option_v2_definition option_defs_hu[] = {
   {
      "prboom-resolution",
      PRBOOM_RESOLUTION_LABEL_HU,
      NULL,
      PRBOOM_RESOLUTION_INFO_0_HU,
      NULL,
      NULL,
      {
         { "320x200",   OPTION_VAL_320X200_HU },
         { "640x400",   OPTION_VAL_640X400_HU },
         { "960x600",   OPTION_VAL_960X600_HU },
         { "1280x800",  OPTION_VAL_1280X800_HU },
         { "1600x1000", OPTION_VAL_1600X1000_HU },
         { "1920x1200", OPTION_VAL_1920X1200_HU },
         { "2240x1400", OPTION_VAL_2240X1400_HU },
         { "2560x1600", OPTION_VAL_2560X1600_HU },
         { NULL, NULL },
      },
      "320x200"
   },
   {
      "prboom-mouse_on",
      PRBOOM_MOUSE_ON_LABEL_HU,
      NULL,
      PRBOOM_MOUSE_ON_INFO_0_HU,
      NULL,
      NULL,
      {
         { "disabled", NULL },
         { "enabled",  NULL },
         { NULL, NULL },
      },
      "disabled"
   },
   {
      "prboom-find_recursive_on",
      PRBOOM_FIND_RECURSIVE_ON_LABEL_HU,
      NULL,
      PRBOOM_FIND_RECURSIVE_ON_INFO_0_HU,
      NULL,
      NULL,
      {
         { "disabled", NULL },
         { "enabled",  NULL },
         { NULL, NULL },
      },
      "enabled"
   },
   {
      "prboom-rumble",
      PRBOOM_RUMBLE_LABEL_HU,
      NULL,
      PRBOOM_RUMBLE_INFO_0_HU,
      NULL,
      NULL,
      {
         { "disabled", NULL },
         { "enabled",  NULL },
         { NULL, NULL },
      },
      "disabled"
   },
   {
      "prboom-analog_deadzone",
      PRBOOM_ANALOG_DEADZONE_LABEL_HU,
      NULL,
      PRBOOM_ANALOG_DEADZONE_INFO_0_HU,
      NULL,
      NULL,
      {
         { "0",  NULL },
         { "5",  NULL },
         { "10", NULL },
         { "15", NULL },
         { "20", NULL },
         { "25", NULL },
         { "30", NULL },
         { NULL, NULL },
      },
      "15"
   },
#if defined(MEMORY_LOW)
   {
      "prboom-purge_limit",
      PRBOOM_PURGE_LIMIT_LABEL_HU,
      NULL,
      PRBOOM_PURGE_LIMIT_INFO_0_HU,
      NULL,
      NULL,
      {
         { "8",   OPTION_VAL_8_HU },
         { "12",  OPTION_VAL_12_HU },
         { "16",  OPTION_VAL_16_HU },
         { "24",  OPTION_VAL_24_HU },
         { "32",  OPTION_VAL_32_HU },
         { "48",  OPTION_VAL_48_HU },
         { "64",  OPTION_VAL_64_HU },
         { "128", OPTION_VAL_128_HU },
         { "256", OPTION_VAL_256_HU },
         { NULL, NULL },
      },
      "16"
   },
#endif
   { NULL, NULL, NULL, NULL, NULL, NULL, {{0}}, NULL },
};
struct retro_core_options_v2 options_hu = {
   option_cats_hu,
   option_defs_hu
};

/* RETRO_LANGUAGE_ID */

#define PRBOOM_RESOLUTION_LABEL_ID NULL
#define PRBOOM_RESOLUTION_INFO_0_ID NULL
#define OPTION_VAL_320X200_ID NULL
#define OPTION_VAL_640X400_ID NULL
#define OPTION_VAL_960X600_ID NULL
#define OPTION_VAL_1280X800_ID NULL
#define OPTION_VAL_1600X1000_ID NULL
#define OPTION_VAL_1920X1200_ID NULL
#define OPTION_VAL_2240X1400_ID NULL
#define OPTION_VAL_2560X1600_ID NULL
#define PRBOOM_MOUSE_ON_LABEL_ID NULL
#define PRBOOM_MOUSE_ON_INFO_0_ID NULL
#define PRBOOM_FIND_RECURSIVE_ON_LABEL_ID NULL
#define PRBOOM_FIND_RECURSIVE_ON_INFO_0_ID NULL
#define PRBOOM_RUMBLE_LABEL_ID NULL
#define PRBOOM_RUMBLE_INFO_0_ID NULL
#define PRBOOM_ANALOG_DEADZONE_LABEL_ID NULL
#define PRBOOM_ANALOG_DEADZONE_INFO_0_ID NULL
#define PRBOOM_PURGE_LIMIT_LABEL_ID NULL
#define PRBOOM_PURGE_LIMIT_INFO_0_ID NULL
#define OPTION_VAL_8_ID NULL
#define OPTION_VAL_12_ID NULL
#define OPTION_VAL_16_ID NULL
#define OPTION_VAL_24_ID NULL
#define OPTION_VAL_32_ID NULL
#define OPTION_VAL_48_ID NULL
#define OPTION_VAL_64_ID NULL
#define OPTION_VAL_128_ID NULL
#define OPTION_VAL_256_ID NULL

struct retro_core_option_v2_category option_cats_id[] = {
   { NULL, NULL, NULL },
};
struct retro_core_option_v2_definition option_defs_id[] = {
   {
      "prboom-resolution",
      PRBOOM_RESOLUTION_LABEL_ID,
      NULL,
      PRBOOM_RESOLUTION_INFO_0_ID,
      NULL,
      NULL,
      {
         { "320x200",   OPTION_VAL_320X200_ID },
         { "640x400",   OPTION_VAL_640X400_ID },
         { "960x600",   OPTION_VAL_960X600_ID },
         { "1280x800",  OPTION_VAL_1280X800_ID },
         { "1600x1000", OPTION_VAL_1600X1000_ID },
         { "1920x1200", OPTION_VAL_1920X1200_ID },
         { "2240x1400", OPTION_VAL_2240X1400_ID },
         { "2560x1600", OPTION_VAL_2560X1600_ID },
         { NULL, NULL },
      },
      "320x200"
   },
   {
      "prboom-mouse_on",
      PRBOOM_MOUSE_ON_LABEL_ID,
      NULL,
      PRBOOM_MOUSE_ON_INFO_0_ID,
      NULL,
      NULL,
      {
         { "disabled", NULL },
         { "enabled",  NULL },
         { NULL, NULL },
      },
      "disabled"
   },
   {
      "prboom-find_recursive_on",
      PRBOOM_FIND_RECURSIVE_ON_LABEL_ID,
      NULL,
      PRBOOM_FIND_RECURSIVE_ON_INFO_0_ID,
      NULL,
      NULL,
      {
         { "disabled", NULL },
         { "enabled",  NULL },
         { NULL, NULL },
      },
      "enabled"
   },
   {
      "prboom-rumble",
      PRBOOM_RUMBLE_LABEL_ID,
      NULL,
      PRBOOM_RUMBLE_INFO_0_ID,
      NULL,
      NULL,
      {
         { "disabled", NULL },
         { "enabled",  NULL },
         { NULL, NULL },
      },
      "disabled"
   },
   {
      "prboom-analog_deadzone",
      PRBOOM_ANALOG_DEADZONE_LABEL_ID,
      NULL,
      PRBOOM_ANALOG_DEADZONE_INFO_0_ID,
      NULL,
      NULL,
      {
         { "0",  NULL },
         { "5",  NULL },
         { "10", NULL },
         { "15", NULL },
         { "20", NULL },
         { "25", NULL },
         { "30", NULL },
         { NULL, NULL },
      },
      "15"
   },
#if defined(MEMORY_LOW)
   {
      "prboom-purge_limit",
      PRBOOM_PURGE_LIMIT_LABEL_ID,
      NULL,
      PRBOOM_PURGE_LIMIT_INFO_0_ID,
      NULL,
      NULL,
      {
         { "8",   OPTION_VAL_8_ID },
         { "12",  OPTION_VAL_12_ID },
         { "16",  OPTION_VAL_16_ID },
         { "24",  OPTION_VAL_24_ID },
         { "32",  OPTION_VAL_32_ID },
         { "48",  OPTION_VAL_48_ID },
         { "64",  OPTION_VAL_64_ID },
         { "128", OPTION_VAL_128_ID },
         { "256", OPTION_VAL_256_ID },
         { NULL, NULL },
      },
      "16"
   },
#endif
   { NULL, NULL, NULL, NULL, NULL, NULL, {{0}}, NULL },
};
struct retro_core_options_v2 options_id = {
   option_cats_id,
   option_defs_id
};

/* RETRO_LANGUAGE_IT */

#define PRBOOM_RESOLUTION_LABEL_IT "Risoluzione interna (Riavvio richiesto)"
#define PRBOOM_RESOLUTION_INFO_0_IT "Configura la risoluzione."
#define OPTION_VAL_320X200_IT NULL
#define OPTION_VAL_640X400_IT NULL
#define OPTION_VAL_960X600_IT NULL
#define OPTION_VAL_1280X800_IT NULL
#define OPTION_VAL_1600X1000_IT NULL
#define OPTION_VAL_1920X1200_IT NULL
#define OPTION_VAL_2240X1400_IT NULL
#define OPTION_VAL_2560X1600_IT NULL
#define PRBOOM_MOUSE_ON_LABEL_IT "Mouse Attivo Quando Si Usano Gamepad"
#define PRBOOM_MOUSE_ON_INFO_0_IT "Consente di utilizzare gli input del mouse anche quando il tipo di dispositivo dell'utente 1 non è impostato su 'RetroKeyboard/Mouse'."
#define PRBOOM_FIND_RECURSIVE_ON_LABEL_IT "Cerca nelle cartelle genitori per gli IWAD"
#define PRBOOM_FIND_RECURSIVE_ON_INFO_0_IT "Scansiona le cartelle genitore per IWADs. NOTA: È necessario disattivare questa opzione se si desidera eseguire SIGIL."
#define PRBOOM_RUMBLE_LABEL_IT "Effetti Di Vibrazione"
#define PRBOOM_RUMBLE_INFO_0_IT "Abilita il feedback tattile quando si utilizza un gamepad dotato di vibrazione."
#define PRBOOM_ANALOG_DEADZONE_LABEL_IT "Deadzone Analogica (Percentuale)"
#define PRBOOM_ANALOG_DEADZONE_INFO_0_IT "Imposta la zona morta dei bastoncini analogici del gamepad quando il tipo di dispositivo di input è impostato su 'Gamepad Modern'."
#define PRBOOM_PURGE_LIMIT_LABEL_IT "Dimensione Cache"
#define PRBOOM_PURGE_LIMIT_INFO_0_IT "Imposta un limite alla dimensione del pool di memoria utilizzato per la cache degli asset di gioco. Valori piccoli possono causare balbuzie durante la navigazione di mappe di grandi dimensioni."
#define OPTION_VAL_8_IT NULL
#define OPTION_VAL_12_IT NULL
#define OPTION_VAL_16_IT NULL
#define OPTION_VAL_24_IT NULL
#define OPTION_VAL_32_IT NULL
#define OPTION_VAL_48_IT NULL
#define OPTION_VAL_64_IT NULL
#define OPTION_VAL_128_IT NULL
#define OPTION_VAL_256_IT NULL

struct retro_core_option_v2_category option_cats_it[] = {
   { NULL, NULL, NULL },
};
struct retro_core_option_v2_definition option_defs_it[] = {
   {
      "prboom-resolution",
      PRBOOM_RESOLUTION_LABEL_IT,
      NULL,
      PRBOOM_RESOLUTION_INFO_0_IT,
      NULL,
      NULL,
      {
         { "320x200",   OPTION_VAL_320X200_IT },
         { "640x400",   OPTION_VAL_640X400_IT },
         { "960x600",   OPTION_VAL_960X600_IT },
         { "1280x800",  OPTION_VAL_1280X800_IT },
         { "1600x1000", OPTION_VAL_1600X1000_IT },
         { "1920x1200", OPTION_VAL_1920X1200_IT },
         { "2240x1400", OPTION_VAL_2240X1400_IT },
         { "2560x1600", OPTION_VAL_2560X1600_IT },
         { NULL, NULL },
      },
      "320x200"
   },
   {
      "prboom-mouse_on",
      PRBOOM_MOUSE_ON_LABEL_IT,
      NULL,
      PRBOOM_MOUSE_ON_INFO_0_IT,
      NULL,
      NULL,
      {
         { "disabled", NULL },
         { "enabled",  NULL },
         { NULL, NULL },
      },
      "disabled"
   },
   {
      "prboom-find_recursive_on",
      PRBOOM_FIND_RECURSIVE_ON_LABEL_IT,
      NULL,
      PRBOOM_FIND_RECURSIVE_ON_INFO_0_IT,
      NULL,
      NULL,
      {
         { "disabled", NULL },
         { "enabled",  NULL },
         { NULL, NULL },
      },
      "enabled"
   },
   {
      "prboom-rumble",
      PRBOOM_RUMBLE_LABEL_IT,
      NULL,
      PRBOOM_RUMBLE_INFO_0_IT,
      NULL,
      NULL,
      {
         { "disabled", NULL },
         { "enabled",  NULL },
         { NULL, NULL },
      },
      "disabled"
   },
   {
      "prboom-analog_deadzone",
      PRBOOM_ANALOG_DEADZONE_LABEL_IT,
      NULL,
      PRBOOM_ANALOG_DEADZONE_INFO_0_IT,
      NULL,
      NULL,
      {
         { "0",  NULL },
         { "5",  NULL },
         { "10", NULL },
         { "15", NULL },
         { "20", NULL },
         { "25", NULL },
         { "30", NULL },
         { NULL, NULL },
      },
      "15"
   },
#if defined(MEMORY_LOW)
   {
      "prboom-purge_limit",
      PRBOOM_PURGE_LIMIT_LABEL_IT,
      NULL,
      PRBOOM_PURGE_LIMIT_INFO_0_IT,
      NULL,
      NULL,
      {
         { "8",   OPTION_VAL_8_IT },
         { "12",  OPTION_VAL_12_IT },
         { "16",  OPTION_VAL_16_IT },
         { "24",  OPTION_VAL_24_IT },
         { "32",  OPTION_VAL_32_IT },
         { "48",  OPTION_VAL_48_IT },
         { "64",  OPTION_VAL_64_IT },
         { "128", OPTION_VAL_128_IT },
         { "256", OPTION_VAL_256_IT },
         { NULL, NULL },
      },
      "16"
   },
#endif
   { NULL, NULL, NULL, NULL, NULL, NULL, {{0}}, NULL },
};
struct retro_core_options_v2 options_it = {
   option_cats_it,
   option_defs_it
};

/* RETRO_LANGUAGE_JA */

#define PRBOOM_RESOLUTION_LABEL_JA NULL
#define PRBOOM_RESOLUTION_INFO_0_JA NULL
#define OPTION_VAL_320X200_JA NULL
#define OPTION_VAL_640X400_JA NULL
#define OPTION_VAL_960X600_JA NULL
#define OPTION_VAL_1280X800_JA NULL
#define OPTION_VAL_1600X1000_JA NULL
#define OPTION_VAL_1920X1200_JA NULL
#define OPTION_VAL_2240X1400_JA NULL
#define OPTION_VAL_2560X1600_JA NULL
#define PRBOOM_MOUSE_ON_LABEL_JA NULL
#define PRBOOM_MOUSE_ON_INFO_0_JA NULL
#define PRBOOM_FIND_RECURSIVE_ON_LABEL_JA NULL
#define PRBOOM_FIND_RECURSIVE_ON_INFO_0_JA NULL
#define PRBOOM_RUMBLE_LABEL_JA NULL
#define PRBOOM_RUMBLE_INFO_0_JA NULL
#define PRBOOM_ANALOG_DEADZONE_LABEL_JA NULL
#define PRBOOM_ANALOG_DEADZONE_INFO_0_JA NULL
#define PRBOOM_PURGE_LIMIT_LABEL_JA NULL
#define PRBOOM_PURGE_LIMIT_INFO_0_JA NULL
#define OPTION_VAL_8_JA NULL
#define OPTION_VAL_12_JA NULL
#define OPTION_VAL_16_JA NULL
#define OPTION_VAL_24_JA NULL
#define OPTION_VAL_32_JA NULL
#define OPTION_VAL_48_JA NULL
#define OPTION_VAL_64_JA NULL
#define OPTION_VAL_128_JA NULL
#define OPTION_VAL_256_JA NULL

struct retro_core_option_v2_category option_cats_ja[] = {
   { NULL, NULL, NULL },
};
struct retro_core_option_v2_definition option_defs_ja[] = {
   {
      "prboom-resolution",
      PRBOOM_RESOLUTION_LABEL_JA,
      NULL,
      PRBOOM_RESOLUTION_INFO_0_JA,
      NULL,
      NULL,
      {
         { "320x200",   OPTION_VAL_320X200_JA },
         { "640x400",   OPTION_VAL_640X400_JA },
         { "960x600",   OPTION_VAL_960X600_JA },
         { "1280x800",  OPTION_VAL_1280X800_JA },
         { "1600x1000", OPTION_VAL_1600X1000_JA },
         { "1920x1200", OPTION_VAL_1920X1200_JA },
         { "2240x1400", OPTION_VAL_2240X1400_JA },
         { "2560x1600", OPTION_VAL_2560X1600_JA },
         { NULL, NULL },
      },
      "320x200"
   },
   {
      "prboom-mouse_on",
      PRBOOM_MOUSE_ON_LABEL_JA,
      NULL,
      PRBOOM_MOUSE_ON_INFO_0_JA,
      NULL,
      NULL,
      {
         { "disabled", NULL },
         { "enabled",  NULL },
         { NULL, NULL },
      },
      "disabled"
   },
   {
      "prboom-find_recursive_on",
      PRBOOM_FIND_RECURSIVE_ON_LABEL_JA,
      NULL,
      PRBOOM_FIND_RECURSIVE_ON_INFO_0_JA,
      NULL,
      NULL,
      {
         { "disabled", NULL },
         { "enabled",  NULL },
         { NULL, NULL },
      },
      "enabled"
   },
   {
      "prboom-rumble",
      PRBOOM_RUMBLE_LABEL_JA,
      NULL,
      PRBOOM_RUMBLE_INFO_0_JA,
      NULL,
      NULL,
      {
         { "disabled", NULL },
         { "enabled",  NULL },
         { NULL, NULL },
      },
      "disabled"
   },
   {
      "prboom-analog_deadzone",
      PRBOOM_ANALOG_DEADZONE_LABEL_JA,
      NULL,
      PRBOOM_ANALOG_DEADZONE_INFO_0_JA,
      NULL,
      NULL,
      {
         { "0",  NULL },
         { "5",  NULL },
         { "10", NULL },
         { "15", NULL },
         { "20", NULL },
         { "25", NULL },
         { "30", NULL },
         { NULL, NULL },
      },
      "15"
   },
#if defined(MEMORY_LOW)
   {
      "prboom-purge_limit",
      PRBOOM_PURGE_LIMIT_LABEL_JA,
      NULL,
      PRBOOM_PURGE_LIMIT_INFO_0_JA,
      NULL,
      NULL,
      {
         { "8",   OPTION_VAL_8_JA },
         { "12",  OPTION_VAL_12_JA },
         { "16",  OPTION_VAL_16_JA },
         { "24",  OPTION_VAL_24_JA },
         { "32",  OPTION_VAL_32_JA },
         { "48",  OPTION_VAL_48_JA },
         { "64",  OPTION_VAL_64_JA },
         { "128", OPTION_VAL_128_JA },
         { "256", OPTION_VAL_256_JA },
         { NULL, NULL },
      },
      "16"
   },
#endif
   { NULL, NULL, NULL, NULL, NULL, NULL, {{0}}, NULL },
};
struct retro_core_options_v2 options_ja = {
   option_cats_ja,
   option_defs_ja
};

/* RETRO_LANGUAGE_KO */

#define PRBOOM_RESOLUTION_LABEL_KO "내부 해상도 (재시작 필요)"
#define PRBOOM_RESOLUTION_INFO_0_KO "해상도를 설정합니다."
#define OPTION_VAL_320X200_KO NULL
#define OPTION_VAL_640X400_KO NULL
#define OPTION_VAL_960X600_KO NULL
#define OPTION_VAL_1280X800_KO NULL
#define OPTION_VAL_1600X1000_KO NULL
#define OPTION_VAL_1920X1200_KO NULL
#define OPTION_VAL_2240X1400_KO NULL
#define OPTION_VAL_2560X1600_KO NULL
#define PRBOOM_MOUSE_ON_LABEL_KO "게임패드 사용 중 마우스 활성"
#define PRBOOM_MOUSE_ON_INFO_0_KO "사용자 1의 장치 종류가 'Retro키보드/마우스'가 아닐 경우에도 마우스 입력을 사용할 수 있게 합니다."
#define PRBOOM_FIND_RECURSIVE_ON_LABEL_KO "상위 폴더에서 IWAD 찾기"
#define PRBOOM_FIND_RECURSIVE_ON_INFO_0_KO "IWAD를 찾을 때 상위 폴더까지 검색합니다. 주의: SIGIL을 실행하려면 이 옵션을 비활성화해야합니다."
#define PRBOOM_RUMBLE_LABEL_KO "진동 효과"
#define PRBOOM_RUMBLE_INFO_0_KO "진동 효과를 지원하는 게임패드를 사용할 때 햅틱 피드백을 사용합니다."
#define PRBOOM_ANALOG_DEADZONE_LABEL_KO "아날로그 데드존 (퍼센트)"
#define PRBOOM_ANALOG_DEADZONE_INFO_0_KO "입력 장치가 'Gamepad Modern'일 때 사용할 아날로그 스틱의 데드존을 설정합니다."
#define PRBOOM_PURGE_LIMIT_LABEL_KO "캐시 크기"
#define PRBOOM_PURGE_LIMIT_INFO_0_KO "게임 애셋을 캐싱해둘 메모리 풀의 한계 크기를 지정합니다. 작은 값으로 설정할 경우 큰 맵을 탐색할 때 끊김이 발생할 수 있습니다."
#define OPTION_VAL_8_KO NULL
#define OPTION_VAL_12_KO NULL
#define OPTION_VAL_16_KO NULL
#define OPTION_VAL_24_KO NULL
#define OPTION_VAL_32_KO NULL
#define OPTION_VAL_48_KO NULL
#define OPTION_VAL_64_KO NULL
#define OPTION_VAL_128_KO NULL
#define OPTION_VAL_256_KO NULL

struct retro_core_option_v2_category option_cats_ko[] = {
   { NULL, NULL, NULL },
};
struct retro_core_option_v2_definition option_defs_ko[] = {
   {
      "prboom-resolution",
      PRBOOM_RESOLUTION_LABEL_KO,
      NULL,
      PRBOOM_RESOLUTION_INFO_0_KO,
      NULL,
      NULL,
      {
         { "320x200",   OPTION_VAL_320X200_KO },
         { "640x400",   OPTION_VAL_640X400_KO },
         { "960x600",   OPTION_VAL_960X600_KO },
         { "1280x800",  OPTION_VAL_1280X800_KO },
         { "1600x1000", OPTION_VAL_1600X1000_KO },
         { "1920x1200", OPTION_VAL_1920X1200_KO },
         { "2240x1400", OPTION_VAL_2240X1400_KO },
         { "2560x1600", OPTION_VAL_2560X1600_KO },
         { NULL, NULL },
      },
      "320x200"
   },
   {
      "prboom-mouse_on",
      PRBOOM_MOUSE_ON_LABEL_KO,
      NULL,
      PRBOOM_MOUSE_ON_INFO_0_KO,
      NULL,
      NULL,
      {
         { "disabled", NULL },
         { "enabled",  NULL },
         { NULL, NULL },
      },
      "disabled"
   },
   {
      "prboom-find_recursive_on",
      PRBOOM_FIND_RECURSIVE_ON_LABEL_KO,
      NULL,
      PRBOOM_FIND_RECURSIVE_ON_INFO_0_KO,
      NULL,
      NULL,
      {
         { "disabled", NULL },
         { "enabled",  NULL },
         { NULL, NULL },
      },
      "enabled"
   },
   {
      "prboom-rumble",
      PRBOOM_RUMBLE_LABEL_KO,
      NULL,
      PRBOOM_RUMBLE_INFO_0_KO,
      NULL,
      NULL,
      {
         { "disabled", NULL },
         { "enabled",  NULL },
         { NULL, NULL },
      },
      "disabled"
   },
   {
      "prboom-analog_deadzone",
      PRBOOM_ANALOG_DEADZONE_LABEL_KO,
      NULL,
      PRBOOM_ANALOG_DEADZONE_INFO_0_KO,
      NULL,
      NULL,
      {
         { "0",  NULL },
         { "5",  NULL },
         { "10", NULL },
         { "15", NULL },
         { "20", NULL },
         { "25", NULL },
         { "30", NULL },
         { NULL, NULL },
      },
      "15"
   },
#if defined(MEMORY_LOW)
   {
      "prboom-purge_limit",
      PRBOOM_PURGE_LIMIT_LABEL_KO,
      NULL,
      PRBOOM_PURGE_LIMIT_INFO_0_KO,
      NULL,
      NULL,
      {
         { "8",   OPTION_VAL_8_KO },
         { "12",  OPTION_VAL_12_KO },
         { "16",  OPTION_VAL_16_KO },
         { "24",  OPTION_VAL_24_KO },
         { "32",  OPTION_VAL_32_KO },
         { "48",  OPTION_VAL_48_KO },
         { "64",  OPTION_VAL_64_KO },
         { "128", OPTION_VAL_128_KO },
         { "256", OPTION_VAL_256_KO },
         { NULL, NULL },
      },
      "16"
   },
#endif
   { NULL, NULL, NULL, NULL, NULL, NULL, {{0}}, NULL },
};
struct retro_core_options_v2 options_ko = {
   option_cats_ko,
   option_defs_ko
};

/* RETRO_LANGUAGE_MT */

#define PRBOOM_RESOLUTION_LABEL_MT NULL
#define PRBOOM_RESOLUTION_INFO_0_MT NULL
#define OPTION_VAL_320X200_MT NULL
#define OPTION_VAL_640X400_MT NULL
#define OPTION_VAL_960X600_MT NULL
#define OPTION_VAL_1280X800_MT NULL
#define OPTION_VAL_1600X1000_MT NULL
#define OPTION_VAL_1920X1200_MT NULL
#define OPTION_VAL_2240X1400_MT NULL
#define OPTION_VAL_2560X1600_MT NULL
#define PRBOOM_MOUSE_ON_LABEL_MT NULL
#define PRBOOM_MOUSE_ON_INFO_0_MT NULL
#define PRBOOM_FIND_RECURSIVE_ON_LABEL_MT NULL
#define PRBOOM_FIND_RECURSIVE_ON_INFO_0_MT NULL
#define PRBOOM_RUMBLE_LABEL_MT NULL
#define PRBOOM_RUMBLE_INFO_0_MT NULL
#define PRBOOM_ANALOG_DEADZONE_LABEL_MT NULL
#define PRBOOM_ANALOG_DEADZONE_INFO_0_MT NULL
#define PRBOOM_PURGE_LIMIT_LABEL_MT NULL
#define PRBOOM_PURGE_LIMIT_INFO_0_MT NULL
#define OPTION_VAL_8_MT NULL
#define OPTION_VAL_12_MT NULL
#define OPTION_VAL_16_MT NULL
#define OPTION_VAL_24_MT NULL
#define OPTION_VAL_32_MT NULL
#define OPTION_VAL_48_MT NULL
#define OPTION_VAL_64_MT NULL
#define OPTION_VAL_128_MT NULL
#define OPTION_VAL_256_MT NULL

struct retro_core_option_v2_category option_cats_mt[] = {
   { NULL, NULL, NULL },
};
struct retro_core_option_v2_definition option_defs_mt[] = {
   {
      "prboom-resolution",
      PRBOOM_RESOLUTION_LABEL_MT,
      NULL,
      PRBOOM_RESOLUTION_INFO_0_MT,
      NULL,
      NULL,
      {
         { "320x200",   OPTION_VAL_320X200_MT },
         { "640x400",   OPTION_VAL_640X400_MT },
         { "960x600",   OPTION_VAL_960X600_MT },
         { "1280x800",  OPTION_VAL_1280X800_MT },
         { "1600x1000", OPTION_VAL_1600X1000_MT },
         { "1920x1200", OPTION_VAL_1920X1200_MT },
         { "2240x1400", OPTION_VAL_2240X1400_MT },
         { "2560x1600", OPTION_VAL_2560X1600_MT },
         { NULL, NULL },
      },
      "320x200"
   },
   {
      "prboom-mouse_on",
      PRBOOM_MOUSE_ON_LABEL_MT,
      NULL,
      PRBOOM_MOUSE_ON_INFO_0_MT,
      NULL,
      NULL,
      {
         { "disabled", NULL },
         { "enabled",  NULL },
         { NULL, NULL },
      },
      "disabled"
   },
   {
      "prboom-find_recursive_on",
      PRBOOM_FIND_RECURSIVE_ON_LABEL_MT,
      NULL,
      PRBOOM_FIND_RECURSIVE_ON_INFO_0_MT,
      NULL,
      NULL,
      {
         { "disabled", NULL },
         { "enabled",  NULL },
         { NULL, NULL },
      },
      "enabled"
   },
   {
      "prboom-rumble",
      PRBOOM_RUMBLE_LABEL_MT,
      NULL,
      PRBOOM_RUMBLE_INFO_0_MT,
      NULL,
      NULL,
      {
         { "disabled", NULL },
         { "enabled",  NULL },
         { NULL, NULL },
      },
      "disabled"
   },
   {
      "prboom-analog_deadzone",
      PRBOOM_ANALOG_DEADZONE_LABEL_MT,
      NULL,
      PRBOOM_ANALOG_DEADZONE_INFO_0_MT,
      NULL,
      NULL,
      {
         { "0",  NULL },
         { "5",  NULL },
         { "10", NULL },
         { "15", NULL },
         { "20", NULL },
         { "25", NULL },
         { "30", NULL },
         { NULL, NULL },
      },
      "15"
   },
#if defined(MEMORY_LOW)
   {
      "prboom-purge_limit",
      PRBOOM_PURGE_LIMIT_LABEL_MT,
      NULL,
      PRBOOM_PURGE_LIMIT_INFO_0_MT,
      NULL,
      NULL,
      {
         { "8",   OPTION_VAL_8_MT },
         { "12",  OPTION_VAL_12_MT },
         { "16",  OPTION_VAL_16_MT },
         { "24",  OPTION_VAL_24_MT },
         { "32",  OPTION_VAL_32_MT },
         { "48",  OPTION_VAL_48_MT },
         { "64",  OPTION_VAL_64_MT },
         { "128", OPTION_VAL_128_MT },
         { "256", OPTION_VAL_256_MT },
         { NULL, NULL },
      },
      "16"
   },
#endif
   { NULL, NULL, NULL, NULL, NULL, NULL, {{0}}, NULL },
};
struct retro_core_options_v2 options_mt = {
   option_cats_mt,
   option_defs_mt
};

/* RETRO_LANGUAGE_NL */

#define PRBOOM_RESOLUTION_LABEL_NL NULL
#define PRBOOM_RESOLUTION_INFO_0_NL NULL
#define OPTION_VAL_320X200_NL NULL
#define OPTION_VAL_640X400_NL NULL
#define OPTION_VAL_960X600_NL NULL
#define OPTION_VAL_1280X800_NL NULL
#define OPTION_VAL_1600X1000_NL NULL
#define OPTION_VAL_1920X1200_NL NULL
#define OPTION_VAL_2240X1400_NL NULL
#define OPTION_VAL_2560X1600_NL NULL
#define PRBOOM_MOUSE_ON_LABEL_NL NULL
#define PRBOOM_MOUSE_ON_INFO_0_NL NULL
#define PRBOOM_FIND_RECURSIVE_ON_LABEL_NL NULL
#define PRBOOM_FIND_RECURSIVE_ON_INFO_0_NL NULL
#define PRBOOM_RUMBLE_LABEL_NL NULL
#define PRBOOM_RUMBLE_INFO_0_NL NULL
#define PRBOOM_ANALOG_DEADZONE_LABEL_NL NULL
#define PRBOOM_ANALOG_DEADZONE_INFO_0_NL NULL
#define PRBOOM_PURGE_LIMIT_LABEL_NL NULL
#define PRBOOM_PURGE_LIMIT_INFO_0_NL NULL
#define OPTION_VAL_8_NL NULL
#define OPTION_VAL_12_NL NULL
#define OPTION_VAL_16_NL NULL
#define OPTION_VAL_24_NL NULL
#define OPTION_VAL_32_NL NULL
#define OPTION_VAL_48_NL NULL
#define OPTION_VAL_64_NL NULL
#define OPTION_VAL_128_NL NULL
#define OPTION_VAL_256_NL NULL

struct retro_core_option_v2_category option_cats_nl[] = {
   { NULL, NULL, NULL },
};
struct retro_core_option_v2_definition option_defs_nl[] = {
   {
      "prboom-resolution",
      PRBOOM_RESOLUTION_LABEL_NL,
      NULL,
      PRBOOM_RESOLUTION_INFO_0_NL,
      NULL,
      NULL,
      {
         { "320x200",   OPTION_VAL_320X200_NL },
         { "640x400",   OPTION_VAL_640X400_NL },
         { "960x600",   OPTION_VAL_960X600_NL },
         { "1280x800",  OPTION_VAL_1280X800_NL },
         { "1600x1000", OPTION_VAL_1600X1000_NL },
         { "1920x1200", OPTION_VAL_1920X1200_NL },
         { "2240x1400", OPTION_VAL_2240X1400_NL },
         { "2560x1600", OPTION_VAL_2560X1600_NL },
         { NULL, NULL },
      },
      "320x200"
   },
   {
      "prboom-mouse_on",
      PRBOOM_MOUSE_ON_LABEL_NL,
      NULL,
      PRBOOM_MOUSE_ON_INFO_0_NL,
      NULL,
      NULL,
      {
         { "disabled", NULL },
         { "enabled",  NULL },
         { NULL, NULL },
      },
      "disabled"
   },
   {
      "prboom-find_recursive_on",
      PRBOOM_FIND_RECURSIVE_ON_LABEL_NL,
      NULL,
      PRBOOM_FIND_RECURSIVE_ON_INFO_0_NL,
      NULL,
      NULL,
      {
         { "disabled", NULL },
         { "enabled",  NULL },
         { NULL, NULL },
      },
      "enabled"
   },
   {
      "prboom-rumble",
      PRBOOM_RUMBLE_LABEL_NL,
      NULL,
      PRBOOM_RUMBLE_INFO_0_NL,
      NULL,
      NULL,
      {
         { "disabled", NULL },
         { "enabled",  NULL },
         { NULL, NULL },
      },
      "disabled"
   },
   {
      "prboom-analog_deadzone",
      PRBOOM_ANALOG_DEADZONE_LABEL_NL,
      NULL,
      PRBOOM_ANALOG_DEADZONE_INFO_0_NL,
      NULL,
      NULL,
      {
         { "0",  NULL },
         { "5",  NULL },
         { "10", NULL },
         { "15", NULL },
         { "20", NULL },
         { "25", NULL },
         { "30", NULL },
         { NULL, NULL },
      },
      "15"
   },
#if defined(MEMORY_LOW)
   {
      "prboom-purge_limit",
      PRBOOM_PURGE_LIMIT_LABEL_NL,
      NULL,
      PRBOOM_PURGE_LIMIT_INFO_0_NL,
      NULL,
      NULL,
      {
         { "8",   OPTION_VAL_8_NL },
         { "12",  OPTION_VAL_12_NL },
         { "16",  OPTION_VAL_16_NL },
         { "24",  OPTION_VAL_24_NL },
         { "32",  OPTION_VAL_32_NL },
         { "48",  OPTION_VAL_48_NL },
         { "64",  OPTION_VAL_64_NL },
         { "128", OPTION_VAL_128_NL },
         { "256", OPTION_VAL_256_NL },
         { NULL, NULL },
      },
      "16"
   },
#endif
   { NULL, NULL, NULL, NULL, NULL, NULL, {{0}}, NULL },
};
struct retro_core_options_v2 options_nl = {
   option_cats_nl,
   option_defs_nl
};

/* RETRO_LANGUAGE_NO */

#define PRBOOM_RESOLUTION_LABEL_NO NULL
#define PRBOOM_RESOLUTION_INFO_0_NO NULL
#define OPTION_VAL_320X200_NO NULL
#define OPTION_VAL_640X400_NO NULL
#define OPTION_VAL_960X600_NO NULL
#define OPTION_VAL_1280X800_NO NULL
#define OPTION_VAL_1600X1000_NO NULL
#define OPTION_VAL_1920X1200_NO NULL
#define OPTION_VAL_2240X1400_NO NULL
#define OPTION_VAL_2560X1600_NO NULL
#define PRBOOM_MOUSE_ON_LABEL_NO NULL
#define PRBOOM_MOUSE_ON_INFO_0_NO NULL
#define PRBOOM_FIND_RECURSIVE_ON_LABEL_NO NULL
#define PRBOOM_FIND_RECURSIVE_ON_INFO_0_NO NULL
#define PRBOOM_RUMBLE_LABEL_NO NULL
#define PRBOOM_RUMBLE_INFO_0_NO NULL
#define PRBOOM_ANALOG_DEADZONE_LABEL_NO NULL
#define PRBOOM_ANALOG_DEADZONE_INFO_0_NO NULL
#define PRBOOM_PURGE_LIMIT_LABEL_NO NULL
#define PRBOOM_PURGE_LIMIT_INFO_0_NO NULL
#define OPTION_VAL_8_NO NULL
#define OPTION_VAL_12_NO NULL
#define OPTION_VAL_16_NO NULL
#define OPTION_VAL_24_NO NULL
#define OPTION_VAL_32_NO NULL
#define OPTION_VAL_48_NO NULL
#define OPTION_VAL_64_NO NULL
#define OPTION_VAL_128_NO NULL
#define OPTION_VAL_256_NO NULL

struct retro_core_option_v2_category option_cats_no[] = {
   { NULL, NULL, NULL },
};
struct retro_core_option_v2_definition option_defs_no[] = {
   {
      "prboom-resolution",
      PRBOOM_RESOLUTION_LABEL_NO,
      NULL,
      PRBOOM_RESOLUTION_INFO_0_NO,
      NULL,
      NULL,
      {
         { "320x200",   OPTION_VAL_320X200_NO },
         { "640x400",   OPTION_VAL_640X400_NO },
         { "960x600",   OPTION_VAL_960X600_NO },
         { "1280x800",  OPTION_VAL_1280X800_NO },
         { "1600x1000", OPTION_VAL_1600X1000_NO },
         { "1920x1200", OPTION_VAL_1920X1200_NO },
         { "2240x1400", OPTION_VAL_2240X1400_NO },
         { "2560x1600", OPTION_VAL_2560X1600_NO },
         { NULL, NULL },
      },
      "320x200"
   },
   {
      "prboom-mouse_on",
      PRBOOM_MOUSE_ON_LABEL_NO,
      NULL,
      PRBOOM_MOUSE_ON_INFO_0_NO,
      NULL,
      NULL,
      {
         { "disabled", NULL },
         { "enabled",  NULL },
         { NULL, NULL },
      },
      "disabled"
   },
   {
      "prboom-find_recursive_on",
      PRBOOM_FIND_RECURSIVE_ON_LABEL_NO,
      NULL,
      PRBOOM_FIND_RECURSIVE_ON_INFO_0_NO,
      NULL,
      NULL,
      {
         { "disabled", NULL },
         { "enabled",  NULL },
         { NULL, NULL },
      },
      "enabled"
   },
   {
      "prboom-rumble",
      PRBOOM_RUMBLE_LABEL_NO,
      NULL,
      PRBOOM_RUMBLE_INFO_0_NO,
      NULL,
      NULL,
      {
         { "disabled", NULL },
         { "enabled",  NULL },
         { NULL, NULL },
      },
      "disabled"
   },
   {
      "prboom-analog_deadzone",
      PRBOOM_ANALOG_DEADZONE_LABEL_NO,
      NULL,
      PRBOOM_ANALOG_DEADZONE_INFO_0_NO,
      NULL,
      NULL,
      {
         { "0",  NULL },
         { "5",  NULL },
         { "10", NULL },
         { "15", NULL },
         { "20", NULL },
         { "25", NULL },
         { "30", NULL },
         { NULL, NULL },
      },
      "15"
   },
#if defined(MEMORY_LOW)
   {
      "prboom-purge_limit",
      PRBOOM_PURGE_LIMIT_LABEL_NO,
      NULL,
      PRBOOM_PURGE_LIMIT_INFO_0_NO,
      NULL,
      NULL,
      {
         { "8",   OPTION_VAL_8_NO },
         { "12",  OPTION_VAL_12_NO },
         { "16",  OPTION_VAL_16_NO },
         { "24",  OPTION_VAL_24_NO },
         { "32",  OPTION_VAL_32_NO },
         { "48",  OPTION_VAL_48_NO },
         { "64",  OPTION_VAL_64_NO },
         { "128", OPTION_VAL_128_NO },
         { "256", OPTION_VAL_256_NO },
         { NULL, NULL },
      },
      "16"
   },
#endif
   { NULL, NULL, NULL, NULL, NULL, NULL, {{0}}, NULL },
};
struct retro_core_options_v2 options_no = {
   option_cats_no,
   option_defs_no
};

/* RETRO_LANGUAGE_OC */

#define PRBOOM_RESOLUTION_LABEL_OC NULL
#define PRBOOM_RESOLUTION_INFO_0_OC NULL
#define OPTION_VAL_320X200_OC NULL
#define OPTION_VAL_640X400_OC NULL
#define OPTION_VAL_960X600_OC NULL
#define OPTION_VAL_1280X800_OC NULL
#define OPTION_VAL_1600X1000_OC NULL
#define OPTION_VAL_1920X1200_OC NULL
#define OPTION_VAL_2240X1400_OC NULL
#define OPTION_VAL_2560X1600_OC NULL
#define PRBOOM_MOUSE_ON_LABEL_OC NULL
#define PRBOOM_MOUSE_ON_INFO_0_OC NULL
#define PRBOOM_FIND_RECURSIVE_ON_LABEL_OC NULL
#define PRBOOM_FIND_RECURSIVE_ON_INFO_0_OC NULL
#define PRBOOM_RUMBLE_LABEL_OC NULL
#define PRBOOM_RUMBLE_INFO_0_OC NULL
#define PRBOOM_ANALOG_DEADZONE_LABEL_OC NULL
#define PRBOOM_ANALOG_DEADZONE_INFO_0_OC NULL
#define PRBOOM_PURGE_LIMIT_LABEL_OC NULL
#define PRBOOM_PURGE_LIMIT_INFO_0_OC NULL
#define OPTION_VAL_8_OC NULL
#define OPTION_VAL_12_OC NULL
#define OPTION_VAL_16_OC NULL
#define OPTION_VAL_24_OC NULL
#define OPTION_VAL_32_OC NULL
#define OPTION_VAL_48_OC NULL
#define OPTION_VAL_64_OC NULL
#define OPTION_VAL_128_OC NULL
#define OPTION_VAL_256_OC NULL

struct retro_core_option_v2_category option_cats_oc[] = {
   { NULL, NULL, NULL },
};
struct retro_core_option_v2_definition option_defs_oc[] = {
   {
      "prboom-resolution",
      PRBOOM_RESOLUTION_LABEL_OC,
      NULL,
      PRBOOM_RESOLUTION_INFO_0_OC,
      NULL,
      NULL,
      {
         { "320x200",   OPTION_VAL_320X200_OC },
         { "640x400",   OPTION_VAL_640X400_OC },
         { "960x600",   OPTION_VAL_960X600_OC },
         { "1280x800",  OPTION_VAL_1280X800_OC },
         { "1600x1000", OPTION_VAL_1600X1000_OC },
         { "1920x1200", OPTION_VAL_1920X1200_OC },
         { "2240x1400", OPTION_VAL_2240X1400_OC },
         { "2560x1600", OPTION_VAL_2560X1600_OC },
         { NULL, NULL },
      },
      "320x200"
   },
   {
      "prboom-mouse_on",
      PRBOOM_MOUSE_ON_LABEL_OC,
      NULL,
      PRBOOM_MOUSE_ON_INFO_0_OC,
      NULL,
      NULL,
      {
         { "disabled", NULL },
         { "enabled",  NULL },
         { NULL, NULL },
      },
      "disabled"
   },
   {
      "prboom-find_recursive_on",
      PRBOOM_FIND_RECURSIVE_ON_LABEL_OC,
      NULL,
      PRBOOM_FIND_RECURSIVE_ON_INFO_0_OC,
      NULL,
      NULL,
      {
         { "disabled", NULL },
         { "enabled",  NULL },
         { NULL, NULL },
      },
      "enabled"
   },
   {
      "prboom-rumble",
      PRBOOM_RUMBLE_LABEL_OC,
      NULL,
      PRBOOM_RUMBLE_INFO_0_OC,
      NULL,
      NULL,
      {
         { "disabled", NULL },
         { "enabled",  NULL },
         { NULL, NULL },
      },
      "disabled"
   },
   {
      "prboom-analog_deadzone",
      PRBOOM_ANALOG_DEADZONE_LABEL_OC,
      NULL,
      PRBOOM_ANALOG_DEADZONE_INFO_0_OC,
      NULL,
      NULL,
      {
         { "0",  NULL },
         { "5",  NULL },
         { "10", NULL },
         { "15", NULL },
         { "20", NULL },
         { "25", NULL },
         { "30", NULL },
         { NULL, NULL },
      },
      "15"
   },
#if defined(MEMORY_LOW)
   {
      "prboom-purge_limit",
      PRBOOM_PURGE_LIMIT_LABEL_OC,
      NULL,
      PRBOOM_PURGE_LIMIT_INFO_0_OC,
      NULL,
      NULL,
      {
         { "8",   OPTION_VAL_8_OC },
         { "12",  OPTION_VAL_12_OC },
         { "16",  OPTION_VAL_16_OC },
         { "24",  OPTION_VAL_24_OC },
         { "32",  OPTION_VAL_32_OC },
         { "48",  OPTION_VAL_48_OC },
         { "64",  OPTION_VAL_64_OC },
         { "128", OPTION_VAL_128_OC },
         { "256", OPTION_VAL_256_OC },
         { NULL, NULL },
      },
      "16"
   },
#endif
   { NULL, NULL, NULL, NULL, NULL, NULL, {{0}}, NULL },
};
struct retro_core_options_v2 options_oc = {
   option_cats_oc,
   option_defs_oc
};

/* RETRO_LANGUAGE_PL */

#define PRBOOM_RESOLUTION_LABEL_PL NULL
#define PRBOOM_RESOLUTION_INFO_0_PL "Skonfiguruj rozdzielczość."
#define OPTION_VAL_320X200_PL NULL
#define OPTION_VAL_640X400_PL NULL
#define OPTION_VAL_960X600_PL NULL
#define OPTION_VAL_1280X800_PL NULL
#define OPTION_VAL_1600X1000_PL NULL
#define OPTION_VAL_1920X1200_PL NULL
#define OPTION_VAL_2240X1400_PL NULL
#define OPTION_VAL_2560X1600_PL NULL
#define PRBOOM_MOUSE_ON_LABEL_PL "Mysz aktywna podczas korzystania z Gamepad"
#define PRBOOM_MOUSE_ON_INFO_0_PL NULL
#define PRBOOM_FIND_RECURSIVE_ON_LABEL_PL NULL
#define PRBOOM_FIND_RECURSIVE_ON_INFO_0_PL NULL
#define PRBOOM_RUMBLE_LABEL_PL "Efekty rozrastania"
#define PRBOOM_RUMBLE_INFO_0_PL NULL
#define PRBOOM_ANALOG_DEADZONE_LABEL_PL NULL
#define PRBOOM_ANALOG_DEADZONE_INFO_0_PL NULL
#define PRBOOM_PURGE_LIMIT_LABEL_PL "Wielkość pamięci podręcznej"
#define PRBOOM_PURGE_LIMIT_INFO_0_PL NULL
#define OPTION_VAL_8_PL NULL
#define OPTION_VAL_12_PL NULL
#define OPTION_VAL_16_PL NULL
#define OPTION_VAL_24_PL NULL
#define OPTION_VAL_32_PL NULL
#define OPTION_VAL_48_PL NULL
#define OPTION_VAL_64_PL NULL
#define OPTION_VAL_128_PL NULL
#define OPTION_VAL_256_PL NULL

struct retro_core_option_v2_category option_cats_pl[] = {
   { NULL, NULL, NULL },
};
struct retro_core_option_v2_definition option_defs_pl[] = {
   {
      "prboom-resolution",
      PRBOOM_RESOLUTION_LABEL_PL,
      NULL,
      PRBOOM_RESOLUTION_INFO_0_PL,
      NULL,
      NULL,
      {
         { "320x200",   OPTION_VAL_320X200_PL },
         { "640x400",   OPTION_VAL_640X400_PL },
         { "960x600",   OPTION_VAL_960X600_PL },
         { "1280x800",  OPTION_VAL_1280X800_PL },
         { "1600x1000", OPTION_VAL_1600X1000_PL },
         { "1920x1200", OPTION_VAL_1920X1200_PL },
         { "2240x1400", OPTION_VAL_2240X1400_PL },
         { "2560x1600", OPTION_VAL_2560X1600_PL },
         { NULL, NULL },
      },
      "320x200"
   },
   {
      "prboom-mouse_on",
      PRBOOM_MOUSE_ON_LABEL_PL,
      NULL,
      PRBOOM_MOUSE_ON_INFO_0_PL,
      NULL,
      NULL,
      {
         { "disabled", NULL },
         { "enabled",  NULL },
         { NULL, NULL },
      },
      "disabled"
   },
   {
      "prboom-find_recursive_on",
      PRBOOM_FIND_RECURSIVE_ON_LABEL_PL,
      NULL,
      PRBOOM_FIND_RECURSIVE_ON_INFO_0_PL,
      NULL,
      NULL,
      {
         { "disabled", NULL },
         { "enabled",  NULL },
         { NULL, NULL },
      },
      "enabled"
   },
   {
      "prboom-rumble",
      PRBOOM_RUMBLE_LABEL_PL,
      NULL,
      PRBOOM_RUMBLE_INFO_0_PL,
      NULL,
      NULL,
      {
         { "disabled", NULL },
         { "enabled",  NULL },
         { NULL, NULL },
      },
      "disabled"
   },
   {
      "prboom-analog_deadzone",
      PRBOOM_ANALOG_DEADZONE_LABEL_PL,
      NULL,
      PRBOOM_ANALOG_DEADZONE_INFO_0_PL,
      NULL,
      NULL,
      {
         { "0",  NULL },
         { "5",  NULL },
         { "10", NULL },
         { "15", NULL },
         { "20", NULL },
         { "25", NULL },
         { "30", NULL },
         { NULL, NULL },
      },
      "15"
   },
#if defined(MEMORY_LOW)
   {
      "prboom-purge_limit",
      PRBOOM_PURGE_LIMIT_LABEL_PL,
      NULL,
      PRBOOM_PURGE_LIMIT_INFO_0_PL,
      NULL,
      NULL,
      {
         { "8",   OPTION_VAL_8_PL },
         { "12",  OPTION_VAL_12_PL },
         { "16",  OPTION_VAL_16_PL },
         { "24",  OPTION_VAL_24_PL },
         { "32",  OPTION_VAL_32_PL },
         { "48",  OPTION_VAL_48_PL },
         { "64",  OPTION_VAL_64_PL },
         { "128", OPTION_VAL_128_PL },
         { "256", OPTION_VAL_256_PL },
         { NULL, NULL },
      },
      "16"
   },
#endif
   { NULL, NULL, NULL, NULL, NULL, NULL, {{0}}, NULL },
};
struct retro_core_options_v2 options_pl = {
   option_cats_pl,
   option_defs_pl
};

/* RETRO_LANGUAGE_PT_BR */

#define PRBOOM_RESOLUTION_LABEL_PT_BR "Resolução interna (requer reinício)"
#define PRBOOM_RESOLUTION_INFO_0_PT_BR "Configurar a resolução."
#define OPTION_VAL_320X200_PT_BR NULL
#define OPTION_VAL_640X400_PT_BR NULL
#define OPTION_VAL_960X600_PT_BR NULL
#define OPTION_VAL_1280X800_PT_BR NULL
#define OPTION_VAL_1600X1000_PT_BR NULL
#define OPTION_VAL_1920X1200_PT_BR NULL
#define OPTION_VAL_2240X1400_PT_BR NULL
#define OPTION_VAL_2560X1600_PT_BR NULL
#define PRBOOM_MOUSE_ON_LABEL_PT_BR "Ativar o mouse ao utilizar o controle"
#define PRBOOM_MOUSE_ON_INFO_0_PT_BR "Permite que você use as entradas do mouse mesmo quando o tipo do dispositivo do usuário 1 não estiver definido como 'RetroKeyboard/Mouse'."
#define PRBOOM_FIND_RECURSIVE_ON_LABEL_PT_BR "Procurar arquivos IWAD nas pastas principais"
#define PRBOOM_FIND_RECURSIVE_ON_INFO_0_PT_BR "Varre as pastas principais por IWADs. OBSERVAÇÃO: É preciso desativar esta opção caso queira executar o SIGIL."
#define PRBOOM_RUMBLE_LABEL_PT_BR "Efeitos de vibração"
#define PRBOOM_RUMBLE_INFO_0_PT_BR "Permite um resposta háptica ao utilizar um controle equipado com vibração."
#define PRBOOM_ANALOG_DEADZONE_LABEL_PT_BR "Zona morta dos analógicos (porcentagem)"
#define PRBOOM_ANALOG_DEADZONE_INFO_0_PT_BR "Define a zona morta dos controles analógicos quando o tipo do dispositivo de entrada for definido como \"Gamepad moderno\"."
#define PRBOOM_PURGE_LIMIT_LABEL_PT_BR "Tamanho do cache"
#define PRBOOM_PURGE_LIMIT_INFO_0_PT_BR "Estabelece um limite para o tamanho do espaço da memória utilizado para armazenar os ativos do jogo. Pequenos valores podem causar disfunções durante a navegação nos mapas grandes."
#define OPTION_VAL_8_PT_BR NULL
#define OPTION_VAL_12_PT_BR NULL
#define OPTION_VAL_16_PT_BR NULL
#define OPTION_VAL_24_PT_BR NULL
#define OPTION_VAL_32_PT_BR NULL
#define OPTION_VAL_48_PT_BR NULL
#define OPTION_VAL_64_PT_BR NULL
#define OPTION_VAL_128_PT_BR NULL
#define OPTION_VAL_256_PT_BR NULL

struct retro_core_option_v2_category option_cats_pt_br[] = {
   { NULL, NULL, NULL },
};
struct retro_core_option_v2_definition option_defs_pt_br[] = {
   {
      "prboom-resolution",
      PRBOOM_RESOLUTION_LABEL_PT_BR,
      NULL,
      PRBOOM_RESOLUTION_INFO_0_PT_BR,
      NULL,
      NULL,
      {
         { "320x200",   OPTION_VAL_320X200_PT_BR },
         { "640x400",   OPTION_VAL_640X400_PT_BR },
         { "960x600",   OPTION_VAL_960X600_PT_BR },
         { "1280x800",  OPTION_VAL_1280X800_PT_BR },
         { "1600x1000", OPTION_VAL_1600X1000_PT_BR },
         { "1920x1200", OPTION_VAL_1920X1200_PT_BR },
         { "2240x1400", OPTION_VAL_2240X1400_PT_BR },
         { "2560x1600", OPTION_VAL_2560X1600_PT_BR },
         { NULL, NULL },
      },
      "320x200"
   },
   {
      "prboom-mouse_on",
      PRBOOM_MOUSE_ON_LABEL_PT_BR,
      NULL,
      PRBOOM_MOUSE_ON_INFO_0_PT_BR,
      NULL,
      NULL,
      {
         { "disabled", NULL },
         { "enabled",  NULL },
         { NULL, NULL },
      },
      "disabled"
   },
   {
      "prboom-find_recursive_on",
      PRBOOM_FIND_RECURSIVE_ON_LABEL_PT_BR,
      NULL,
      PRBOOM_FIND_RECURSIVE_ON_INFO_0_PT_BR,
      NULL,
      NULL,
      {
         { "disabled", NULL },
         { "enabled",  NULL },
         { NULL, NULL },
      },
      "enabled"
   },
   {
      "prboom-rumble",
      PRBOOM_RUMBLE_LABEL_PT_BR,
      NULL,
      PRBOOM_RUMBLE_INFO_0_PT_BR,
      NULL,
      NULL,
      {
         { "disabled", NULL },
         { "enabled",  NULL },
         { NULL, NULL },
      },
      "disabled"
   },
   {
      "prboom-analog_deadzone",
      PRBOOM_ANALOG_DEADZONE_LABEL_PT_BR,
      NULL,
      PRBOOM_ANALOG_DEADZONE_INFO_0_PT_BR,
      NULL,
      NULL,
      {
         { "0",  NULL },
         { "5",  NULL },
         { "10", NULL },
         { "15", NULL },
         { "20", NULL },
         { "25", NULL },
         { "30", NULL },
         { NULL, NULL },
      },
      "15"
   },
#if defined(MEMORY_LOW)
   {
      "prboom-purge_limit",
      PRBOOM_PURGE_LIMIT_LABEL_PT_BR,
      NULL,
      PRBOOM_PURGE_LIMIT_INFO_0_PT_BR,
      NULL,
      NULL,
      {
         { "8",   OPTION_VAL_8_PT_BR },
         { "12",  OPTION_VAL_12_PT_BR },
         { "16",  OPTION_VAL_16_PT_BR },
         { "24",  OPTION_VAL_24_PT_BR },
         { "32",  OPTION_VAL_32_PT_BR },
         { "48",  OPTION_VAL_48_PT_BR },
         { "64",  OPTION_VAL_64_PT_BR },
         { "128", OPTION_VAL_128_PT_BR },
         { "256", OPTION_VAL_256_PT_BR },
         { NULL, NULL },
      },
      "16"
   },
#endif
   { NULL, NULL, NULL, NULL, NULL, NULL, {{0}}, NULL },
};
struct retro_core_options_v2 options_pt_br = {
   option_cats_pt_br,
   option_defs_pt_br
};

/* RETRO_LANGUAGE_PT_PT */

#define PRBOOM_RESOLUTION_LABEL_PT_PT NULL
#define PRBOOM_RESOLUTION_INFO_0_PT_PT NULL
#define OPTION_VAL_320X200_PT_PT NULL
#define OPTION_VAL_640X400_PT_PT NULL
#define OPTION_VAL_960X600_PT_PT NULL
#define OPTION_VAL_1280X800_PT_PT NULL
#define OPTION_VAL_1600X1000_PT_PT NULL
#define OPTION_VAL_1920X1200_PT_PT NULL
#define OPTION_VAL_2240X1400_PT_PT NULL
#define OPTION_VAL_2560X1600_PT_PT NULL
#define PRBOOM_MOUSE_ON_LABEL_PT_PT NULL
#define PRBOOM_MOUSE_ON_INFO_0_PT_PT NULL
#define PRBOOM_FIND_RECURSIVE_ON_LABEL_PT_PT NULL
#define PRBOOM_FIND_RECURSIVE_ON_INFO_0_PT_PT NULL
#define PRBOOM_RUMBLE_LABEL_PT_PT NULL
#define PRBOOM_RUMBLE_INFO_0_PT_PT NULL
#define PRBOOM_ANALOG_DEADZONE_LABEL_PT_PT NULL
#define PRBOOM_ANALOG_DEADZONE_INFO_0_PT_PT NULL
#define PRBOOM_PURGE_LIMIT_LABEL_PT_PT NULL
#define PRBOOM_PURGE_LIMIT_INFO_0_PT_PT NULL
#define OPTION_VAL_8_PT_PT NULL
#define OPTION_VAL_12_PT_PT NULL
#define OPTION_VAL_16_PT_PT NULL
#define OPTION_VAL_24_PT_PT NULL
#define OPTION_VAL_32_PT_PT NULL
#define OPTION_VAL_48_PT_PT NULL
#define OPTION_VAL_64_PT_PT NULL
#define OPTION_VAL_128_PT_PT NULL
#define OPTION_VAL_256_PT_PT NULL

struct retro_core_option_v2_category option_cats_pt_pt[] = {
   { NULL, NULL, NULL },
};
struct retro_core_option_v2_definition option_defs_pt_pt[] = {
   {
      "prboom-resolution",
      PRBOOM_RESOLUTION_LABEL_PT_PT,
      NULL,
      PRBOOM_RESOLUTION_INFO_0_PT_PT,
      NULL,
      NULL,
      {
         { "320x200",   OPTION_VAL_320X200_PT_PT },
         { "640x400",   OPTION_VAL_640X400_PT_PT },
         { "960x600",   OPTION_VAL_960X600_PT_PT },
         { "1280x800",  OPTION_VAL_1280X800_PT_PT },
         { "1600x1000", OPTION_VAL_1600X1000_PT_PT },
         { "1920x1200", OPTION_VAL_1920X1200_PT_PT },
         { "2240x1400", OPTION_VAL_2240X1400_PT_PT },
         { "2560x1600", OPTION_VAL_2560X1600_PT_PT },
         { NULL, NULL },
      },
      "320x200"
   },
   {
      "prboom-mouse_on",
      PRBOOM_MOUSE_ON_LABEL_PT_PT,
      NULL,
      PRBOOM_MOUSE_ON_INFO_0_PT_PT,
      NULL,
      NULL,
      {
         { "disabled", NULL },
         { "enabled",  NULL },
         { NULL, NULL },
      },
      "disabled"
   },
   {
      "prboom-find_recursive_on",
      PRBOOM_FIND_RECURSIVE_ON_LABEL_PT_PT,
      NULL,
      PRBOOM_FIND_RECURSIVE_ON_INFO_0_PT_PT,
      NULL,
      NULL,
      {
         { "disabled", NULL },
         { "enabled",  NULL },
         { NULL, NULL },
      },
      "enabled"
   },
   {
      "prboom-rumble",
      PRBOOM_RUMBLE_LABEL_PT_PT,
      NULL,
      PRBOOM_RUMBLE_INFO_0_PT_PT,
      NULL,
      NULL,
      {
         { "disabled", NULL },
         { "enabled",  NULL },
         { NULL, NULL },
      },
      "disabled"
   },
   {
      "prboom-analog_deadzone",
      PRBOOM_ANALOG_DEADZONE_LABEL_PT_PT,
      NULL,
      PRBOOM_ANALOG_DEADZONE_INFO_0_PT_PT,
      NULL,
      NULL,
      {
         { "0",  NULL },
         { "5",  NULL },
         { "10", NULL },
         { "15", NULL },
         { "20", NULL },
         { "25", NULL },
         { "30", NULL },
         { NULL, NULL },
      },
      "15"
   },
#if defined(MEMORY_LOW)
   {
      "prboom-purge_limit",
      PRBOOM_PURGE_LIMIT_LABEL_PT_PT,
      NULL,
      PRBOOM_PURGE_LIMIT_INFO_0_PT_PT,
      NULL,
      NULL,
      {
         { "8",   OPTION_VAL_8_PT_PT },
         { "12",  OPTION_VAL_12_PT_PT },
         { "16",  OPTION_VAL_16_PT_PT },
         { "24",  OPTION_VAL_24_PT_PT },
         { "32",  OPTION_VAL_32_PT_PT },
         { "48",  OPTION_VAL_48_PT_PT },
         { "64",  OPTION_VAL_64_PT_PT },
         { "128", OPTION_VAL_128_PT_PT },
         { "256", OPTION_VAL_256_PT_PT },
         { NULL, NULL },
      },
      "16"
   },
#endif
   { NULL, NULL, NULL, NULL, NULL, NULL, {{0}}, NULL },
};
struct retro_core_options_v2 options_pt_pt = {
   option_cats_pt_pt,
   option_defs_pt_pt
};

/* RETRO_LANGUAGE_RO */

#define PRBOOM_RESOLUTION_LABEL_RO NULL
#define PRBOOM_RESOLUTION_INFO_0_RO NULL
#define OPTION_VAL_320X200_RO NULL
#define OPTION_VAL_640X400_RO NULL
#define OPTION_VAL_960X600_RO NULL
#define OPTION_VAL_1280X800_RO NULL
#define OPTION_VAL_1600X1000_RO NULL
#define OPTION_VAL_1920X1200_RO NULL
#define OPTION_VAL_2240X1400_RO NULL
#define OPTION_VAL_2560X1600_RO NULL
#define PRBOOM_MOUSE_ON_LABEL_RO NULL
#define PRBOOM_MOUSE_ON_INFO_0_RO NULL
#define PRBOOM_FIND_RECURSIVE_ON_LABEL_RO NULL
#define PRBOOM_FIND_RECURSIVE_ON_INFO_0_RO NULL
#define PRBOOM_RUMBLE_LABEL_RO NULL
#define PRBOOM_RUMBLE_INFO_0_RO NULL
#define PRBOOM_ANALOG_DEADZONE_LABEL_RO NULL
#define PRBOOM_ANALOG_DEADZONE_INFO_0_RO NULL
#define PRBOOM_PURGE_LIMIT_LABEL_RO NULL
#define PRBOOM_PURGE_LIMIT_INFO_0_RO NULL
#define OPTION_VAL_8_RO NULL
#define OPTION_VAL_12_RO NULL
#define OPTION_VAL_16_RO NULL
#define OPTION_VAL_24_RO NULL
#define OPTION_VAL_32_RO NULL
#define OPTION_VAL_48_RO NULL
#define OPTION_VAL_64_RO NULL
#define OPTION_VAL_128_RO NULL
#define OPTION_VAL_256_RO NULL

struct retro_core_option_v2_category option_cats_ro[] = {
   { NULL, NULL, NULL },
};
struct retro_core_option_v2_definition option_defs_ro[] = {
   {
      "prboom-resolution",
      PRBOOM_RESOLUTION_LABEL_RO,
      NULL,
      PRBOOM_RESOLUTION_INFO_0_RO,
      NULL,
      NULL,
      {
         { "320x200",   OPTION_VAL_320X200_RO },
         { "640x400",   OPTION_VAL_640X400_RO },
         { "960x600",   OPTION_VAL_960X600_RO },
         { "1280x800",  OPTION_VAL_1280X800_RO },
         { "1600x1000", OPTION_VAL_1600X1000_RO },
         { "1920x1200", OPTION_VAL_1920X1200_RO },
         { "2240x1400", OPTION_VAL_2240X1400_RO },
         { "2560x1600", OPTION_VAL_2560X1600_RO },
         { NULL, NULL },
      },
      "320x200"
   },
   {
      "prboom-mouse_on",
      PRBOOM_MOUSE_ON_LABEL_RO,
      NULL,
      PRBOOM_MOUSE_ON_INFO_0_RO,
      NULL,
      NULL,
      {
         { "disabled", NULL },
         { "enabled",  NULL },
         { NULL, NULL },
      },
      "disabled"
   },
   {
      "prboom-find_recursive_on",
      PRBOOM_FIND_RECURSIVE_ON_LABEL_RO,
      NULL,
      PRBOOM_FIND_RECURSIVE_ON_INFO_0_RO,
      NULL,
      NULL,
      {
         { "disabled", NULL },
         { "enabled",  NULL },
         { NULL, NULL },
      },
      "enabled"
   },
   {
      "prboom-rumble",
      PRBOOM_RUMBLE_LABEL_RO,
      NULL,
      PRBOOM_RUMBLE_INFO_0_RO,
      NULL,
      NULL,
      {
         { "disabled", NULL },
         { "enabled",  NULL },
         { NULL, NULL },
      },
      "disabled"
   },
   {
      "prboom-analog_deadzone",
      PRBOOM_ANALOG_DEADZONE_LABEL_RO,
      NULL,
      PRBOOM_ANALOG_DEADZONE_INFO_0_RO,
      NULL,
      NULL,
      {
         { "0",  NULL },
         { "5",  NULL },
         { "10", NULL },
         { "15", NULL },
         { "20", NULL },
         { "25", NULL },
         { "30", NULL },
         { NULL, NULL },
      },
      "15"
   },
#if defined(MEMORY_LOW)
   {
      "prboom-purge_limit",
      PRBOOM_PURGE_LIMIT_LABEL_RO,
      NULL,
      PRBOOM_PURGE_LIMIT_INFO_0_RO,
      NULL,
      NULL,
      {
         { "8",   OPTION_VAL_8_RO },
         { "12",  OPTION_VAL_12_RO },
         { "16",  OPTION_VAL_16_RO },
         { "24",  OPTION_VAL_24_RO },
         { "32",  OPTION_VAL_32_RO },
         { "48",  OPTION_VAL_48_RO },
         { "64",  OPTION_VAL_64_RO },
         { "128", OPTION_VAL_128_RO },
         { "256", OPTION_VAL_256_RO },
         { NULL, NULL },
      },
      "16"
   },
#endif
   { NULL, NULL, NULL, NULL, NULL, NULL, {{0}}, NULL },
};
struct retro_core_options_v2 options_ro = {
   option_cats_ro,
   option_defs_ro
};

/* RETRO_LANGUAGE_RU */

#define PRBOOM_RESOLUTION_LABEL_RU "Внутреннее разрешение (требуется перезапуск)"
#define PRBOOM_RESOLUTION_INFO_0_RU "Настройка разрешения."
#define OPTION_VAL_320X200_RU NULL
#define OPTION_VAL_640X400_RU NULL
#define OPTION_VAL_960X600_RU NULL
#define OPTION_VAL_1280X800_RU NULL
#define OPTION_VAL_1600X1000_RU NULL
#define OPTION_VAL_1920X1200_RU NULL
#define OPTION_VAL_2240X1400_RU NULL
#define OPTION_VAL_2560X1600_RU NULL
#define PRBOOM_MOUSE_ON_LABEL_RU "Использовать мышку совместно с геймпадом"
#define PRBOOM_MOUSE_ON_INFO_0_RU "Позволяет использовать мышку даже когда тип устройства Игрока 1 отличается от 'RetroKeyboard/Mouse'."
#define PRBOOM_FIND_RECURSIVE_ON_LABEL_RU "Искать IWAD в родительских папках"
#define PRBOOM_FIND_RECURSIVE_ON_INFO_0_RU "Искать файлы IWAD в родительских каталогах. Примечание: отключите для запуска SIGIL."
#define PRBOOM_RUMBLE_LABEL_RU "Эффекты вибрации"
#define PRBOOM_RUMBLE_INFO_0_RU "Включает тактильную отдачу для совместимых геймпадов."
#define PRBOOM_ANALOG_DEADZONE_LABEL_RU "Мёртвая зона стиков (в процентах)"
#define PRBOOM_ANALOG_DEADZONE_INFO_0_RU "Установка мёртвой зоны аналоговых стиков геймпада для устройства ввода типа 'Gamepad Modern'."
#define PRBOOM_PURGE_LIMIT_LABEL_RU "Размер кэша"
#define PRBOOM_PURGE_LIMIT_INFO_0_RU "Установка ограничения памяти, доступной для кэширования ресурсов игры. Меньшие значения могут вызывать подтормаживания на больших картах."
#define OPTION_VAL_8_RU NULL
#define OPTION_VAL_12_RU NULL
#define OPTION_VAL_16_RU NULL
#define OPTION_VAL_24_RU NULL
#define OPTION_VAL_32_RU NULL
#define OPTION_VAL_48_RU NULL
#define OPTION_VAL_64_RU NULL
#define OPTION_VAL_128_RU NULL
#define OPTION_VAL_256_RU NULL

struct retro_core_option_v2_category option_cats_ru[] = {
   { NULL, NULL, NULL },
};
struct retro_core_option_v2_definition option_defs_ru[] = {
   {
      "prboom-resolution",
      PRBOOM_RESOLUTION_LABEL_RU,
      NULL,
      PRBOOM_RESOLUTION_INFO_0_RU,
      NULL,
      NULL,
      {
         { "320x200",   OPTION_VAL_320X200_RU },
         { "640x400",   OPTION_VAL_640X400_RU },
         { "960x600",   OPTION_VAL_960X600_RU },
         { "1280x800",  OPTION_VAL_1280X800_RU },
         { "1600x1000", OPTION_VAL_1600X1000_RU },
         { "1920x1200", OPTION_VAL_1920X1200_RU },
         { "2240x1400", OPTION_VAL_2240X1400_RU },
         { "2560x1600", OPTION_VAL_2560X1600_RU },
         { NULL, NULL },
      },
      "320x200"
   },
   {
      "prboom-mouse_on",
      PRBOOM_MOUSE_ON_LABEL_RU,
      NULL,
      PRBOOM_MOUSE_ON_INFO_0_RU,
      NULL,
      NULL,
      {
         { "disabled", NULL },
         { "enabled",  NULL },
         { NULL, NULL },
      },
      "disabled"
   },
   {
      "prboom-find_recursive_on",
      PRBOOM_FIND_RECURSIVE_ON_LABEL_RU,
      NULL,
      PRBOOM_FIND_RECURSIVE_ON_INFO_0_RU,
      NULL,
      NULL,
      {
         { "disabled", NULL },
         { "enabled",  NULL },
         { NULL, NULL },
      },
      "enabled"
   },
   {
      "prboom-rumble",
      PRBOOM_RUMBLE_LABEL_RU,
      NULL,
      PRBOOM_RUMBLE_INFO_0_RU,
      NULL,
      NULL,
      {
         { "disabled", NULL },
         { "enabled",  NULL },
         { NULL, NULL },
      },
      "disabled"
   },
   {
      "prboom-analog_deadzone",
      PRBOOM_ANALOG_DEADZONE_LABEL_RU,
      NULL,
      PRBOOM_ANALOG_DEADZONE_INFO_0_RU,
      NULL,
      NULL,
      {
         { "0",  NULL },
         { "5",  NULL },
         { "10", NULL },
         { "15", NULL },
         { "20", NULL },
         { "25", NULL },
         { "30", NULL },
         { NULL, NULL },
      },
      "15"
   },
#if defined(MEMORY_LOW)
   {
      "prboom-purge_limit",
      PRBOOM_PURGE_LIMIT_LABEL_RU,
      NULL,
      PRBOOM_PURGE_LIMIT_INFO_0_RU,
      NULL,
      NULL,
      {
         { "8",   OPTION_VAL_8_RU },
         { "12",  OPTION_VAL_12_RU },
         { "16",  OPTION_VAL_16_RU },
         { "24",  OPTION_VAL_24_RU },
         { "32",  OPTION_VAL_32_RU },
         { "48",  OPTION_VAL_48_RU },
         { "64",  OPTION_VAL_64_RU },
         { "128", OPTION_VAL_128_RU },
         { "256", OPTION_VAL_256_RU },
         { NULL, NULL },
      },
      "16"
   },
#endif
   { NULL, NULL, NULL, NULL, NULL, NULL, {{0}}, NULL },
};
struct retro_core_options_v2 options_ru = {
   option_cats_ru,
   option_defs_ru
};

/* RETRO_LANGUAGE_SI */

#define PRBOOM_RESOLUTION_LABEL_SI NULL
#define PRBOOM_RESOLUTION_INFO_0_SI NULL
#define OPTION_VAL_320X200_SI NULL
#define OPTION_VAL_640X400_SI NULL
#define OPTION_VAL_960X600_SI NULL
#define OPTION_VAL_1280X800_SI NULL
#define OPTION_VAL_1600X1000_SI NULL
#define OPTION_VAL_1920X1200_SI NULL
#define OPTION_VAL_2240X1400_SI NULL
#define OPTION_VAL_2560X1600_SI NULL
#define PRBOOM_MOUSE_ON_LABEL_SI NULL
#define PRBOOM_MOUSE_ON_INFO_0_SI NULL
#define PRBOOM_FIND_RECURSIVE_ON_LABEL_SI NULL
#define PRBOOM_FIND_RECURSIVE_ON_INFO_0_SI NULL
#define PRBOOM_RUMBLE_LABEL_SI NULL
#define PRBOOM_RUMBLE_INFO_0_SI NULL
#define PRBOOM_ANALOG_DEADZONE_LABEL_SI NULL
#define PRBOOM_ANALOG_DEADZONE_INFO_0_SI NULL
#define PRBOOM_PURGE_LIMIT_LABEL_SI NULL
#define PRBOOM_PURGE_LIMIT_INFO_0_SI NULL
#define OPTION_VAL_8_SI NULL
#define OPTION_VAL_12_SI NULL
#define OPTION_VAL_16_SI NULL
#define OPTION_VAL_24_SI NULL
#define OPTION_VAL_32_SI NULL
#define OPTION_VAL_48_SI NULL
#define OPTION_VAL_64_SI NULL
#define OPTION_VAL_128_SI NULL
#define OPTION_VAL_256_SI NULL

struct retro_core_option_v2_category option_cats_si[] = {
   { NULL, NULL, NULL },
};
struct retro_core_option_v2_definition option_defs_si[] = {
   {
      "prboom-resolution",
      PRBOOM_RESOLUTION_LABEL_SI,
      NULL,
      PRBOOM_RESOLUTION_INFO_0_SI,
      NULL,
      NULL,
      {
         { "320x200",   OPTION_VAL_320X200_SI },
         { "640x400",   OPTION_VAL_640X400_SI },
         { "960x600",   OPTION_VAL_960X600_SI },
         { "1280x800",  OPTION_VAL_1280X800_SI },
         { "1600x1000", OPTION_VAL_1600X1000_SI },
         { "1920x1200", OPTION_VAL_1920X1200_SI },
         { "2240x1400", OPTION_VAL_2240X1400_SI },
         { "2560x1600", OPTION_VAL_2560X1600_SI },
         { NULL, NULL },
      },
      "320x200"
   },
   {
      "prboom-mouse_on",
      PRBOOM_MOUSE_ON_LABEL_SI,
      NULL,
      PRBOOM_MOUSE_ON_INFO_0_SI,
      NULL,
      NULL,
      {
         { "disabled", NULL },
         { "enabled",  NULL },
         { NULL, NULL },
      },
      "disabled"
   },
   {
      "prboom-find_recursive_on",
      PRBOOM_FIND_RECURSIVE_ON_LABEL_SI,
      NULL,
      PRBOOM_FIND_RECURSIVE_ON_INFO_0_SI,
      NULL,
      NULL,
      {
         { "disabled", NULL },
         { "enabled",  NULL },
         { NULL, NULL },
      },
      "enabled"
   },
   {
      "prboom-rumble",
      PRBOOM_RUMBLE_LABEL_SI,
      NULL,
      PRBOOM_RUMBLE_INFO_0_SI,
      NULL,
      NULL,
      {
         { "disabled", NULL },
         { "enabled",  NULL },
         { NULL, NULL },
      },
      "disabled"
   },
   {
      "prboom-analog_deadzone",
      PRBOOM_ANALOG_DEADZONE_LABEL_SI,
      NULL,
      PRBOOM_ANALOG_DEADZONE_INFO_0_SI,
      NULL,
      NULL,
      {
         { "0",  NULL },
         { "5",  NULL },
         { "10", NULL },
         { "15", NULL },
         { "20", NULL },
         { "25", NULL },
         { "30", NULL },
         { NULL, NULL },
      },
      "15"
   },
#if defined(MEMORY_LOW)
   {
      "prboom-purge_limit",
      PRBOOM_PURGE_LIMIT_LABEL_SI,
      NULL,
      PRBOOM_PURGE_LIMIT_INFO_0_SI,
      NULL,
      NULL,
      {
         { "8",   OPTION_VAL_8_SI },
         { "12",  OPTION_VAL_12_SI },
         { "16",  OPTION_VAL_16_SI },
         { "24",  OPTION_VAL_24_SI },
         { "32",  OPTION_VAL_32_SI },
         { "48",  OPTION_VAL_48_SI },
         { "64",  OPTION_VAL_64_SI },
         { "128", OPTION_VAL_128_SI },
         { "256", OPTION_VAL_256_SI },
         { NULL, NULL },
      },
      "16"
   },
#endif
   { NULL, NULL, NULL, NULL, NULL, NULL, {{0}}, NULL },
};
struct retro_core_options_v2 options_si = {
   option_cats_si,
   option_defs_si
};

/* RETRO_LANGUAGE_SK */

#define PRBOOM_RESOLUTION_LABEL_SK NULL
#define PRBOOM_RESOLUTION_INFO_0_SK NULL
#define OPTION_VAL_320X200_SK NULL
#define OPTION_VAL_640X400_SK NULL
#define OPTION_VAL_960X600_SK NULL
#define OPTION_VAL_1280X800_SK NULL
#define OPTION_VAL_1600X1000_SK NULL
#define OPTION_VAL_1920X1200_SK NULL
#define OPTION_VAL_2240X1400_SK NULL
#define OPTION_VAL_2560X1600_SK NULL
#define PRBOOM_MOUSE_ON_LABEL_SK NULL
#define PRBOOM_MOUSE_ON_INFO_0_SK NULL
#define PRBOOM_FIND_RECURSIVE_ON_LABEL_SK NULL
#define PRBOOM_FIND_RECURSIVE_ON_INFO_0_SK NULL
#define PRBOOM_RUMBLE_LABEL_SK NULL
#define PRBOOM_RUMBLE_INFO_0_SK NULL
#define PRBOOM_ANALOG_DEADZONE_LABEL_SK NULL
#define PRBOOM_ANALOG_DEADZONE_INFO_0_SK NULL
#define PRBOOM_PURGE_LIMIT_LABEL_SK NULL
#define PRBOOM_PURGE_LIMIT_INFO_0_SK NULL
#define OPTION_VAL_8_SK NULL
#define OPTION_VAL_12_SK NULL
#define OPTION_VAL_16_SK NULL
#define OPTION_VAL_24_SK NULL
#define OPTION_VAL_32_SK NULL
#define OPTION_VAL_48_SK NULL
#define OPTION_VAL_64_SK NULL
#define OPTION_VAL_128_SK NULL
#define OPTION_VAL_256_SK NULL

struct retro_core_option_v2_category option_cats_sk[] = {
   { NULL, NULL, NULL },
};
struct retro_core_option_v2_definition option_defs_sk[] = {
   {
      "prboom-resolution",
      PRBOOM_RESOLUTION_LABEL_SK,
      NULL,
      PRBOOM_RESOLUTION_INFO_0_SK,
      NULL,
      NULL,
      {
         { "320x200",   OPTION_VAL_320X200_SK },
         { "640x400",   OPTION_VAL_640X400_SK },
         { "960x600",   OPTION_VAL_960X600_SK },
         { "1280x800",  OPTION_VAL_1280X800_SK },
         { "1600x1000", OPTION_VAL_1600X1000_SK },
         { "1920x1200", OPTION_VAL_1920X1200_SK },
         { "2240x1400", OPTION_VAL_2240X1400_SK },
         { "2560x1600", OPTION_VAL_2560X1600_SK },
         { NULL, NULL },
      },
      "320x200"
   },
   {
      "prboom-mouse_on",
      PRBOOM_MOUSE_ON_LABEL_SK,
      NULL,
      PRBOOM_MOUSE_ON_INFO_0_SK,
      NULL,
      NULL,
      {
         { "disabled", NULL },
         { "enabled",  NULL },
         { NULL, NULL },
      },
      "disabled"
   },
   {
      "prboom-find_recursive_on",
      PRBOOM_FIND_RECURSIVE_ON_LABEL_SK,
      NULL,
      PRBOOM_FIND_RECURSIVE_ON_INFO_0_SK,
      NULL,
      NULL,
      {
         { "disabled", NULL },
         { "enabled",  NULL },
         { NULL, NULL },
      },
      "enabled"
   },
   {
      "prboom-rumble",
      PRBOOM_RUMBLE_LABEL_SK,
      NULL,
      PRBOOM_RUMBLE_INFO_0_SK,
      NULL,
      NULL,
      {
         { "disabled", NULL },
         { "enabled",  NULL },
         { NULL, NULL },
      },
      "disabled"
   },
   {
      "prboom-analog_deadzone",
      PRBOOM_ANALOG_DEADZONE_LABEL_SK,
      NULL,
      PRBOOM_ANALOG_DEADZONE_INFO_0_SK,
      NULL,
      NULL,
      {
         { "0",  NULL },
         { "5",  NULL },
         { "10", NULL },
         { "15", NULL },
         { "20", NULL },
         { "25", NULL },
         { "30", NULL },
         { NULL, NULL },
      },
      "15"
   },
#if defined(MEMORY_LOW)
   {
      "prboom-purge_limit",
      PRBOOM_PURGE_LIMIT_LABEL_SK,
      NULL,
      PRBOOM_PURGE_LIMIT_INFO_0_SK,
      NULL,
      NULL,
      {
         { "8",   OPTION_VAL_8_SK },
         { "12",  OPTION_VAL_12_SK },
         { "16",  OPTION_VAL_16_SK },
         { "24",  OPTION_VAL_24_SK },
         { "32",  OPTION_VAL_32_SK },
         { "48",  OPTION_VAL_48_SK },
         { "64",  OPTION_VAL_64_SK },
         { "128", OPTION_VAL_128_SK },
         { "256", OPTION_VAL_256_SK },
         { NULL, NULL },
      },
      "16"
   },
#endif
   { NULL, NULL, NULL, NULL, NULL, NULL, {{0}}, NULL },
};
struct retro_core_options_v2 options_sk = {
   option_cats_sk,
   option_defs_sk
};

/* RETRO_LANGUAGE_SR */

#define PRBOOM_RESOLUTION_LABEL_SR NULL
#define PRBOOM_RESOLUTION_INFO_0_SR NULL
#define OPTION_VAL_320X200_SR NULL
#define OPTION_VAL_640X400_SR NULL
#define OPTION_VAL_960X600_SR NULL
#define OPTION_VAL_1280X800_SR NULL
#define OPTION_VAL_1600X1000_SR NULL
#define OPTION_VAL_1920X1200_SR NULL
#define OPTION_VAL_2240X1400_SR NULL
#define OPTION_VAL_2560X1600_SR NULL
#define PRBOOM_MOUSE_ON_LABEL_SR NULL
#define PRBOOM_MOUSE_ON_INFO_0_SR NULL
#define PRBOOM_FIND_RECURSIVE_ON_LABEL_SR NULL
#define PRBOOM_FIND_RECURSIVE_ON_INFO_0_SR NULL
#define PRBOOM_RUMBLE_LABEL_SR NULL
#define PRBOOM_RUMBLE_INFO_0_SR NULL
#define PRBOOM_ANALOG_DEADZONE_LABEL_SR NULL
#define PRBOOM_ANALOG_DEADZONE_INFO_0_SR NULL
#define PRBOOM_PURGE_LIMIT_LABEL_SR NULL
#define PRBOOM_PURGE_LIMIT_INFO_0_SR NULL
#define OPTION_VAL_8_SR NULL
#define OPTION_VAL_12_SR NULL
#define OPTION_VAL_16_SR NULL
#define OPTION_VAL_24_SR NULL
#define OPTION_VAL_32_SR NULL
#define OPTION_VAL_48_SR NULL
#define OPTION_VAL_64_SR NULL
#define OPTION_VAL_128_SR NULL
#define OPTION_VAL_256_SR NULL

struct retro_core_option_v2_category option_cats_sr[] = {
   { NULL, NULL, NULL },
};
struct retro_core_option_v2_definition option_defs_sr[] = {
   {
      "prboom-resolution",
      PRBOOM_RESOLUTION_LABEL_SR,
      NULL,
      PRBOOM_RESOLUTION_INFO_0_SR,
      NULL,
      NULL,
      {
         { "320x200",   OPTION_VAL_320X200_SR },
         { "640x400",   OPTION_VAL_640X400_SR },
         { "960x600",   OPTION_VAL_960X600_SR },
         { "1280x800",  OPTION_VAL_1280X800_SR },
         { "1600x1000", OPTION_VAL_1600X1000_SR },
         { "1920x1200", OPTION_VAL_1920X1200_SR },
         { "2240x1400", OPTION_VAL_2240X1400_SR },
         { "2560x1600", OPTION_VAL_2560X1600_SR },
         { NULL, NULL },
      },
      "320x200"
   },
   {
      "prboom-mouse_on",
      PRBOOM_MOUSE_ON_LABEL_SR,
      NULL,
      PRBOOM_MOUSE_ON_INFO_0_SR,
      NULL,
      NULL,
      {
         { "disabled", NULL },
         { "enabled",  NULL },
         { NULL, NULL },
      },
      "disabled"
   },
   {
      "prboom-find_recursive_on",
      PRBOOM_FIND_RECURSIVE_ON_LABEL_SR,
      NULL,
      PRBOOM_FIND_RECURSIVE_ON_INFO_0_SR,
      NULL,
      NULL,
      {
         { "disabled", NULL },
         { "enabled",  NULL },
         { NULL, NULL },
      },
      "enabled"
   },
   {
      "prboom-rumble",
      PRBOOM_RUMBLE_LABEL_SR,
      NULL,
      PRBOOM_RUMBLE_INFO_0_SR,
      NULL,
      NULL,
      {
         { "disabled", NULL },
         { "enabled",  NULL },
         { NULL, NULL },
      },
      "disabled"
   },
   {
      "prboom-analog_deadzone",
      PRBOOM_ANALOG_DEADZONE_LABEL_SR,
      NULL,
      PRBOOM_ANALOG_DEADZONE_INFO_0_SR,
      NULL,
      NULL,
      {
         { "0",  NULL },
         { "5",  NULL },
         { "10", NULL },
         { "15", NULL },
         { "20", NULL },
         { "25", NULL },
         { "30", NULL },
         { NULL, NULL },
      },
      "15"
   },
#if defined(MEMORY_LOW)
   {
      "prboom-purge_limit",
      PRBOOM_PURGE_LIMIT_LABEL_SR,
      NULL,
      PRBOOM_PURGE_LIMIT_INFO_0_SR,
      NULL,
      NULL,
      {
         { "8",   OPTION_VAL_8_SR },
         { "12",  OPTION_VAL_12_SR },
         { "16",  OPTION_VAL_16_SR },
         { "24",  OPTION_VAL_24_SR },
         { "32",  OPTION_VAL_32_SR },
         { "48",  OPTION_VAL_48_SR },
         { "64",  OPTION_VAL_64_SR },
         { "128", OPTION_VAL_128_SR },
         { "256", OPTION_VAL_256_SR },
         { NULL, NULL },
      },
      "16"
   },
#endif
   { NULL, NULL, NULL, NULL, NULL, NULL, {{0}}, NULL },
};
struct retro_core_options_v2 options_sr = {
   option_cats_sr,
   option_defs_sr
};

/* RETRO_LANGUAGE_SV */

#define PRBOOM_RESOLUTION_LABEL_SV NULL
#define PRBOOM_RESOLUTION_INFO_0_SV NULL
#define OPTION_VAL_320X200_SV NULL
#define OPTION_VAL_640X400_SV NULL
#define OPTION_VAL_960X600_SV NULL
#define OPTION_VAL_1280X800_SV NULL
#define OPTION_VAL_1600X1000_SV NULL
#define OPTION_VAL_1920X1200_SV NULL
#define OPTION_VAL_2240X1400_SV NULL
#define OPTION_VAL_2560X1600_SV NULL
#define PRBOOM_MOUSE_ON_LABEL_SV NULL
#define PRBOOM_MOUSE_ON_INFO_0_SV NULL
#define PRBOOM_FIND_RECURSIVE_ON_LABEL_SV NULL
#define PRBOOM_FIND_RECURSIVE_ON_INFO_0_SV NULL
#define PRBOOM_RUMBLE_LABEL_SV NULL
#define PRBOOM_RUMBLE_INFO_0_SV NULL
#define PRBOOM_ANALOG_DEADZONE_LABEL_SV NULL
#define PRBOOM_ANALOG_DEADZONE_INFO_0_SV NULL
#define PRBOOM_PURGE_LIMIT_LABEL_SV NULL
#define PRBOOM_PURGE_LIMIT_INFO_0_SV NULL
#define OPTION_VAL_8_SV NULL
#define OPTION_VAL_12_SV NULL
#define OPTION_VAL_16_SV NULL
#define OPTION_VAL_24_SV NULL
#define OPTION_VAL_32_SV NULL
#define OPTION_VAL_48_SV NULL
#define OPTION_VAL_64_SV NULL
#define OPTION_VAL_128_SV NULL
#define OPTION_VAL_256_SV NULL

struct retro_core_option_v2_category option_cats_sv[] = {
   { NULL, NULL, NULL },
};
struct retro_core_option_v2_definition option_defs_sv[] = {
   {
      "prboom-resolution",
      PRBOOM_RESOLUTION_LABEL_SV,
      NULL,
      PRBOOM_RESOLUTION_INFO_0_SV,
      NULL,
      NULL,
      {
         { "320x200",   OPTION_VAL_320X200_SV },
         { "640x400",   OPTION_VAL_640X400_SV },
         { "960x600",   OPTION_VAL_960X600_SV },
         { "1280x800",  OPTION_VAL_1280X800_SV },
         { "1600x1000", OPTION_VAL_1600X1000_SV },
         { "1920x1200", OPTION_VAL_1920X1200_SV },
         { "2240x1400", OPTION_VAL_2240X1400_SV },
         { "2560x1600", OPTION_VAL_2560X1600_SV },
         { NULL, NULL },
      },
      "320x200"
   },
   {
      "prboom-mouse_on",
      PRBOOM_MOUSE_ON_LABEL_SV,
      NULL,
      PRBOOM_MOUSE_ON_INFO_0_SV,
      NULL,
      NULL,
      {
         { "disabled", NULL },
         { "enabled",  NULL },
         { NULL, NULL },
      },
      "disabled"
   },
   {
      "prboom-find_recursive_on",
      PRBOOM_FIND_RECURSIVE_ON_LABEL_SV,
      NULL,
      PRBOOM_FIND_RECURSIVE_ON_INFO_0_SV,
      NULL,
      NULL,
      {
         { "disabled", NULL },
         { "enabled",  NULL },
         { NULL, NULL },
      },
      "enabled"
   },
   {
      "prboom-rumble",
      PRBOOM_RUMBLE_LABEL_SV,
      NULL,
      PRBOOM_RUMBLE_INFO_0_SV,
      NULL,
      NULL,
      {
         { "disabled", NULL },
         { "enabled",  NULL },
         { NULL, NULL },
      },
      "disabled"
   },
   {
      "prboom-analog_deadzone",
      PRBOOM_ANALOG_DEADZONE_LABEL_SV,
      NULL,
      PRBOOM_ANALOG_DEADZONE_INFO_0_SV,
      NULL,
      NULL,
      {
         { "0",  NULL },
         { "5",  NULL },
         { "10", NULL },
         { "15", NULL },
         { "20", NULL },
         { "25", NULL },
         { "30", NULL },
         { NULL, NULL },
      },
      "15"
   },
#if defined(MEMORY_LOW)
   {
      "prboom-purge_limit",
      PRBOOM_PURGE_LIMIT_LABEL_SV,
      NULL,
      PRBOOM_PURGE_LIMIT_INFO_0_SV,
      NULL,
      NULL,
      {
         { "8",   OPTION_VAL_8_SV },
         { "12",  OPTION_VAL_12_SV },
         { "16",  OPTION_VAL_16_SV },
         { "24",  OPTION_VAL_24_SV },
         { "32",  OPTION_VAL_32_SV },
         { "48",  OPTION_VAL_48_SV },
         { "64",  OPTION_VAL_64_SV },
         { "128", OPTION_VAL_128_SV },
         { "256", OPTION_VAL_256_SV },
         { NULL, NULL },
      },
      "16"
   },
#endif
   { NULL, NULL, NULL, NULL, NULL, NULL, {{0}}, NULL },
};
struct retro_core_options_v2 options_sv = {
   option_cats_sv,
   option_defs_sv
};

/* RETRO_LANGUAGE_TR */

#define PRBOOM_RESOLUTION_LABEL_TR "Dahili çözünürlük (Yeniden Başlatılmalı)"
#define PRBOOM_RESOLUTION_INFO_0_TR "Çözünürlüğü yapılandırın."
#define OPTION_VAL_320X200_TR NULL
#define OPTION_VAL_640X400_TR NULL
#define OPTION_VAL_960X600_TR NULL
#define OPTION_VAL_1280X800_TR NULL
#define OPTION_VAL_1600X1000_TR NULL
#define OPTION_VAL_1920X1200_TR NULL
#define OPTION_VAL_2240X1400_TR NULL
#define OPTION_VAL_2560X1600_TR NULL
#define PRBOOM_MOUSE_ON_LABEL_TR "Oyun Kolu Kullanılırken Fare Etkin"
#define PRBOOM_MOUSE_ON_INFO_0_TR "1. Kullanıcı cihaz türü 'RetroKeyboard/Fare' olarak ayarlanmadığında bile fare girişlerini kullanmanıza izin verir."
#define PRBOOM_FIND_RECURSIVE_ON_LABEL_TR "IWAD'ler için Ana Klasörlere Bakın"
#define PRBOOM_FIND_RECURSIVE_ON_INFO_0_TR "IWAD'ler için ana klasörleri tarar. NOT: SIGIL'i çalıştırmak istiyorsanız bunu devre dışı bırakmanız gerekir."
#define PRBOOM_RUMBLE_LABEL_TR "Titreşim Efektleri"
#define PRBOOM_RUMBLE_INFO_0_TR "Titreşim donanımlı bir oyun kolu kullanırken dokunsal geri bildirim sağlar."
#define PRBOOM_ANALOG_DEADZONE_LABEL_TR "Analog Ölü Bölge (Yüzde)"
#define PRBOOM_ANALOG_DEADZONE_INFO_0_TR "Giriş cihazı türü 'Modern Oyun Kolu' olarak ayarlandığında oyun kumandası analog çubuklarının ölü bölgesini ayarlar."
#define PRBOOM_PURGE_LIMIT_LABEL_TR "Önbellek Boyutu"
#define PRBOOM_PURGE_LIMIT_INFO_0_TR "Oyun içeriklerini önbelleğe almak için kullanılan bellek havuzunun boyutuna bir sınır ayarlar. Küçük değerler, büyük haritalarda gezinirken takılmalara neden olabilir."
#define OPTION_VAL_8_TR NULL
#define OPTION_VAL_12_TR NULL
#define OPTION_VAL_16_TR NULL
#define OPTION_VAL_24_TR NULL
#define OPTION_VAL_32_TR NULL
#define OPTION_VAL_48_TR NULL
#define OPTION_VAL_64_TR NULL
#define OPTION_VAL_128_TR NULL
#define OPTION_VAL_256_TR NULL

struct retro_core_option_v2_category option_cats_tr[] = {
   { NULL, NULL, NULL },
};
struct retro_core_option_v2_definition option_defs_tr[] = {
   {
      "prboom-resolution",
      PRBOOM_RESOLUTION_LABEL_TR,
      NULL,
      PRBOOM_RESOLUTION_INFO_0_TR,
      NULL,
      NULL,
      {
         { "320x200",   OPTION_VAL_320X200_TR },
         { "640x400",   OPTION_VAL_640X400_TR },
         { "960x600",   OPTION_VAL_960X600_TR },
         { "1280x800",  OPTION_VAL_1280X800_TR },
         { "1600x1000", OPTION_VAL_1600X1000_TR },
         { "1920x1200", OPTION_VAL_1920X1200_TR },
         { "2240x1400", OPTION_VAL_2240X1400_TR },
         { "2560x1600", OPTION_VAL_2560X1600_TR },
         { NULL, NULL },
      },
      "320x200"
   },
   {
      "prboom-mouse_on",
      PRBOOM_MOUSE_ON_LABEL_TR,
      NULL,
      PRBOOM_MOUSE_ON_INFO_0_TR,
      NULL,
      NULL,
      {
         { "disabled", NULL },
         { "enabled",  NULL },
         { NULL, NULL },
      },
      "disabled"
   },
   {
      "prboom-find_recursive_on",
      PRBOOM_FIND_RECURSIVE_ON_LABEL_TR,
      NULL,
      PRBOOM_FIND_RECURSIVE_ON_INFO_0_TR,
      NULL,
      NULL,
      {
         { "disabled", NULL },
         { "enabled",  NULL },
         { NULL, NULL },
      },
      "enabled"
   },
   {
      "prboom-rumble",
      PRBOOM_RUMBLE_LABEL_TR,
      NULL,
      PRBOOM_RUMBLE_INFO_0_TR,
      NULL,
      NULL,
      {
         { "disabled", NULL },
         { "enabled",  NULL },
         { NULL, NULL },
      },
      "disabled"
   },
   {
      "prboom-analog_deadzone",
      PRBOOM_ANALOG_DEADZONE_LABEL_TR,
      NULL,
      PRBOOM_ANALOG_DEADZONE_INFO_0_TR,
      NULL,
      NULL,
      {
         { "0",  NULL },
         { "5",  NULL },
         { "10", NULL },
         { "15", NULL },
         { "20", NULL },
         { "25", NULL },
         { "30", NULL },
         { NULL, NULL },
      },
      "15"
   },
#if defined(MEMORY_LOW)
   {
      "prboom-purge_limit",
      PRBOOM_PURGE_LIMIT_LABEL_TR,
      NULL,
      PRBOOM_PURGE_LIMIT_INFO_0_TR,
      NULL,
      NULL,
      {
         { "8",   OPTION_VAL_8_TR },
         { "12",  OPTION_VAL_12_TR },
         { "16",  OPTION_VAL_16_TR },
         { "24",  OPTION_VAL_24_TR },
         { "32",  OPTION_VAL_32_TR },
         { "48",  OPTION_VAL_48_TR },
         { "64",  OPTION_VAL_64_TR },
         { "128", OPTION_VAL_128_TR },
         { "256", OPTION_VAL_256_TR },
         { NULL, NULL },
      },
      "16"
   },
#endif
   { NULL, NULL, NULL, NULL, NULL, NULL, {{0}}, NULL },
};
struct retro_core_options_v2 options_tr = {
   option_cats_tr,
   option_defs_tr
};

/* RETRO_LANGUAGE_UK */

#define PRBOOM_RESOLUTION_LABEL_UK NULL
#define PRBOOM_RESOLUTION_INFO_0_UK NULL
#define OPTION_VAL_320X200_UK NULL
#define OPTION_VAL_640X400_UK NULL
#define OPTION_VAL_960X600_UK NULL
#define OPTION_VAL_1280X800_UK NULL
#define OPTION_VAL_1600X1000_UK NULL
#define OPTION_VAL_1920X1200_UK NULL
#define OPTION_VAL_2240X1400_UK NULL
#define OPTION_VAL_2560X1600_UK NULL
#define PRBOOM_MOUSE_ON_LABEL_UK NULL
#define PRBOOM_MOUSE_ON_INFO_0_UK NULL
#define PRBOOM_FIND_RECURSIVE_ON_LABEL_UK NULL
#define PRBOOM_FIND_RECURSIVE_ON_INFO_0_UK NULL
#define PRBOOM_RUMBLE_LABEL_UK NULL
#define PRBOOM_RUMBLE_INFO_0_UK NULL
#define PRBOOM_ANALOG_DEADZONE_LABEL_UK NULL
#define PRBOOM_ANALOG_DEADZONE_INFO_0_UK NULL
#define PRBOOM_PURGE_LIMIT_LABEL_UK NULL
#define PRBOOM_PURGE_LIMIT_INFO_0_UK NULL
#define OPTION_VAL_8_UK NULL
#define OPTION_VAL_12_UK NULL
#define OPTION_VAL_16_UK NULL
#define OPTION_VAL_24_UK NULL
#define OPTION_VAL_32_UK NULL
#define OPTION_VAL_48_UK NULL
#define OPTION_VAL_64_UK NULL
#define OPTION_VAL_128_UK NULL
#define OPTION_VAL_256_UK NULL

struct retro_core_option_v2_category option_cats_uk[] = {
   { NULL, NULL, NULL },
};
struct retro_core_option_v2_definition option_defs_uk[] = {
   {
      "prboom-resolution",
      PRBOOM_RESOLUTION_LABEL_UK,
      NULL,
      PRBOOM_RESOLUTION_INFO_0_UK,
      NULL,
      NULL,
      {
         { "320x200",   OPTION_VAL_320X200_UK },
         { "640x400",   OPTION_VAL_640X400_UK },
         { "960x600",   OPTION_VAL_960X600_UK },
         { "1280x800",  OPTION_VAL_1280X800_UK },
         { "1600x1000", OPTION_VAL_1600X1000_UK },
         { "1920x1200", OPTION_VAL_1920X1200_UK },
         { "2240x1400", OPTION_VAL_2240X1400_UK },
         { "2560x1600", OPTION_VAL_2560X1600_UK },
         { NULL, NULL },
      },
      "320x200"
   },
   {
      "prboom-mouse_on",
      PRBOOM_MOUSE_ON_LABEL_UK,
      NULL,
      PRBOOM_MOUSE_ON_INFO_0_UK,
      NULL,
      NULL,
      {
         { "disabled", NULL },
         { "enabled",  NULL },
         { NULL, NULL },
      },
      "disabled"
   },
   {
      "prboom-find_recursive_on",
      PRBOOM_FIND_RECURSIVE_ON_LABEL_UK,
      NULL,
      PRBOOM_FIND_RECURSIVE_ON_INFO_0_UK,
      NULL,
      NULL,
      {
         { "disabled", NULL },
         { "enabled",  NULL },
         { NULL, NULL },
      },
      "enabled"
   },
   {
      "prboom-rumble",
      PRBOOM_RUMBLE_LABEL_UK,
      NULL,
      PRBOOM_RUMBLE_INFO_0_UK,
      NULL,
      NULL,
      {
         { "disabled", NULL },
         { "enabled",  NULL },
         { NULL, NULL },
      },
      "disabled"
   },
   {
      "prboom-analog_deadzone",
      PRBOOM_ANALOG_DEADZONE_LABEL_UK,
      NULL,
      PRBOOM_ANALOG_DEADZONE_INFO_0_UK,
      NULL,
      NULL,
      {
         { "0",  NULL },
         { "5",  NULL },
         { "10", NULL },
         { "15", NULL },
         { "20", NULL },
         { "25", NULL },
         { "30", NULL },
         { NULL, NULL },
      },
      "15"
   },
#if defined(MEMORY_LOW)
   {
      "prboom-purge_limit",
      PRBOOM_PURGE_LIMIT_LABEL_UK,
      NULL,
      PRBOOM_PURGE_LIMIT_INFO_0_UK,
      NULL,
      NULL,
      {
         { "8",   OPTION_VAL_8_UK },
         { "12",  OPTION_VAL_12_UK },
         { "16",  OPTION_VAL_16_UK },
         { "24",  OPTION_VAL_24_UK },
         { "32",  OPTION_VAL_32_UK },
         { "48",  OPTION_VAL_48_UK },
         { "64",  OPTION_VAL_64_UK },
         { "128", OPTION_VAL_128_UK },
         { "256", OPTION_VAL_256_UK },
         { NULL, NULL },
      },
      "16"
   },
#endif
   { NULL, NULL, NULL, NULL, NULL, NULL, {{0}}, NULL },
};
struct retro_core_options_v2 options_uk = {
   option_cats_uk,
   option_defs_uk
};

/* RETRO_LANGUAGE_VAL */

#define PRBOOM_RESOLUTION_LABEL_VAL NULL
#define PRBOOM_RESOLUTION_INFO_0_VAL NULL
#define OPTION_VAL_320X200_VAL NULL
#define OPTION_VAL_640X400_VAL NULL
#define OPTION_VAL_960X600_VAL NULL
#define OPTION_VAL_1280X800_VAL NULL
#define OPTION_VAL_1600X1000_VAL NULL
#define OPTION_VAL_1920X1200_VAL NULL
#define OPTION_VAL_2240X1400_VAL NULL
#define OPTION_VAL_2560X1600_VAL NULL
#define PRBOOM_MOUSE_ON_LABEL_VAL NULL
#define PRBOOM_MOUSE_ON_INFO_0_VAL NULL
#define PRBOOM_FIND_RECURSIVE_ON_LABEL_VAL NULL
#define PRBOOM_FIND_RECURSIVE_ON_INFO_0_VAL NULL
#define PRBOOM_RUMBLE_LABEL_VAL NULL
#define PRBOOM_RUMBLE_INFO_0_VAL NULL
#define PRBOOM_ANALOG_DEADZONE_LABEL_VAL NULL
#define PRBOOM_ANALOG_DEADZONE_INFO_0_VAL NULL
#define PRBOOM_PURGE_LIMIT_LABEL_VAL NULL
#define PRBOOM_PURGE_LIMIT_INFO_0_VAL NULL
#define OPTION_VAL_8_VAL NULL
#define OPTION_VAL_12_VAL NULL
#define OPTION_VAL_16_VAL NULL
#define OPTION_VAL_24_VAL NULL
#define OPTION_VAL_32_VAL NULL
#define OPTION_VAL_48_VAL NULL
#define OPTION_VAL_64_VAL NULL
#define OPTION_VAL_128_VAL NULL
#define OPTION_VAL_256_VAL NULL

struct retro_core_option_v2_category option_cats_val[] = {
   { NULL, NULL, NULL },
};
struct retro_core_option_v2_definition option_defs_val[] = {
   {
      "prboom-resolution",
      PRBOOM_RESOLUTION_LABEL_VAL,
      NULL,
      PRBOOM_RESOLUTION_INFO_0_VAL,
      NULL,
      NULL,
      {
         { "320x200",   OPTION_VAL_320X200_VAL },
         { "640x400",   OPTION_VAL_640X400_VAL },
         { "960x600",   OPTION_VAL_960X600_VAL },
         { "1280x800",  OPTION_VAL_1280X800_VAL },
         { "1600x1000", OPTION_VAL_1600X1000_VAL },
         { "1920x1200", OPTION_VAL_1920X1200_VAL },
         { "2240x1400", OPTION_VAL_2240X1400_VAL },
         { "2560x1600", OPTION_VAL_2560X1600_VAL },
         { NULL, NULL },
      },
      "320x200"
   },
   {
      "prboom-mouse_on",
      PRBOOM_MOUSE_ON_LABEL_VAL,
      NULL,
      PRBOOM_MOUSE_ON_INFO_0_VAL,
      NULL,
      NULL,
      {
         { "disabled", NULL },
         { "enabled",  NULL },
         { NULL, NULL },
      },
      "disabled"
   },
   {
      "prboom-find_recursive_on",
      PRBOOM_FIND_RECURSIVE_ON_LABEL_VAL,
      NULL,
      PRBOOM_FIND_RECURSIVE_ON_INFO_0_VAL,
      NULL,
      NULL,
      {
         { "disabled", NULL },
         { "enabled",  NULL },
         { NULL, NULL },
      },
      "enabled"
   },
   {
      "prboom-rumble",
      PRBOOM_RUMBLE_LABEL_VAL,
      NULL,
      PRBOOM_RUMBLE_INFO_0_VAL,
      NULL,
      NULL,
      {
         { "disabled", NULL },
         { "enabled",  NULL },
         { NULL, NULL },
      },
      "disabled"
   },
   {
      "prboom-analog_deadzone",
      PRBOOM_ANALOG_DEADZONE_LABEL_VAL,
      NULL,
      PRBOOM_ANALOG_DEADZONE_INFO_0_VAL,
      NULL,
      NULL,
      {
         { "0",  NULL },
         { "5",  NULL },
         { "10", NULL },
         { "15", NULL },
         { "20", NULL },
         { "25", NULL },
         { "30", NULL },
         { NULL, NULL },
      },
      "15"
   },
#if defined(MEMORY_LOW)
   {
      "prboom-purge_limit",
      PRBOOM_PURGE_LIMIT_LABEL_VAL,
      NULL,
      PRBOOM_PURGE_LIMIT_INFO_0_VAL,
      NULL,
      NULL,
      {
         { "8",   OPTION_VAL_8_VAL },
         { "12",  OPTION_VAL_12_VAL },
         { "16",  OPTION_VAL_16_VAL },
         { "24",  OPTION_VAL_24_VAL },
         { "32",  OPTION_VAL_32_VAL },
         { "48",  OPTION_VAL_48_VAL },
         { "64",  OPTION_VAL_64_VAL },
         { "128", OPTION_VAL_128_VAL },
         { "256", OPTION_VAL_256_VAL },
         { NULL, NULL },
      },
      "16"
   },
#endif
   { NULL, NULL, NULL, NULL, NULL, NULL, {{0}}, NULL },
};
struct retro_core_options_v2 options_val = {
   option_cats_val,
   option_defs_val
};

/* RETRO_LANGUAGE_VN */

#define PRBOOM_RESOLUTION_LABEL_VN NULL
#define PRBOOM_RESOLUTION_INFO_0_VN NULL
#define OPTION_VAL_320X200_VN NULL
#define OPTION_VAL_640X400_VN NULL
#define OPTION_VAL_960X600_VN NULL
#define OPTION_VAL_1280X800_VN NULL
#define OPTION_VAL_1600X1000_VN NULL
#define OPTION_VAL_1920X1200_VN NULL
#define OPTION_VAL_2240X1400_VN NULL
#define OPTION_VAL_2560X1600_VN NULL
#define PRBOOM_MOUSE_ON_LABEL_VN NULL
#define PRBOOM_MOUSE_ON_INFO_0_VN NULL
#define PRBOOM_FIND_RECURSIVE_ON_LABEL_VN NULL
#define PRBOOM_FIND_RECURSIVE_ON_INFO_0_VN NULL
#define PRBOOM_RUMBLE_LABEL_VN NULL
#define PRBOOM_RUMBLE_INFO_0_VN NULL
#define PRBOOM_ANALOG_DEADZONE_LABEL_VN NULL
#define PRBOOM_ANALOG_DEADZONE_INFO_0_VN NULL
#define PRBOOM_PURGE_LIMIT_LABEL_VN NULL
#define PRBOOM_PURGE_LIMIT_INFO_0_VN NULL
#define OPTION_VAL_8_VN NULL
#define OPTION_VAL_12_VN NULL
#define OPTION_VAL_16_VN NULL
#define OPTION_VAL_24_VN NULL
#define OPTION_VAL_32_VN NULL
#define OPTION_VAL_48_VN NULL
#define OPTION_VAL_64_VN NULL
#define OPTION_VAL_128_VN NULL
#define OPTION_VAL_256_VN NULL

struct retro_core_option_v2_category option_cats_vn[] = {
   { NULL, NULL, NULL },
};
struct retro_core_option_v2_definition option_defs_vn[] = {
   {
      "prboom-resolution",
      PRBOOM_RESOLUTION_LABEL_VN,
      NULL,
      PRBOOM_RESOLUTION_INFO_0_VN,
      NULL,
      NULL,
      {
         { "320x200",   OPTION_VAL_320X200_VN },
         { "640x400",   OPTION_VAL_640X400_VN },
         { "960x600",   OPTION_VAL_960X600_VN },
         { "1280x800",  OPTION_VAL_1280X800_VN },
         { "1600x1000", OPTION_VAL_1600X1000_VN },
         { "1920x1200", OPTION_VAL_1920X1200_VN },
         { "2240x1400", OPTION_VAL_2240X1400_VN },
         { "2560x1600", OPTION_VAL_2560X1600_VN },
         { NULL, NULL },
      },
      "320x200"
   },
   {
      "prboom-mouse_on",
      PRBOOM_MOUSE_ON_LABEL_VN,
      NULL,
      PRBOOM_MOUSE_ON_INFO_0_VN,
      NULL,
      NULL,
      {
         { "disabled", NULL },
         { "enabled",  NULL },
         { NULL, NULL },
      },
      "disabled"
   },
   {
      "prboom-find_recursive_on",
      PRBOOM_FIND_RECURSIVE_ON_LABEL_VN,
      NULL,
      PRBOOM_FIND_RECURSIVE_ON_INFO_0_VN,
      NULL,
      NULL,
      {
         { "disabled", NULL },
         { "enabled",  NULL },
         { NULL, NULL },
      },
      "enabled"
   },
   {
      "prboom-rumble",
      PRBOOM_RUMBLE_LABEL_VN,
      NULL,
      PRBOOM_RUMBLE_INFO_0_VN,
      NULL,
      NULL,
      {
         { "disabled", NULL },
         { "enabled",  NULL },
         { NULL, NULL },
      },
      "disabled"
   },
   {
      "prboom-analog_deadzone",
      PRBOOM_ANALOG_DEADZONE_LABEL_VN,
      NULL,
      PRBOOM_ANALOG_DEADZONE_INFO_0_VN,
      NULL,
      NULL,
      {
         { "0",  NULL },
         { "5",  NULL },
         { "10", NULL },
         { "15", NULL },
         { "20", NULL },
         { "25", NULL },
         { "30", NULL },
         { NULL, NULL },
      },
      "15"
   },
#if defined(MEMORY_LOW)
   {
      "prboom-purge_limit",
      PRBOOM_PURGE_LIMIT_LABEL_VN,
      NULL,
      PRBOOM_PURGE_LIMIT_INFO_0_VN,
      NULL,
      NULL,
      {
         { "8",   OPTION_VAL_8_VN },
         { "12",  OPTION_VAL_12_VN },
         { "16",  OPTION_VAL_16_VN },
         { "24",  OPTION_VAL_24_VN },
         { "32",  OPTION_VAL_32_VN },
         { "48",  OPTION_VAL_48_VN },
         { "64",  OPTION_VAL_64_VN },
         { "128", OPTION_VAL_128_VN },
         { "256", OPTION_VAL_256_VN },
         { NULL, NULL },
      },
      "16"
   },
#endif
   { NULL, NULL, NULL, NULL, NULL, NULL, {{0}}, NULL },
};
struct retro_core_options_v2 options_vn = {
   option_cats_vn,
   option_defs_vn
};


#ifdef __cplusplus
}
#endif

#endif