DEBUG ?= 0
STATIC_LINKING ?= 0
WANT_FLUIDSYNTH ?= 0
HAVE_LOW_MEMORY ?= 0

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

prefix := /usr
libdir := $(prefix)/lib

LIBRETRO_DIR := libretro

TARGET_NAME := prboom
GIT_VERSION := " $(shell git rev-parse --short HEAD || echo unknown)"
ifneq ($(GIT_VERSION)," unknown")
	CFLAGS += -DGIT_VERSION=\"$(GIT_VERSION)\"
endif

LIBS    :=

ifeq (,$(findstring msvc,$(platform)))
LIBS    += -lm
endif

ifeq ($(STATIC_LINKING),1)
EXT=a

ifeq ($(platform), unix)
PLAT=_unix
endif
endif

SPACE :=
SPACE := $(SPACE) $(SPACE)
BACKSLASH :=
BACKSLASH := \$(BACKSLASH)
filter_out1 = $(filter-out $(firstword $1),$1)
filter_out2 = $(call filter_out1,$(call filter_out1,$1))
unixpath = $(subst \,/,$1)
unixcygpath = /$(subst :,,$(call unixpath,$1))

ifeq ($(platform), unix)
	EXT    ?= so
   TARGET := $(TARGET_NAME)_libretro$(PLAT).$(EXT)
   fpic := -fPIC
   SHARED := -shared -Wl,--version-script=libretro/link.T -Wl,--no-undefined -Wl,--as-needed
   CFLAGS += -std=c99
else ifeq ($(platform), linux-portable)
	EXT    ?= so
   TARGET := $(TARGET_NAME)_libretro.$(EXT)
   fpic := -fPIC -nostdlib
   SHARED := -shared -Wl,--version-script=libretro/link.T
	LIBS =
else ifeq ($(platform), osx)
	EXT    ?= dylib
   TARGET := $(TARGET_NAME)_libretro.$(EXT)
   fpic := -fPIC
   SHARED := -dynamiclib
   OSXVER = `sw_vers -productVersion | cut -d. -f 2`
   OSX_LT_MAVERICKS = `(( $(OSXVER) <= 9)) && echo "YES"`
   LDFLAGS += -framework CoreFoundation
ifeq ($(OSX_LT_MAVERICKS),YES)
   fpic += -mmacosx-version-min=10.1
endif

   ifeq ($(CROSS_COMPILE),1)
	TARGET_RULE   = -target $(LIBRETRO_APPLE_PLATFORM) -isysroot $(LIBRETRO_APPLE_ISYSROOT)
	CFLAGS   += $(TARGET_RULE)
	LDFLAGS  += $(TARGET_RULE)
   endif

# iOS
else ifneq (,$(findstring ios,$(platform)))
	EXT    ?= dylib
   TARGET := $(TARGET_NAME)_libretro_ios.$(EXT)
   fpic := -fPIC
   LDFLAGS += -framework CoreFoundation
   SHARED := -dynamiclib

ifeq ($(IOSSDK),)
   IOSSDK := $(shell xcodebuild -version -sdk iphoneos Path)
endif

ifeq ($(platform),ios-arm64)
   CC = clang -arch arm64 -isysroot $(IOSSDK)
else
   CC = clang -arch armv7 -isysroot $(IOSSDK)
endif

ifeq ($(platform),$(filter $(platform),ios9 ios-arm64))
	CFLAGS +=  -miphoneos-version-min=8.0
else
  CFLAGS +=  -miphoneos-version-min=5.0
endif

# tvOS
else ifeq ($(platform), tvos-arm64)
   TARGET := $(TARGET_NAME)_libretro_tvos.dylib
   fpic := -fPIC
   SHARED := -dynamiclib
   LDFLAGS += -framework CoreFoundation
   SHARED := -dynamiclib

ifeq ($(IOSSDK),)
   IOSSDK := $(shell xcodebuild -version -sdk appletvos Path)
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

# Classic Platforms ####################
# Platform affix = classic_<ISA>_<µARCH>
# Help at https://modmyclassic.com/comp

# (armv7 a7, hard point, neon based) ### 
# NESC, SNESC, C64 mini 
else ifeq ($(platform), classic_armv7_a7)
	TARGET := $(TARGET_NAME)_libretro.so
	fpic := -fPIC
	SHARED := -shared -Wl,--version-script=libretro/link.T -Wl,-no-undefined
	CFLAGS += -Ofast \
	-flto=4 -fwhole-program -fuse-linker-plugin \
	-fdata-sections -ffunction-sections -Wl,--gc-sections \
	-fno-stack-protector -fno-ident -fomit-frame-pointer \
	-falign-functions=1 -falign-jumps=1 -falign-loops=1 \
	-fno-unwind-tables -fno-asynchronous-unwind-tables -fno-unroll-loops \
	-fmerge-all-constants -fno-math-errno \
	-marm -mtune=cortex-a7 -mfpu=neon-vfpv4 -mfloat-abi=hard
	CXXFLAGS += $(CFLAGS)
	HAVE_NEON = 1
	ARCH = arm
	ifeq ($(shell echo `$(CC) -dumpversion` "< 4.9" | bc -l), 1)
	  CFLAGS += -march=armv7-a
	else
	  CFLAGS += -march=armv7ve
	  # If gcc is 5.0 or later
	  ifeq ($(shell echo `$(CC) -dumpversion` ">= 5" | bc -l), 1)
	    LDFLAGS += -static-libgcc -static-libstdc++
	  endif
	endif
#######################################

# (armv8 a35, hard point, neon based) ###
# PlayStation Classic 
else ifeq ($(platform), classic_armv8_a35)
	TARGET := $(TARGET_NAME)_libretro.so
	fpic := -fPIC
	SHARED := -shared -Wl,--version-script=libretro/link.T -Wl,-no-undefined
	CFLAGS += -Ofast \
	-fuse-linker-plugin \
	-fno-stack-protector -fno-ident -fomit-frame-pointer \
	-fmerge-all-constants -ffast-math -funroll-all-loops \
	-marm -mcpu=cortex-a35 -mfpu=neon-fp-armv8 -mfloat-abi=hard
	CXXFLAGS += $(CFLAGS)
	CPPFLAGS += $(CFLAGS)
	ASFLAGS += $(CFLAGS)
	HAVE_NEON = 1
	ARCH = arm
	LDFLAGS += -marm -mcpu=cortex-a35 -mfpu=neon-fp-armv8 -mfloat-abi=hard -Ofast -flto -fuse-linker-plugin
#######################################

else ifeq ($(platform), qnx)
	EXT    ?= so
   TARGET := $(TARGET_NAME)_libretro_qnx.$(EXT)
   fpic := -fPIC
   SHARED := -shared -Wl,--version-script=libretro/link.T -Wl,-no-undefined
	CC = qcc -Vgcc_ntoarmv7le
	AR = qcc -Vgcc_ntoarmv7le
   CFLAGS += -DHAVE_STRLWR
	CFLAGS += -D__BLACKBERRY_QNX__ -marm -mcpu=cortex-a9 -mfpu=neon -mfloat-abi=softfp
# Nintendo Switch (libnx)
else ifeq ($(platform), libnx)
    include $(DEVKITPRO)/libnx/switch_rules
    EXT=a
    TARGET := $(TARGET_NAME)_libretro_$(platform).$(EXT)
    DEFINES := -DSWITCH=1 -U__linux__ -U__linux -DRARCH_INTERNAL
    CFLAGS	:=	 $(DEFINES) -g -O3 \
                 -fPIE -I$(LIBNX)/include/ -ffunction-sections -fdata-sections -ftls-model=local-exec -Wl,--allow-multiple-definition -specs=$(LIBNX)/switch.specs
    CFLAGS += $(INCDIRS)
    CFLAGS	+=	-D__SWITCH__ -DHAVE_LIBNX -march=armv8-a -mtune=cortex-a57 -mtp=soft
    CXXFLAGS := $(ASFLAGS) $(CFLAGS) -fno-rtti -std=gnu++11
	 CFLAGS += -DHAVE_STRLWR
    CFLAGS += -std=gnu11
    STATIC_LINKING = 1
else ifeq ($(platform), psl1ght)
	EXT=a
   TARGET := $(TARGET_NAME)_libretro_$(platform).$(EXT)
   CC = $(PS3DEV)/ppu/bin/ppu-gcc$(EXE_EXT)
   AR = $(PS3DEV)/ppu/bin/ppu-ar$(EXE_EXT)
   CFLAGS += -DHAVE_STRLWR -D__PSL1GHT__
	STATIC_LINKING = 1

# PS2
else ifeq ($(platform), ps2)
	EXT=a
   TARGET := $(TARGET_NAME)_libretro_$(platform).$(EXT)
   CC = mips64r5900el-ps2-elf-gcc$(EXE_EXT)
   AR = mips64r5900el-ps2-elf-ar$(EXE_EXT)
   CFLAGS += -DHAVE_STRLWR -DPS2 -G0 -ffast-math -DABGR1555 -DNO_FAST_SQRT
   STATIC_LINKING = 1
   HAVE_LOW_MEMORY = 1
# PSP1
else ifeq ($(platform), psp1)
	EXT=a
   TARGET := $(TARGET_NAME)_libretro_$(platform).$(EXT)
   CC = psp-gcc$(EXE_EXT)
   AR = psp-ar$(EXE_EXT)
   CFLAGS += -DHAVE_STRLWR -DPSP -G0 -I$(shell psp-config --pspsdk-path)/include
	STATIC_LINKING = 1

# Vita
else ifeq ($(platform), vita)
	EXT=a
   TARGET := $(TARGET_NAME)_libretro_$(platform).$(EXT)
	CC = arm-vita-eabi-gcc$(EXE_EXT)
	AR = arm-vita-eabi-ar$(EXE_EXT)
   CFLAGS += -DHAVE_STRLWR -DVITA -fno-short-enums
	STATIC_LINKING = 1

# CTR (3DS)
else ifeq ($(platform), ctr)
	EXT=a
	TARGET := $(TARGET_NAME)_libretro_$(platform).$(EXT)
	CC = $(DEVKITARM)/bin/arm-none-eabi-gcc$(EXE_EXT)
	AR = $(DEVKITARM)/bin/arm-none-eabi-ar$(EXE_EXT)
	CFLAGS += -DARM11 -D_3DS -DHAVE_STRLWR
	CFLAGS += -march=armv6k -mtune=mpcore -mfloat-abi=hard
	CFLAGS += -Wall -mword-relocations
	CFLAGS += -fomit-frame-pointer -ffast-math
	STATIC_LINKING = 1

# emscripten
else ifeq ($(platform), emscripten)
   TARGET := $(TARGET_NAME)_libretro_$(platform).bc
   CFLAGS += -DHAVE_STRLWR
   STATIC_LINKING = 1

else ifeq ($(platform), xenon)
   EXT=a
   TARGET := $(TARGET_NAME)_libretro_xenon360.$(EXT)
   CC = xenon-gcc$(EXE_EXT)
   AR = xenon-ar$(EXE_EXT)
   CFLAGS += -D__LIBXENON__ -m32 -D__ppc__
   STATIC_LINKING = 1
else ifeq ($(platform), ngc)
   EXT=a
   TARGET := $(TARGET_NAME)_libretro_$(platform).$(EXT)
   CC = $(DEVKITPPC)/bin/powerpc-eabi-gcc$(EXE_EXT)
   AR = $(DEVKITPPC)/bin/powerpc-eabi-ar$(EXE_EXT)
   CFLAGS += -DGEKKO -DHW_DOL -mogc -mcpu=750 -meabi -mhard-float
   STATIC_LINKING = 1
   HAVE_LOW_MEMORY = 1

else ifeq ($(platform), wii)
   EXT=a
   TARGET := $(TARGET_NAME)_libretro_$(platform).$(EXT)
   CC = $(DEVKITPPC)/bin/powerpc-eabi-gcc$(EXE_EXT)
   AR = $(DEVKITPPC)/bin/powerpc-eabi-ar$(EXE_EXT)
   CFLAGS += -DGEKKO -DHW_RVL -mrvl -mcpu=750 -meabi -mhard-float
   STATIC_LINKING = 1

else ifeq ($(platform), wiiu)
   EXT=a
   TARGET := $(TARGET_NAME)_libretro_$(platform).$(EXT)
   CC = $(DEVKITPPC)/bin/powerpc-eabi-gcc$(EXE_EXT)
   AR = $(DEVKITPPC)/bin/powerpc-eabi-ar$(EXE_EXT)
   CFLAGS += -DGEKKO -DHW_RVL -DWIIU -mcpu=750 -meabi -mhard-float
   STATIC_LINKING = 1

# Nintendo Switch (libtransistor)
else ifeq ($(platform), switch)
	EXT=a
        TARGET := $(TARGET_NAME)_libretro_$(platform).$(EXT)
        include $(LIBTRANSISTOR_HOME)/libtransistor.mk
        STATIC_LINKING=1
CFLAGS += -DHAVE_STRLWR

else ifneq (,$(findstring armv,$(platform)))
   EXT?=so
   TARGET := $(TARGET_NAME)_libretro.$(EXT)
   SHARED := -shared -Wl,--no-undefined
   fpic := -fPIC
   # CC = gcc
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
else ifeq ($(platform), gcw0)
   TARGET := $(TARGET_NAME)_libretro.so
   CC = /opt/gcw0-toolchain/usr/bin/mipsel-linux-gcc
   CXX = /opt/gcw0-toolchain/usr/bin/mipsel-linux-g++
   AR = /opt/gcw0-toolchain/usr/bin/mipsel-linux-ar
   fpic := -fPIC
   SHARED := -shared -Wl,--version-script=libretro/link.T -Wl,-no-undefined
   CFLAGS += -ffast-math -march=mips32 -mtune=mips32r2 -mhard-float -fomit-frame-pointer
else ifeq ($(platform), retrofw)
   TARGET := $(TARGET_NAME)_libretro.so
   CC = /opt/retrofw-toolchain/usr/bin/mipsel-linux-gcc
   CXX = /opt/retrofw-toolchain/usr/bin/mipsel-linux-g++
   AR = /opt/retrofw-toolchain/usr/bin/mipsel-linux-ar
   fpic := -fPIC
   SHARED := -shared -Wl,--version-script=libretro/link.T -Wl,-no-undefined
   CFLAGS += -ffast-math -march=mips32 -mtune=mips32 -mhard-float -fomit-frame-pointer
else ifeq ($(platform), miyoo)
   TARGET := $(TARGET_NAME)_libretro.so
   CC = /opt/miyoo/usr/bin/arm-linux-gcc
   CXX = /opt/miyoo/usr/bin/arm-linux-g++
   AR = /opt/miyoo/usr/bin/arm-linux-ar
   fpic := -fPIC
   SHARED := -shared -Wl,--version-script=libretro/link.T -Wl,-no-undefined
   CFLAGS += -ffast-math -march=armv5te -mtune=arm926ej-s -fomit-frame-pointer
   HAVE_LOW_MEMORY = 1

# Windows MSVC 2003 Xbox 1
else ifeq ($(platform), xbox1_msvc2003)
TARGET := $(TARGET_NAME)_libretro_xdk1.lib
CC  = CL.exe
CXX  = CL.exe
LD   = lib.exe

export INCLUDE := $(XDK)/xbox/include
export LIB := $(XDK)/xbox/lib
PATH := $(call unixcygpath,$(XDK)/xbox/bin/vc71):$(PATH)
PSS_STYLE :=2
CFLAGS   += -D_XBOX -D_XBOX1
CXXFLAGS += -D_XBOX -D_XBOX1
STATIC_LINKING=1
HAS_GCC := 0
# Windows MSVC 2010 Xbox 360
else ifeq ($(platform), xbox360_msvc2010)
TARGET := $(TARGET_NAME)_libretro_xdk360.lib
MSVCBINDIRPREFIX = $(XEDK)/bin/win32
CC  = "$(MSVCBINDIRPREFIX)/cl.exe"
CXX  = "$(MSVCBINDIRPREFIX)/cl.exe"
LD   = "$(MSVCBINDIRPREFIX)/lib.exe"

export INCLUDE := $(XEDK)/include/xbox
export LIB := $(XEDK)/lib/xbox
PSS_STYLE :=2
CFLAGS   += -D_XBOX -D_XBOX360
CXXFLAGS += -D_XBOX -D_XBOX360
STATIC_LINKING=1
HAS_GCC := 0
CFLAGS += -DHAVE_STRLWR

# Windows MSVC 2010 x64
else ifeq ($(platform), windows_msvc2010_x64)
	CC  = cl.exe
	CXX = cl.exe

PATH := $(shell IFS=$$'\n'; cygpath "$(VS100COMNTOOLS)../../VC/bin/amd64"):$(PATH)
PATH := $(PATH):$(shell IFS=$$'\n'; cygpath "$(VS100COMNTOOLS)../IDE")
LIB := $(shell IFS=$$'\n'; cygpath "$(VS100COMNTOOLS)../../VC/lib/amd64")
INCLUDE := $(shell IFS=$$'\n'; cygpath "$(VS100COMNTOOLS)../../VC/include")

WindowsSdkDir := $(shell reg query "HKLM\SOFTWARE\Microsoft\Microsoft SDKs\Windows\v7.1A" -v "InstallationFolder" | grep -o '[A-Z]:\\.*')
WindowsSdkDir ?= $(shell reg query "HKLM\SOFTWARE\Microsoft\Microsoft SDKs\Windows\v7.0A" -v "InstallationFolder" | grep -o '[A-Z]:\\.*')

WindowsSdkDirInc := $(shell reg query "HKLM\SOFTWARE\Microsoft\Microsoft SDKs\Windows\v7.0A" -v "InstallationFolder" | grep -o '[A-Z]:\\.*')Include
WindowsSdkDirInc ?= $(shell reg query "HKLM\SOFTWARE\Microsoft\Microsoft SDKs\Windows\v7.1A" -v "InstallationFolder" | grep -o '[A-Z]:\\.*')Include


WindowsSDKIncludeDir := $(shell cygpath -w "$(WindowsSdkDir)\Include")
WindowsSDKGlIncludeDir := $(shell cygpath -w "$(WindowsSdkDir)\Include\gl")
WindowsSDKLibDir := $(shell cygpath -w "$(WindowsSdkDir)\Lib\x64")

INCFLAGS_PLATFORM = -I"$(WindowsSDKIncludeDir)"
export INCLUDE := $(INCLUDE);$(WindowsSDKIncludeDir);$(WindowsSDKGlIncludeDir)
export LIB := $(LIB);$(WindowsSDKLibDir)
TARGET := $(TARGET_NAME)_libretro.dll
PSS_STYLE :=2
LDFLAGS += -DLL
LIBS =
CFLAGS += -DHAVE_STRLWR
CFLAGS += -DFPM_64BIT
# Windows MSVC 2010 x86
else ifeq ($(platform), windows_msvc2010_x86)
	CC  = cl.exe
	CXX = cl.exe

PATH := $(shell IFS=$$'\n'; cygpath "$(VS100COMNTOOLS)../../VC/bin"):$(PATH)
PATH := $(PATH):$(shell IFS=$$'\n'; cygpath "$(VS100COMNTOOLS)../IDE")
LIB := $(shell IFS=$$'\n'; cygpath -w "$(VS100COMNTOOLS)../../VC/lib")
INCLUDE := $(shell IFS=$$'\n'; cygpath "$(VS100COMNTOOLS)../../VC/include")

WindowsSdkDir := $(shell reg query "HKLM\SOFTWARE\Microsoft\Microsoft SDKs\Windows\v7.1A" -v "InstallationFolder" | grep -o '[A-Z]:\\.*')
WindowsSdkDir ?= $(shell reg query "HKLM\SOFTWARE\Microsoft\Microsoft SDKs\Windows\v7.0A" -v "InstallationFolder" | grep -o '[A-Z]:\\.*')

WindowsSDKIncludeDir := $(shell cygpath -w "$(WindowsSdkDir)\Include")
WindowsSDKGlIncludeDir := $(shell cygpath -w "$(WindowsSdkDir)\Include\gl")
WindowsSDKLibDir := $(shell cygpath -w "$(WindowsSdkDir)\Lib")

INCFLAGS_PLATFORM = -I"$(WindowsSDKIncludeDir)"
export INCLUDE := $(INCLUDE);$(WindowsSDKIncludeDir);$(WindowsSDKGlIncludeDir)
export LIB := $(LIB);$(WindowsSDKLibDir)
TARGET := $(TARGET_NAME)_libretro.dll
PSS_STYLE :=2
LDFLAGS += -DLL
LIBS =
CFLAGS += -DHAVE_STRLWR
# Windows MSVC 2005 x86
else ifeq ($(platform), windows_msvc2005_x86)
	CC  = cl.exe
	CXX = cl.exe

PATH := $(shell IFS=$$'\n'; cygpath "$(VS80COMNTOOLS)../../VC/bin"):$(PATH)
PATH := $(PATH):$(shell IFS=$$'\n'; cygpath "$(VS80COMNTOOLS)../IDE")
INCLUDE := $(shell IFS=$$'\n'; cygpath "$(VS80COMNTOOLS)../../VC/include")
LIB := $(shell IFS=$$'\n'; cygpath -w "$(VS80COMNTOOLS)../../VC/lib")
BIN := $(shell IFS=$$'\n'; cygpath "$(VS80COMNTOOLS)../../VC/bin")

WindowsSdkDir := $(shell reg query "HKLM\SOFTWARE\Microsoft\Microsoft SDKs\Windows\v7.1A" -v "InstallationFolder" | grep -o '[A-Z]:\\.*')
WindowsSdkDir ?= $(shell reg query "HKLM\SOFTWARE\Microsoft\Microsoft SDKs\Windows\v7.0A" -v "InstallationFolder" | grep -o '[A-Z]:\\.*')

WindowsSDKIncludeDir := $(shell cygpath -w "$(WindowsSdkDir)\Include")
WindowsSDKAtlIncludeDir := $(shell cygpath -w "$(WindowsSdkDir)\Include\atl")
WindowsSDKCrtIncludeDir := $(shell cygpath -w "$(WindowsSdkDir)\Include\crt")
WindowsSDKGlIncludeDir := $(shell cygpath -w "$(WindowsSdkDir)\Include\gl")
WindowsSDKMfcIncludeDir := $(shell cygpath -w "$(WindowsSdkDir)\Include\mfc")
WindowsSDKLibDir := $(shell cygpath -w "$(WindowsSdkDir)\Lib")

INCFLAGS_PLATFORM = -I"$(WindowsSDKIncludeDir)"
export INCLUDE := $(INCLUDE);$(WindowsSDKIncludeDir);$(WindowsSDKAtlIncludeDir);$(WindowsSDKCrtIncludeDir);$(WindowsSDKGlIncludeDir);$(WindowsSDKMfcIncludeDir);libretro/libretro-common/include/compat/msvc
export LIB := $(LIB);$(WindowsSDKLibDir)
TARGET := $(TARGET_NAME)_libretro.dll
PSS_STYLE :=2
LDFLAGS += -DLL
CFLAGS += -D_CRT_SECURE_NO_DEPRECATE
CFLAGS += -DHAVE_STRLWR

# Windows MSVC 2003 x86
else ifeq ($(platform), windows_msvc2003_x86)
	CC  = cl.exe
	CXX = cl.exe

PATH := $(shell IFS=$$'\n'; cygpath "$(VS71COMNTOOLS)../../Vc7/bin"):$(PATH)
PATH := $(PATH):$(shell IFS=$$'\n'; cygpath "$(VS71COMNTOOLS)../IDE")
INCLUDE := $(shell IFS=$$'\n'; cygpath "$(VS71COMNTOOLS)../../Vc7/include")
LIB := $(shell IFS=$$'\n'; cygpath -w "$(VS71COMNTOOLS)../../Vc7/lib")
BIN := $(shell IFS=$$'\n'; cygpath "$(VS71COMNTOOLS)../../Vc7/bin")

WindowsSdkDir := $(INETSDK)

export INCLUDE := $(INCLUDE);$(INETSDK)/Include;src/drivers/libretro/msvc/msvc-2005
export LIB := $(LIB);$(WindowsSdkDir);$(INETSDK)/Lib
TARGET := $(TARGET_NAME)_libretro.dll
PSS_STYLE :=2
LDFLAGS += -DLL
CFLAGS += -D_CRT_SECURE_NO_DEPRECATE
CFLAGS += -DHAVE_STRLWR

# Windows MSVC 2017 all architectures
else ifneq (,$(findstring windows_msvc2017,$(platform)))

    NO_GCC := 1
    CFLAGS += -DNOMINMAX
    CXXFLAGS += -DNOMINMAX
    WINDOWS_VERSION = 1

	PlatformSuffix = $(subst windows_msvc2017_,,$(platform))
	ifneq (,$(findstring desktop,$(PlatformSuffix)))
		WinPartition = desktop
		MSVC2017CompileFlags = -DWINAPI_FAMILY=WINAPI_FAMILY_DESKTOP_APP -FS
		LDFLAGS += -MANIFEST -LTCG:incremental -NXCOMPAT -DYNAMICBASE -DEBUG -OPT:REF -INCREMENTAL:NO -SUBSYSTEM:WINDOWS -MANIFESTUAC:"level='asInvoker' uiAccess='false'" -OPT:ICF -ERRORREPORT:PROMPT -NOLOGO -TLBID:1
		LIBS += kernel32.lib user32.lib gdi32.lib winspool.lib comdlg32.lib advapi32.lib
	else ifneq (,$(findstring uwp,$(PlatformSuffix)))
		WinPartition = uwp
		MSVC2017CompileFlags = -DWINAPI_FAMILY=WINAPI_FAMILY_APP -D_WINDLL -D_UNICODE -DUNICODE -D__WRL_NO_DEFAULT_LIB__ -EHsc -FS
		LDFLAGS += -APPCONTAINER -NXCOMPAT -DYNAMICBASE -MANIFEST:NO -LTCG -OPT:REF -SUBSYSTEM:CONSOLE -MANIFESTUAC:NO -OPT:ICF -ERRORREPORT:PROMPT -NOLOGO -TLBID:1 -DEBUG:FULL -WINMD:NO
		LIBS += WindowsApp.lib
	endif

	CFLAGS += $(MSVC2017CompileFlags)
	CXXFLAGS += $(MSVC2017CompileFlags)

	TargetArchMoniker = $(subst $(WinPartition)_,,$(PlatformSuffix))

	CC  = cl.exe
	CXX = cl.exe
	LD = link.exe

	reg_query = $(call filter_out2,$(subst $2,,$(shell reg query "$2" -v "$1" 2>nul)))
	fix_path = $(subst $(SPACE),\ ,$(subst \,/,$1))

	ProgramFiles86w := $(shell cmd //c "echo %PROGRAMFILES(x86)%")
	ProgramFiles86 := $(shell cygpath "$(ProgramFiles86w)")

	WindowsSdkDir ?= $(call reg_query,InstallationFolder,HKEY_LOCAL_MACHINE\SOFTWARE\Wow6432Node\Microsoft\Microsoft SDKs\Windows\v10.0)
	WindowsSdkDir ?= $(call reg_query,InstallationFolder,HKEY_CURRENT_USER\SOFTWARE\Wow6432Node\Microsoft\Microsoft SDKs\Windows\v10.0)
	WindowsSdkDir ?= $(call reg_query,InstallationFolder,HKEY_LOCAL_MACHINE\SOFTWARE\Microsoft\Microsoft SDKs\Windows\v10.0)
	WindowsSdkDir ?= $(call reg_query,InstallationFolder,HKEY_CURRENT_USER\SOFTWARE\Microsoft\Microsoft SDKs\Windows\v10.0)
	WindowsSdkDir := $(WindowsSdkDir)

	WindowsSDKVersion ?= $(firstword $(foreach folder,$(subst $(subst \,/,$(WindowsSdkDir)Include/),,$(wildcard $(call fix_path,$(WindowsSdkDir)Include\*))),$(if $(wildcard $(call fix_path,$(WindowsSdkDir)Include/$(folder)/um/Windows.h)),$(folder),)))$(BACKSLASH)
	WindowsSDKVersion := $(WindowsSDKVersion)

	VsInstallBuildTools = $(ProgramFiles86)/Microsoft Visual Studio/2017/BuildTools
	VsInstallEnterprise = $(ProgramFiles86)/Microsoft Visual Studio/2017/Enterprise
	VsInstallProfessional = $(ProgramFiles86)/Microsoft Visual Studio/2017/Professional
	VsInstallCommunity = $(ProgramFiles86)/Microsoft Visual Studio/2017/Community

	VsInstallRoot ?= $(shell if [ -d "$(VsInstallBuildTools)" ]; then echo "$(VsInstallBuildTools)"; fi)
	ifeq ($(VsInstallRoot), )
		VsInstallRoot = $(shell if [ -d "$(VsInstallEnterprise)" ]; then echo "$(VsInstallEnterprise)"; fi)
	endif
	ifeq ($(VsInstallRoot), )
		VsInstallRoot = $(shell if [ -d "$(VsInstallProfessional)" ]; then echo "$(VsInstallProfessional)"; fi)
	endif
	ifeq ($(VsInstallRoot), )
		VsInstallRoot = $(shell if [ -d "$(VsInstallCommunity)" ]; then echo "$(VsInstallCommunity)"; fi)
	endif
	VsInstallRoot := $(VsInstallRoot)

	VcCompilerToolsVer := $(shell cat "$(VsInstallRoot)/VC/Auxiliary/Build/Microsoft.VCToolsVersion.default.txt" | grep -o '[0-9\.]*')
	VcCompilerToolsDir := $(VsInstallRoot)/VC/Tools/MSVC/$(VcCompilerToolsVer)

	WindowsSDKSharedIncludeDir := $(shell cygpath -w "$(WindowsSdkDir)\Include\$(WindowsSDKVersion)\shared")
	WindowsSDKUCRTIncludeDir := $(shell cygpath -w "$(WindowsSdkDir)\Include\$(WindowsSDKVersion)\ucrt")
	WindowsSDKUMIncludeDir := $(shell cygpath -w "$(WindowsSdkDir)\Include\$(WindowsSDKVersion)\um")
	WindowsSDKUCRTLibDir := $(shell cygpath -w "$(WindowsSdkDir)\Lib\$(WindowsSDKVersion)\ucrt\$(TargetArchMoniker)")
	WindowsSDKUMLibDir := $(shell cygpath -w "$(WindowsSdkDir)\Lib\$(WindowsSDKVersion)\um\$(TargetArchMoniker)")

	# For some reason the HostX86 compiler doesn't like compiling for x64
	# ("no such file" opening a shared library), and vice-versa.
	# Work around it for now by using the strictly x86 compiler for x86, and x64 for x64.
	# NOTE: What about ARM?
	ifneq (,$(findstring x64,$(TargetArchMoniker)))
		VCCompilerToolsBinDir := $(VcCompilerToolsDir)\bin\HostX64
	else
		VCCompilerToolsBinDir := $(VcCompilerToolsDir)\bin\HostX86
	endif

	PATH := $(shell IFS=$$'\n'; cygpath "$(VCCompilerToolsBinDir)/$(TargetArchMoniker)"):$(PATH)
	PATH := $(PATH):$(shell IFS=$$'\n'; cygpath "$(VsInstallRoot)/Common7/IDE")
	INCLUDE := $(shell IFS=$$'\n'; cygpath -w "$(VcCompilerToolsDir)/include")
	LIB := $(shell IFS=$$'\n'; cygpath -w "$(VcCompilerToolsDir)/lib/$(TargetArchMoniker)")
	ifneq (,$(findstring uwp,$(PlatformSuffix)))
		LIB := $(shell IFS=$$'\n'; cygpath -w "$(LIB)/store")
	endif
    
	export INCLUDE := $(INCLUDE);$(WindowsSDKSharedIncludeDir);$(WindowsSDKUCRTIncludeDir);$(WindowsSDKUMIncludeDir)
	export LIB := $(LIB);$(WindowsSDKUCRTLibDir);$(WindowsSDKUMLibDir)
	TARGET := $(TARGET_NAME)_libretro.dll
	PSS_STYLE :=2
	LDFLAGS += -DLL
	CFLAGS += -DHAVE_STRLWR

# Windows
else
	EXT?=dll
   TARGET := $(TARGET_NAME)_libretro.$(EXT)
   CC ?= gcc
   SHARED := -shared -static-libgcc -static-libstdc++ -s -Wl,--version-script=libretro/link.T
   CFLAGS += -D__WIN32__ -Wno-missing-field-initializers -DHAVE_STRLWR
LIBS =
endif

ifeq ($(STATIC_LINKING),1)
SHARED=
fpic=
endif

LDFLAGS += $(LIBS)

CFLAGS += -DHAVE_LIBMAD -DMUSIC_SUPPORT

ifeq ($(WANT_FLUIDSYNTH), 1)
CFLAGS += -DHAVE_LIBFLUIDSYNTH
endif

ifeq ($(HAVE_LOW_MEMORY), 1)
CFLAGS += -DMEMORY_LOW
endif

ifeq ($(DEBUG), 1)
ifneq (,$(findstring msvc,$(platform)))
   CFLAGS   += -MTd
   CXXFLAGS += -MTd
   CFLAGS   += -Od -Zi -D_DEBUG
   CXXFLAGS += -Od -Zi -D_DEBUG
else
   CFLAGS   += -O0 -g
   CXXFLAGS += -O0 -g
endif
   CFLAGS   += -DDEBUG
   CXXFLAGS += -DDEBUG
else
ifneq (,$(findstring msvc,$(platform)))
   CFLAGS   += -MT
   CXXFLAGS += -MT
endif
   CFLAGS   += -O2 -DNDEBUG
   CXXFLAGS += -O2 -DNDEBUG
endif

ROOT_DIR    := .
CORE_DIR    := src

include Makefile.common

OBJECTS = $(SOURCES_C:.c=.o)

DEFINES    = $(COREDEFINES)

ifneq (,$(findstring msvc,$(platform)))
WARNINGS_DEFINES =
CODE_DEFINES =
else
WARNINGS_DEFINES = -Wall -W -Wno-unused-parameter
CODE_DEFINES = -fomit-frame-pointer
endif

COMMON_DEFINES += $(CODE_DEFINES) $(WARNINGS_DEFINES) $(fpic) $(INCFLAGS) $(INCFLAGS_PLATFORM)

CFLAGS     += $(DEFINES) $(COMMON_DEFINES)

OBJOUT   = -o
LINKOUT  = -o 

ifneq (,$(findstring msvc,$(platform)))
	OBJOUT = -Fo
	LINKOUT = -out:
ifeq ($(STATIC_LINKING),1)
	LD ?= lib.exe
	STATIC_LINKING=0
else
	LD = link.exe
endif
else
	LD = $(CC)
endif

%.o: %.c
	$(CC) $(CFLAGS) -c $(OBJOUT)$@ $<

ifeq ($(platform), theos_ios)
COMMON_FLAGS := -DIOS $(COMMON_DEFINES) -I$(THEOS_INCLUDE_PATH) -Wno-error
$(LIBRARY_NAME)_CFLAGS += $(COMMON_FLAGS) $(CFLAGS)
${LIBRARY_NAME}_FILES = $(SOURCES_C)
include $(THEOS_MAKE_PATH)/library.mk
else
all: $(TARGET)

$(TARGET): $(OBJECTS)
ifeq ($(STATIC_LINKING), 1)
	$(AR) rcs $@ $(OBJECTS)
else
	$(LD) $(fpic) $(SHARED) $(LINKOUT)$@ $(OBJECTS) $(LDFLAGS)
endif

clean-objs:
	rm -f $(OBJECTS)

clean:
	rm -f $(OBJECTS) $(TARGET)

install:
	install -D -m 755 $(TARGET) $(DESTDIR)$(libdir)/$(LIBRETRO_DIR)/$(TARGET)

uninstall:
	rm $(DESTDIR)$(libdir)/$(LIBRETRO_DIR)/$(TARGET)

.PHONY: clean clean-objs install uninstall
endif
