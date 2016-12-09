LOCAL_PATH := $(call my-dir)
GIT_VERSION := " $(shell git rev-parse --short HEAD || echo unknown)"
ifneq ($(GIT_VERSION)," unknown")
	LOCAL_CFLAGS += -DGIT_VERSION=\"$(GIT_VERSION)\"
endif

include $(CLEAR_VARS)

ifeq ($(TARGET_ARCH),arm)
LOCAL_CFLAGS += -DANDROID_ARM
LOCAL_ARM_MODE := arm
endif

ifeq ($(TARGET_ARCH),x86)
LOCAL_CFLAGS +=  -DANDROID_X86
endif

ifeq ($(TARGET_ARCH),mips)
LOCAL_CFLAGS += -DANDROID_MIPS
endif

LOCAL_MODULE    := libretro

ROOT_DIR := ../
CORE_DIR := $(ROOT_DIR)/src

include $(ROOT_DIR)/Makefile.common

LOCAL_SRC_FILES    = $(SOURCES_C)

LOCAL_CFLAGS += -O3 -DHAVE_LIBMAD -DMUSIC_SUPPORT -DINLINE=inline -D__LIBRETRO__ -DFPM_DEFAULT $(INCFLAGS)
LOCAL_C_INCLUDES = $(INCFLAGS)

include $(BUILD_SHARED_LIBRARY)
