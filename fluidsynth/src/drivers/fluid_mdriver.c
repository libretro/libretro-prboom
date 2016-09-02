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

#include "fluid_mdriver.h"
#include "fluid_settings.h"


/*
 * fluid_mdriver_definition
 */
struct fluid_mdriver_definition_t {
  char* name;
  fluid_midi_driver_t* (*new)(fluid_settings_t* settings,
			     handle_midi_event_func_t event_handler,
			     void* event_handler_data);
  int (*free)(fluid_midi_driver_t* p);
  void (*settings)(fluid_settings_t* settings);
};


struct fluid_mdriver_definition_t fluid_midi_drivers[] = {
  { NULL, NULL, NULL, NULL }
};



void fluid_midi_driver_settings(fluid_settings_t* settings)
{
  int i;

  fluid_settings_register_int (settings, "midi.realtime-prio",
                               FLUID_DEFAULT_MIDI_RT_PRIO, 0, 99, 0, NULL, NULL);

  /* Set the default driver */
  fluid_settings_register_str(settings, "midi.driver", "", 0, NULL, NULL);

  /* Add all drivers to the list of options */
  for (i = 0; fluid_midi_drivers[i].name != NULL; i++) {
    if (fluid_midi_drivers[i].settings != NULL) {
      fluid_midi_drivers[i].settings(settings);
    }
  }
}

/**
 * Create a new MIDI driver instance.
 * @param settings Settings used to configure new MIDI driver.
 * @param handler MIDI handler callback (for example: fluid_midi_router_handle_midi_event()
 *   for MIDI router)
 * @param event_handler_data Caller defined data to pass to 'handler'
 * @return New MIDI driver instance or NULL on error
 */
fluid_midi_driver_t* new_fluid_midi_driver(fluid_settings_t* settings, handle_midi_event_func_t handler, void* event_handler_data)
{
  fluid_midi_driver_t* driver = NULL;
  char *allnames;
  int i;

  for (i = 0; fluid_midi_drivers[i].name != NULL; i++) {
    if (fluid_settings_str_equal(settings, "midi.driver", fluid_midi_drivers[i].name)) {
      FLUID_LOG(FLUID_DBG, "Using '%s' midi driver", fluid_midi_drivers[i].name);
      driver = fluid_midi_drivers[i].new(settings, handler, event_handler_data);
      if (driver) {
        driver->name = fluid_midi_drivers[i].name;
      }
      return driver;
    }
  }

  allnames = fluid_settings_option_concat (settings, "midi.driver", NULL);
  FLUID_LOG(FLUID_ERR, "Couldn't find the requested midi driver. Valid drivers are: %s.",
            allnames ? allnames : "ERROR");
  if (allnames) FLUID_FREE (allnames);

  return NULL;
}

/**
 * Delete a MIDI driver instance.
 * @param driver MIDI driver to delete
 */
void delete_fluid_midi_driver(fluid_midi_driver_t* driver)
{
  int i;

  for (i = 0; fluid_midi_drivers[i].name != NULL; i++) {
    if (fluid_midi_drivers[i].name == driver->name) {
      fluid_midi_drivers[i].free(driver);
      return;
    }
  }
}
