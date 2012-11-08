LOCAL_PATH := $(call my-dir)

include $(CLEAR_VARS)

ifeq ($(TARGET_ARCH),arm)
LOCAL_CFLAGS += -DANDROID_ARM
endif

ifeq ($(TARGET_ARCH),x86)
LOCAL_CFLAGS +=  -DANDROID_X86
endif

ifeq ($(TARGET_ARCH),mips)
LOCAL_CFLAGS += -DANDROID_MIPS
endif

LMADSRCDIR = ../../libmad
DOOMSRC = ../../src

LIBMADOBJECTS = $(LMADSRCDIR)/bit.c $(LMADSRCDIR)/decoder.c $(LMADSRCDIR)/fixed.c $(LMADSRCDIR)/frame.c $(LMADSRCDIR)/huffman.c $(LMADSRCDIR)/layer3.c $(LMADSRCDIR)/layer12.c $(LMADSRCDIR)/stream.c $(LMADSRCDIR)/synth.c $(LMADSRCDIR)/timer.c

OBJECTS    = $(DOOMSRC)/am_map.c $(DOOMSRC)/d_deh.c $(DOOMSRC)/d_items.c $(DOOMSRC)/d_main.c $(DOOMSRC)/doomstat.c $(DOOMSRC)/dstrings.c $(DOOMSRC)/f_finale.c $(DOOMSRC)/f_wipe.c $(DOOMSRC)/g_game.c $(DOOMSRC)/hu_lib.c $(DOOMSRC)/hu_stuff.c $(DOOMSRC)/info.c $(DOOMSRC)/m_argv.c $(DOOMSRC)/m_bbox.c $(DOOMSRC)/m_cheat.c $(DOOMSRC)/m_menu.c $(DOOMSRC)/m_misc.c $(DOOMSRC)/m_random.c $(DOOMSRC)/p_ceilng.c $(DOOMSRC)/p_doors.c $(DOOMSRC)/p_enemy.c $(DOOMSRC)/p_floor.c $(DOOMSRC)/p_inter.c $(DOOMSRC)/p_lights.c $(DOOMSRC)/p_map.c $(DOOMSRC)/p_maputl.c $(DOOMSRC)/p_mobj.c $(DOOMSRC)/p_plats.c $(DOOMSRC)/p_pspr.c $(DOOMSRC)/p_saveg.c $(DOOMSRC)/p_setup.c $(DOOMSRC)/p_sight.c $(DOOMSRC)/p_spec.c $(DOOMSRC)/p_switch.c $(DOOMSRC)/p_telept.c $(DOOMSRC)/p_tick.c $(DOOMSRC)/p_user.c $(DOOMSRC)/r_bsp.c $(DOOMSRC)/r_data.c $(DOOMSRC)/r_draw.c $(DOOMSRC)/r_main.c $(DOOMSRC)/r_plane.c $(DOOMSRC)/r_segs.c $(DOOMSRC)/r_sky.c $(DOOMSRC)/r_things.c $(DOOMSRC)/r_patch.c $(DOOMSRC)/s_sound.c $(DOOMSRC)/sounds.c $(DOOMSRC)/st_lib.c $(DOOMSRC)/st_stuff.c $(DOOMSRC)/tables.c $(DOOMSRC)/v_video.c $(DOOMSRC)/w_wad.c $(DOOMSRC)/z_zone.c $(DOOMSRC)/w_memcache.c $(DOOMSRC)/r_fps.c $(DOOMSRC)/r_filter.c $(DOOMSRC)/p_genlin.c $(DOOMSRC)/r_demo.c $(DOOMSRC)/z_bmalloc.c $(DOOMSRC)/lprintf.c $(DOOMSRC)/wi_stuff.c $(DOOMSRC)/p_checksum.c $(DOOMSRC)/md5.c $(DOOMSRC)/version.c $(DOOMSRC)/d_client.c $(DOOMSRC)/mmus2mid.c $(PORTOBJECTS) $(DOOMSRC)/madplayer.c ../libretro.c

LOCAL_MODULE    := libretro

LOCAL_SRC_FILES    = $(LIBMADOBJECTS) $(OBJECTS)

LOCAL_CFLAGS += -DHAVE_LIBMAD -DMUSIC_SUPPORT -DGNU_SOURCE=1 -DINLINE=inline -DHAVE_INTTYPES_H -DHAVE_MEMORY_H -DLSB_FIRST -D__LIBRETRO__ -DFPM_DEFAULT -DSIZEOF_INT=4 -DSIZEOF_LONG=4 -DSIZEOF_LONG_LONG=8 -DFRONTEND_SUPPORTS_RGB565

include $(BUILD_SHARED_LIBRARY)
