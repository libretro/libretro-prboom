DEBUG=0
STATIC_LINKING=0

ifeq ($(platform),)
platform = unix
ifeq ($(shell uname -a),)
   platform = win
else ifneq ($(findstring MINGW,$(shell uname -a)),)
   platform = win
else ifneq ($(findstring Darwin,$(shell uname -a)),)
   platform = osx
	arch = intel
ifeq ($(shell uname -p),powerpc)
	arch = ppc
endif
else ifneq ($(findstring win,$(shell uname -a)),)
   platform = win
endif
endif

# system platform
system_platform = unix
ifeq ($(shell uname -a),)
EXE_EXT = .exe
   system_platform = win
else ifneq ($(findstring Darwin,$(shell uname -a)),)
   system_platform = osx
	arch = intel
ifeq ($(shell uname -p),powerpc)
	arch = ppc
endif
else ifneq ($(findstring MINGW,$(shell uname -a)),)
   system_platform = win
endif

TARGET_NAME := prboom

LIBM := 
LDFLAGS := 

ifeq ($(STATIC_LINKING),1)
EXT=a
endif

ifeq ($(platform), unix)
	EXT    ?= so
   TARGET := $(TARGET_NAME)_libretro.$(EXT)
   fpic := -fPIC
   SHARED := -shared -Wl,--version-script=libretro/link.T -Wl,-no-undefined
else ifeq ($(platform), linux-portable)
	EXT    ?= so
   TARGET := $(TARGET_NAME)_libretro.$(EXT)
   fpic := -fPIC -nostdlib
   SHARED := -shared -Wl,--version-script=libretro/link.T
	LIBM :=
else ifeq ($(platform), osx)
	EXT    ?= dylib
   TARGET := $(TARGET_NAME)_libretro.$(EXT)
   fpic := -fPIC
   SHARED := -dynamiclib
ifeq ($(arch),ppc)
   CFLAGS += -DMSB_FIRST
endif
   OSXVER = `sw_vers -productVersion | cut -d. -f 2`
   OSX_LT_MAVERICKS = `(( $(OSXVER) <= 9)) && echo "YES"`
   fpic += -mmacosx-version-min=10.1

# iOS
else ifneq (,$(findstring ios,$(platform)))
	EXT    ?= dylib
   TARGET := $(TARGET_NAME)_libretro_ios.$(EXT)
   fpic := -fPIC
   SHARED := -dynamiclib

ifeq ($(IOSSDK),)
   IOSSDK := $(shell xcodebuild -version -sdk iphoneos Path)
endif

   CC = clang -arch armv7 -isysroot $(IOSSDK)
ifeq ($(platform),ios9)
   CFLAGS +=  -miphoneos-version-min=8.0
else
   CFLAGS +=  -miphoneos-version-min=5.0
endif
else ifeq ($(platform), theos_ios)
	# Theos iOS
DEPLOYMENT_IOSVERSION = 5.0
TARGET = iphone:latest:$(DEPLOYMENT_IOSVERSION)
ARCHS = armv7 armv7s
TARGET_IPHONEOS_DEPLOYMENT_VERSION=$(DEPLOYMENT_IOSVERSION)
THEOS_BUILD_DIR := objs
include $(THEOS)/makefiles/common.mk

LIBRARY_NAME = $(TARGET_NAME)_libretro_ios


else ifeq ($(platform), qnx)
	EXT    ?= so
   TARGET := $(TARGET_NAME)_libretro_qnx.$(EXT)
   fpic := -fPIC
   SHARED := -shared -Wl,--version-script=libretro/link.T -Wl,-no-undefined
	CC = qcc -Vgcc_ntoarmv7le
	AR = qcc -Vgcc_ntoarmv7le
   CFLAGS += -DHAVE_STRLWR
	CFLAGS += -D__BLACKBERRY_QNX__ -marm -mcpu=cortex-a9 -mfpu=neon -mfloat-abi=softfp
else ifeq ($(platform), ps3)
	EXT=a
   TARGET := $(TARGET_NAME)_libretro_ps3.$(EXT)
   CC = $(CELL_SDK)/host-win32/ppu/bin/ppu-lv2-gcc.exe
   AR = $(CELL_SDK)/host-win32/ppu/bin/ppu-lv2-ar.exe
   CFLAGS += -DMSB_FIRST
	STATIC_LINKING = 1
else ifeq ($(platform), sncps3)
	EXT=a
   TARGET := $(TARGET_NAME)_libretro_ps3.$(EXT)
   CC = $(CELL_SDK)/host-win32/sn/bin/ps3ppusnc.exe
   AR = $(CELL_SDK)/host-win32/sn/bin/ps3snarl.exe
   CFLAGS += -DMSB_FIRST
	STATIC_LINKING = 1
else ifeq ($(platform), psl1ght)
	EXT=a
   TARGET := $(TARGET_NAME)_libretro_psl1ght.$(EXT)
   CC = $(PS3DEV)/ppu/bin/ppu-gcc$(EXE_EXT)
   AR = $(PS3DEV)/ppu/bin/ppu-ar$(EXE_EXT)
   CFLAGS += -DMSB_FIRST -DHAVE_STRLWR
	STATIC_LINKING = 1

# PSP1
else ifeq ($(platform), psp1)
	EXT=a
   TARGET := $(TARGET_NAME)_libretro_psp1.$(EXT)
   CC = psp-gcc$(EXE_EXT)
   AR = psp-ar$(EXE_EXT)
   CFLAGS += -DHAVE_STRLWR -DPSP -G0
	STATIC_LINKING = 1

# Vita
else ifeq ($(platform), vita)
	EXT=a
   TARGET := $(TARGET_NAME)_libretro_vita.$(EXT)
	CC = arm-vita-eabi-gcc$(EXE_EXT)
	AR = arm-vita-eabi-ar$(EXE_EXT)
   CFLAGS += -DHAVE_STRLWR -DVITA
	STATIC_LINKING = 1

# CTR (3DS)
else ifeq ($(platform), ctr)
	EXT=a
	TARGET := $(TARGET_NAME)_libretro_ctr.$(EXT)
	CC = $(DEVKITARM)/bin/arm-none-eabi-gcc$(EXE_EXT)
	AR = $(DEVKITARM)/bin/arm-none-eabi-ar$(EXE_EXT)
	PLATFORM_DEFINES := -DARM11 -D_3DS
	CFLAGS += -march=armv6k -mtune=mpcore -mfloat-abi=hard
	CFLAGS += -Wall -mword-relocations
	CFLAGS += -fomit-frame-pointer -ffast-math
	STATIC_LINKING = 1

else ifeq ($(platform), xenon)
	EXT=a
   TARGET := $(TARGET_NAME)_libretro_xenon360.$(EXT)
   CC = xenon-gcc$(EXE_EXT)
   AR = xenon-ar$(EXE_EXT)
   CFLAGS += -D__LIBXENON__ -m32 -D__ppc__ -DMSB_FIRST
	STATIC_LINKING = 1
else ifeq ($(platform), ngc)
	EXT=a
   TARGET := $(TARGET_NAME)_libretro_ngc.$(EXT)
   CC = $(DEVKITPPC)/bin/powerpc-eabi-gcc$(EXE_EXT)
   AR = $(DEVKITPPC)/bin/powerpc-eabi-ar$(EXE_EXT)
   CFLAGS += -DGEKKO -DHW_DOL -mrvl -mcpu=750 -meabi -mhard-float -DMEMORY_LOW -DMSB_FIRST 
	STATIC_LINKING = 1
else ifeq ($(platform), wii)
	EXT=a
   TARGET := $(TARGET_NAME)_libretro_wii.$(EXT)
   CC = $(DEVKITPPC)/bin/powerpc-eabi-gcc$(EXE_EXT)
   AR = $(DEVKITPPC)/bin/powerpc-eabi-ar$(EXE_EXT)
   CFLAGS += -DGEKKO -DHW_RVL -mrvl -mcpu=750 -meabi -mhard-float -DMSB_FIRST
	STATIC_LINKING = 1
else ifneq (,$(findstring armv,$(platform)))
	EXT?=so
   TARGET := $(TARGET_NAME)_libretro.$(EXT)
   SHARED := -shared -Wl,--no-undefined
   fpic := -fPIC
   CC = gcc
ifneq (,$(findstring cortexa8,$(platform)))
   CFLAGS += -marm -mcpu=cortex-a8
   ASFLAGS += -mcpu=cortex-a8
else ifneq (,$(findstring cortexa9,$(platform)))
   CFLAGS += -marm -mcpu=cortex-a9
   ASFLAGS += -mcpu=cortex-a9
endif
   CFLAGS += -marm
ifneq (,$(findstring neon,$(platform)))
   CFLAGS += -mfpu=neon
   ASFLAGS += -mfpu=neon
   HAVE_NEON = 1
endif
ifneq (,$(findstring softfloat,$(platform)))
   CFLAGS += -mfloat-abi=softfp
   ASFLAGS += -mfloat-abi=softfp
else ifneq (,$(findstring hardfloat,$(platform)))
   CFLAGS += -mfloat-abi=hard
   ASFLAGS += -mfloat-abi=hard
endif
   CFLAGS += -DARM
else
	EXT?=dll
   TARGET := $(TARGET_NAME)_libretro.$(EXT)
   CC = gcc
   SHARED := -shared -static-libgcc -static-libstdc++ -s -Wl,--version-script=libretro/link.T
   CFLAGS += -D__WIN32__ -D__WIN32_LIBRETRO__ -Wno-missing-field-initializers -DHAVE_STRLWR
endif

ifeq ($(STATIC_LINKING),1)
SHARED=
fpic=
endif

LDFLAGS += $(LIBM)

CFLAGS += -DHAVE_LIBMAD -DMUSIC_SUPPORT

ifeq ($(DEBUG), 1)
CFLAGS += -O0 -g
else
CFLAGS += -O2 -DNDEBUG
endif

ROOT_DIR    := .
CORE_DIR    := src

include Makefile.common

OBJECTS = $(SOURCES_C:.c=.o)

DEFINES    = -D__LIBRETRO__ -DINLINE=inline -DFPM_DEFAULT -DFRONTEND_SUPPORTS_RGB565

ifeq ($(platform), sncps3)
WARNINGS_DEFINES =
CODE_DEFINES =
else
WARNINGS_DEFINES = -Wall -W -Wno-unused-parameter
CODE_DEFINES = -fomit-frame-pointer
endif

COMMON_DEFINES += $(CODE_DEFINES) $(WARNINGS_DEFINES) $(fpic)

CFLAGS     += $(DEFINES) $(COMMON_DEFINES)

%.o: %.c
	$(CC) $(INCFLAGS) $(CFLAGS) -c -o $@ $<

ifeq ($(platform), theos_ios)
COMMON_FLAGS := -DIOS $(COMMON_DEFINES) $(INCFLAGS) -I$(THEOS_INCLUDE_PATH) -Wno-error
$(LIBRARY_NAME)_CFLAGS += $(COMMON_FLAGS) $(CFLAGS)
${LIBRARY_NAME}_FILES = $(SOURCES_C)
include $(THEOS_MAKE_PATH)/library.mk
else
all: $(TARGET)

$(TARGET): $(OBJECTS)
ifeq ($(STATIC_LINKING), 1)
	$(AR) rcs $@ $(OBJECTS)
else
	$(CC) $(fpic) $(SHARED) $(INCFLAGS) -o $@ $(OBJECTS) $(LDFLAGS)
endif


clean-objs:
	rm -f $(OBJECTS)

clean:
	rm -f $(OBJECTS) $(TARGET)

.PHONY: clean clean-objs
endif
