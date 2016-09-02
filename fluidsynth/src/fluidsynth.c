/* FluidSynth - A Software Synthesizer
 *
 * Copyright (C) 2003  Peter Hanappe and others.
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Library General Public License
 * as published by the Free Software Foundation; either version 2 of
 * the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful, but
 * WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Library General Public License for more details.
 *
 * You should have received a copy of the GNU Library General Public
 * License along with this library; if not, write to the Free
 * Software Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA
 * 02110-1301, USA
 */

#if HAVE_CONFIG_H
#include "config.h"
#endif

#include <stdlib.h>
#include <stdio.h>
#include <string.h>

#include "fluidsynth_priv.h"

#if !defined(WIN32) && !defined(MACINTOSH)
#define _GNU_SOURCE
#endif

#if defined(HAVE_GETOPT_H)
#include <getopt.h>
#endif

#include "fluidsynth.h"

#ifdef HAVE_SIGNAL_H
#include "signal.h"
#endif

#include "fluid_lash.h"

#ifndef WITH_MIDI
#define WITH_MIDI 1
#endif

/*
 * the globals
 */
fluid_cmd_handler_t* cmd_handler = NULL;
int option_help = 0;		/* set to 1 if "-o help" is specified */

/*
 * support for the getopt function
 */
#if defined(HAVE_GETOPT_H)
#define GETOPT_SUPPORT 1
int getopt(int argc, char * const argv[], const char *optstring);
extern char *optarg;
extern int optind, opterr, optopt;
#endif

/* Process a command line option -o setting=value, for example: -o synth.polyhony=16 */
void process_o_cmd_line_option(fluid_settings_t* settings, char* optarg)
{
  char* val;
  int hints;
  int ival;

  for (val = optarg; *val != '\0'; val++) {
    if (*val == '=') {
      *val++ = 0;
      break;
    }
  }

  /* did user request list of settings */
  if (strcmp (optarg, "help") == 0)
  {
    option_help = 1;
    return;
  }

  if (strcmp (optarg, "") == 0) {
    fprintf (stderr, "Invalid -o option (name part is empty)\n");
    return;
  }

  switch(fluid_settings_get_type(settings, optarg)){
  case FLUID_NUM_TYPE:
    if (!fluid_settings_setnum (settings, optarg, atof (val)))
    {
      fprintf (stderr, "Failed to set floating point parameter '%s'\n", optarg);
      exit (1);
    }
    break;
  case FLUID_INT_TYPE:
    hints = fluid_settings_get_hints (settings, optarg);

    if (hints & FLUID_HINT_TOGGLED)
    {
      if (FLUID_STRCMP (val, "yes") == 0 || FLUID_STRCMP (val, "True") == 0
          || FLUID_STRCMP (val, "TRUE") == 0 || FLUID_STRCMP (val, "true") == 0
          || FLUID_STRCMP (val, "T") == 0)
        ival = 1;
      else ival = atoi (val);
    }
    else ival = atoi (val);

    if (!fluid_settings_setint (settings, optarg, ival))
    {
      fprintf (stderr, "Failed to set integer parameter '%s'\n", optarg);
      exit (1);
    }
    break;
  case FLUID_STR_TYPE:
    if (!fluid_settings_setstr (settings, optarg, val))
    {
      fprintf (stderr, "Failed to set string parameter '%s'\n", optarg);
      exit (1);
    }
    break;
  default:
    fprintf (stderr, "Setting parameter '%s' not found\n", optarg);
    exit (1);
  }
}

#ifdef HAVE_SIGNAL_H
/*
 * handle_signal
 */
void handle_signal(int sig_num)
{
}
#endif
