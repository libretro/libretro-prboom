#ifndef LIBRETRO_CORE_OPTIONS_H__
#define LIBRETRO_CORE_OPTIONS_H__

#include <stdlib.h>
#include <string.h>

#include <libretro.h>
#include <retro_inline.h>

#ifndef HAVE_NO_LANGEXTRA
#include "libretro_core_options_intl.h"
#endif

#ifdef __cplusplus
extern "C" {
#endif

/*
 ********************************
 * Core Option Definitions
 ********************************
*/

/* RETRO_LANGUAGE_ENGLISH */

/* Default language:
 * - All other languages must include the same keys and values
 * - Will be used as a fallback in the event that frontend language
 *   is not available
 * - Will be used as a fallback for any missing entries in
 *   frontend language definition */


struct retro_core_option_v2_category option_cats_us[] = {
   {
      "audio",
      "Audio",
      "Configure audio output, including the synthesis/output sample rate."
   },
   { NULL, NULL, NULL },
};

struct retro_core_option_v2_definition option_defs_us[] = {
   {
      "prboom-resolution",
      "Internal resolution (Restart Required)",
      NULL,
      "Configure the resolution.",
      NULL,
      NULL,
      {
         { "320x200",   NULL },
         { "640x400",   NULL },
         { "960x600",   NULL },
         { "1280x800",  NULL },
         { "1600x1000", NULL },
         { "1920x1200", NULL },
         { "2240x1400", NULL },
         { "2560x1600", NULL },
         { NULL, NULL },
      },
      "320x200"
   },
   {
      "prboom-color_format",
      "Color Format (Restart Required)",
      NULL,
      "Output colour depth. '16bits' is the classic RGB565 renderer. '24bits' renders in full 8-bit-per-channel truecolor, which removes the banding the 16-bit light ramp introduces in distance shading and smooth gradients. '30bits' renders at 10 bits per channel for finer gradients still; it is used only when the frontend can actually present a 10-bit surface, otherwise the core falls back to 24bits automatically.",
      NULL,
      NULL,
      {
         { "16bits",             NULL },
         { "24bits (truecolor)", NULL },
         { "30bits (HDR)",       NULL },
         { NULL, NULL },
      },
      "16bits"
   },
   {
      "prboom-diminished_lighting",
      "Diminished Lighting",
      NULL,
      "How distance light is applied. 'Classic' snaps it to the 32 discrete colormaps the DOS engine used, giving the original banded look. 'Smooth' shades continuously, removing the band edges. 'Auto' uses Classic at 16-bit and Smooth at 24/30-bit, where the banded path would discard most of the extra colour precision.",
      NULL,
      NULL,
      {
         { "auto",    "Auto" },
         { "classic", "Classic (banded)" },
         { "smooth",  "Smooth" },
         { NULL, NULL },
      },
      "auto"
   },
   {
      "prboom-mouse_on",
      "Mouse Active When Using Gamepad",
      NULL,
      "Allows you to use mouse inputs even when User 1's device type isn't set to 'RetroKeyboard/Mouse'.",
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
      "prboom-wall_decals",
      "Wall Bullet Decals",
      NULL,
      "Stamp scuff marks on walls where hitscan shots land. Off by default, matching ZDoom mods that do not request bullet decals; enable for the classic wall-chip marks.",
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
      "prboom-dynlight_wall_falloff",
      "Dynamic Light Wall Falloff",
      NULL,
      "Give GLDEFS point lights a vertical light pool on walls (brightest at the light's height, fading up and down) instead of a single level per wall column. Off by default; the per-band shading is noticeably heavier on tall, close, heavily-lit walls.",
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
      "Look on Parent Folders for IWADs",
      NULL,
      "Scans parent folders for IWADs. NOTE: You need to disable this if you want to run SIGIL.",
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
      "Rumble Effects",
      NULL,
      "Enables haptic feedback when using a rumble-equipped gamepad.",
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
      "prboom-mmap_wads",
      "Memory-Map WAD Files (Restart Required)",
      NULL,
      "Load WAD data by memory-mapping the file instead of reading it fully into RAM. Lowers memory use and speeds up loading of large WADs, since only the lumps actually used are paged in. Falls back to a normal read for archives, if mapping fails, or on builds without memory-mapping support. Leave disabled to perform all WAD file I/O up front at load time.",
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
      "Analog Deadzone (Percent)",
      NULL,
      "Sets the deadzone of the gamepad analog sticks when the input device type is set to 'Gamepad Modern'.",
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
      "Cache Size",
      NULL,
      "Sets a limit on the size of the memory pool used to cache game assets. Small values may cause stuttering when navigating large maps.",
      NULL,
      NULL,
      {
         { "8",   "8 MB" },
         { "12",  "12 MB" },
         { "16",  "16 MB" },
         { "24",  "24 MB" },
         { "32",  "32 MB" },
         { "48",  "48 MB" },
         { "64",  "64 MB" },
         { "128", "128 MB" },
         { "256", "256 MB" },
         { NULL, NULL },
      },
      "16"
   },
#endif
   {
      "prboom-complevel",
      "Compatibility Level (Restart Required)",
      NULL,
      "Forces a demo-compatibility level. 'Default' uses the engine's automatic choice. Set 'MBF21' to play MBF21 WADs that require complevel 21.",
      NULL,
      NULL,
      {
         { "-1", "Default (Auto)" },
         { "4",  "Ultimate Doom" },
         { "5",  "Final Doom" },
         { "9",  "Boom" },
         { "11", "MBF" },
         { "17", "PrBoom latest" },
         { "18", "MBF21" },
         { NULL, NULL },
      },
      "-1"
   },
   {
      "prboom-sound_samplerate",
      "Sound Samplerate (Hint)",
      "Sound Samplerate (Hint)",
      "Audio output rate. prboom synthesizes MIDI and tracker music in real time, so it has no fixed native rate and can render directly at whichever rate you pick. Higher rates lower latency, push aliasing above the audible range, avoid the frontend resampler's low-pass smearing, and give the sound-effect and music-stream resamplers finer time resolution. 'Auto' queries the frontend's target rate and snaps to the nearest supported value.",
      "Audio output rate. prboom has no fixed native rate (music is synthesized in real time), so it renders directly at the chosen rate. Higher rates lower latency, reduce aliasing, and avoid resampler smearing. 'Auto' matches the frontend's target rate.",
      "audio",
      {
         { "auto",  "Auto" },
         { "32000", "32 kHz" },
         { "44100", "44 kHz" },
         { "48000", "48 kHz" },
         { "96000", "96 kHz" },
         { NULL, NULL },
      },
      "auto"
   },
   { NULL, NULL, NULL, NULL, NULL, NULL, {{0}}, NULL },
};

struct retro_core_options_v2 options_us = {
   option_cats_us,
   option_defs_us
};

/*
 ********************************
 * Language Mapping
 ********************************
*/

#ifndef HAVE_NO_LANGEXTRA
struct retro_core_options_v2 *options_intl[RETRO_LANGUAGE_LAST] = {
   &options_us, /* RETRO_LANGUAGE_ENGLISH */
   &options_ja,      /* RETRO_LANGUAGE_JAPANESE */
   &options_fr,      /* RETRO_LANGUAGE_FRENCH */
   &options_es,      /* RETRO_LANGUAGE_SPANISH */
   &options_de,      /* RETRO_LANGUAGE_GERMAN */
   &options_it,      /* RETRO_LANGUAGE_ITALIAN */
   &options_nl,      /* RETRO_LANGUAGE_DUTCH */
   &options_pt_br,   /* RETRO_LANGUAGE_PORTUGUESE_BRAZIL */
   &options_pt_pt,   /* RETRO_LANGUAGE_PORTUGUESE_PORTUGAL */
   &options_ru,      /* RETRO_LANGUAGE_RUSSIAN */
   &options_ko,      /* RETRO_LANGUAGE_KOREAN */
   &options_cht,     /* RETRO_LANGUAGE_CHINESE_TRADITIONAL */
   &options_chs,     /* RETRO_LANGUAGE_CHINESE_SIMPLIFIED */
   &options_eo,      /* RETRO_LANGUAGE_ESPERANTO */
   &options_pl,      /* RETRO_LANGUAGE_POLISH */
   &options_vn,      /* RETRO_LANGUAGE_VIETNAMESE */
   &options_ar,      /* RETRO_LANGUAGE_ARABIC */
   &options_el,      /* RETRO_LANGUAGE_GREEK */
   &options_tr,      /* RETRO_LANGUAGE_TURKISH */
   &options_sk,      /* RETRO_LANGUAGE_SLOVAK */
   &options_fa,      /* RETRO_LANGUAGE_PERSIAN */
   &options_he,      /* RETRO_LANGUAGE_HEBREW */
   &options_ast,     /* RETRO_LANGUAGE_ASTURIAN */
   &options_fi,      /* RETRO_LANGUAGE_FINNISH */
   &options_id,      /* RETRO_LANGUAGE_INDONESIAN */
   &options_sv,      /* RETRO_LANGUAGE_SWEDISH */
   &options_uk,      /* RETRO_LANGUAGE_UKRAINIAN */
};
#endif

/*
 ********************************
 * Functions
 ********************************
*/

/* Handles configuration/setting of core options.
 * Should only be called inside retro_set_environment().
 * > We place the function body in the header to avoid the
 *   necessity of adding more .c files (i.e. want this to
 *   be as painless as possible for core devs)
 */

static INLINE void libretro_set_core_options(retro_environment_t environ_cb,
      bool *categories_supported)
{
   unsigned version  = 0;
#ifndef HAVE_NO_LANGEXTRA
   unsigned language = 0;
#endif

   if (!environ_cb || !categories_supported)
      return;

   *categories_supported = false;

   if (!environ_cb(RETRO_ENVIRONMENT_GET_CORE_OPTIONS_VERSION, &version))
      version = 0;

   if (version >= 2)
   {
#ifndef HAVE_NO_LANGEXTRA
      struct retro_core_options_v2_intl core_options_intl;

      core_options_intl.us    = &options_us;
      core_options_intl.local = NULL;

      if (environ_cb(RETRO_ENVIRONMENT_GET_LANGUAGE, &language) &&
          (language < RETRO_LANGUAGE_LAST) && (language != RETRO_LANGUAGE_ENGLISH))
         core_options_intl.local = options_intl[language];

      *categories_supported = environ_cb(RETRO_ENVIRONMENT_SET_CORE_OPTIONS_V2_INTL,
            &core_options_intl);
#else
      *categories_supported = environ_cb(RETRO_ENVIRONMENT_SET_CORE_OPTIONS_V2,
            &options_us);
#endif
   }
   else
   {
      size_t i, j;
      size_t option_index              = 0;
      size_t num_options               = 0;
      struct retro_core_option_definition
            *option_v1_defs_us         = NULL;
#ifndef HAVE_NO_LANGEXTRA
      size_t num_options_intl          = 0;
      struct retro_core_option_v2_definition
            *option_defs_intl          = NULL;
      struct retro_core_option_definition
            *option_v1_defs_intl       = NULL;
      struct retro_core_options_intl
            core_options_v1_intl;
#endif
      struct retro_variable *variables = NULL;
      char **values_buf                = NULL;

      /* Determine total number of options */
      while (true)
      {
         if (option_defs_us[num_options].key)
            num_options++;
         else
            break;
      }

      if (version >= 1)
      {
         /* Allocate US array */
         option_v1_defs_us = (struct retro_core_option_definition *)
               calloc(num_options + 1, sizeof(struct retro_core_option_definition));

         /* Copy parameters from option_defs_us array */
         for (i = 0; i < num_options; i++)
         {
            struct retro_core_option_v2_definition *option_def_us = &option_defs_us[i];
            struct retro_core_option_value *option_values         = option_def_us->values;
            struct retro_core_option_definition *option_v1_def_us = &option_v1_defs_us[i];
            struct retro_core_option_value *option_v1_values      = option_v1_def_us->values;

            option_v1_def_us->key           = option_def_us->key;
            option_v1_def_us->desc          = option_def_us->desc;
            option_v1_def_us->info          = option_def_us->info;
            option_v1_def_us->default_value = option_def_us->default_value;

            /* Values must be copied individually... */
            while (option_values->value)
            {
               option_v1_values->value = option_values->value;
               option_v1_values->label = option_values->label;

               option_values++;
               option_v1_values++;
            }
         }

#ifndef HAVE_NO_LANGEXTRA
         if (environ_cb(RETRO_ENVIRONMENT_GET_LANGUAGE, &language) &&
             (language < RETRO_LANGUAGE_LAST) && (language != RETRO_LANGUAGE_ENGLISH) &&
             options_intl[language])
            option_defs_intl = options_intl[language]->definitions;

         if (option_defs_intl)
         {
            /* Determine number of intl options */
            while (true)
            {
               if (option_defs_intl[num_options_intl].key)
                  num_options_intl++;
               else
                  break;
            }

            /* Allocate intl array */
            option_v1_defs_intl = (struct retro_core_option_definition *)
                  calloc(num_options_intl + 1, sizeof(struct retro_core_option_definition));

            /* Copy parameters from option_defs_intl array */
            for (i = 0; i < num_options_intl; i++)
            {
               struct retro_core_option_v2_definition *option_def_intl = &option_defs_intl[i];
               struct retro_core_option_value *option_values           = option_def_intl->values;
               struct retro_core_option_definition *option_v1_def_intl = &option_v1_defs_intl[i];
               struct retro_core_option_value *option_v1_values        = option_v1_def_intl->values;

               option_v1_def_intl->key           = option_def_intl->key;
               option_v1_def_intl->desc          = option_def_intl->desc;
               option_v1_def_intl->info          = option_def_intl->info;
               option_v1_def_intl->default_value = option_def_intl->default_value;

               /* Values must be copied individually... */
               while (option_values->value)
               {
                  option_v1_values->value = option_values->value;
                  option_v1_values->label = option_values->label;

                  option_values++;
                  option_v1_values++;
               }
            }
         }

         core_options_v1_intl.us    = option_v1_defs_us;
         core_options_v1_intl.local = option_v1_defs_intl;

         environ_cb(RETRO_ENVIRONMENT_SET_CORE_OPTIONS_INTL, &core_options_v1_intl);
#else
         environ_cb(RETRO_ENVIRONMENT_SET_CORE_OPTIONS, option_v1_defs_us);
#endif
      }
      else
      {
         /* Allocate arrays */
         variables  = (struct retro_variable *)calloc(num_options + 1,
               sizeof(struct retro_variable));
         values_buf = (char **)calloc(num_options, sizeof(char *));

         if (!variables || !values_buf)
            goto error;

         /* Copy parameters from option_defs_us array */
         for (i = 0; i < num_options; i++)
         {
            const char *key                        = option_defs_us[i].key;
            const char *desc                       = option_defs_us[i].desc;
            const char *default_value              = option_defs_us[i].default_value;
            struct retro_core_option_value *values = option_defs_us[i].values;
            size_t buf_len                         = 3;
            size_t default_index                   = 0;

            values_buf[i] = NULL;

            if (desc)
            {
               size_t num_values = 0;

               /* Determine number of values */
               while (true)
               {
                  if (values[num_values].value)
                  {
                     /* Check if this is the default value */
                     if (default_value)
                        if (strcmp(values[num_values].value, default_value) == 0)
                           default_index = num_values;

                     buf_len += strlen(values[num_values].value);
                     num_values++;
                  }
                  else
                     break;
               }

               /* Build values string */
               if (num_values > 0)
               {
                  buf_len += num_values - 1;
                  buf_len += strlen(desc);

                  values_buf[i] = (char *)calloc(buf_len, sizeof(char));
                  if (!values_buf[i])
                     goto error;

                  strcpy(values_buf[i], desc);
                  strcat(values_buf[i], "; ");

                  /* Default value goes first */
                  strcat(values_buf[i], values[default_index].value);

                  /* Add remaining values */
                  for (j = 0; j < num_values; j++)
                  {
                     if (j != default_index)
                     {
                        strcat(values_buf[i], "|");
                        strcat(values_buf[i], values[j].value);
                     }
                  }
               }
            }

            variables[option_index].key   = key;
            variables[option_index].value = values_buf[i];
            option_index++;
         }

         /* Set variables */
         environ_cb(RETRO_ENVIRONMENT_SET_VARIABLES, variables);
      }

error:
      /* Clean up */

      if (option_v1_defs_us)
      {
         free(option_v1_defs_us);
         option_v1_defs_us = NULL;
      }

#ifndef HAVE_NO_LANGEXTRA
      if (option_v1_defs_intl)
      {
         free(option_v1_defs_intl);
         option_v1_defs_intl = NULL;
      }
#endif

      if (values_buf)
      {
         for (i = 0; i < num_options; i++)
         {
            if (values_buf[i])
            {
               free(values_buf[i]);
               values_buf[i] = NULL;
            }
         }

         free(values_buf);
         values_buf = NULL;
      }

      if (variables)
      {
         free(variables);
         variables = NULL;
      }
   }
}

#ifdef __cplusplus
}
#endif

#endif

