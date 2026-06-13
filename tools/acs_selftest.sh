#!/bin/sh
# ACS conversation-lifecycle regression runner.
#
# Builds prboom_libretro with -DACS_SELFTEST and runs the in-core
# ZACS_ConversationSelfTest() once at startup against a stock IWAD.  The probe
# is content-neutral: it drives the dialogue subsystem through synthetic
# conversations (trigger/freeze, posting, branching/hub returns, the runaway-
# loop guard, the stall watchdog, unfreeze) and prints one line per check.
#
# Usage:  tools/acs_selftest.sh /path/to/DOOM.WAD
# Exit:   0 if every check passed, non-zero otherwise.
#
# This exists so changes to the ACS conversation code (p_zacs.c) are validated
# against the whole lifecycle, not just the path a given change touched -- the
# class of regression that has bitten this subsystem before.

set -e

IWAD="${1:-DOOM.WAD}"
if [ ! -f "$IWAD" ]; then
  echo "acs_selftest: IWAD not found: $IWAD" >&2
  echo "usage: tools/acs_selftest.sh /path/to/DOOM.WAD" >&2
  exit 2
fi

ROOT=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
WORK=$(mktemp -d)
trap 'rm -rf "$WORK"' EXIT

echo "acs_selftest: building prboom_libretro.so with -DACS_SELFTEST ..."
# The Makefile appends $(CFLAGS) after its own defines, so passing the define
# through CFLAGS reliably injects it.  A clean build guarantees every TU sees
# the flag (object reuse from a prior non-instrumented build would hide it).
make -C "$ROOT" clean >/dev/null 2>&1 || true
CFLAGS="-DACS_SELFTEST" make -C "$ROOT" -j4 >/dev/null 2>&1

SO="$ROOT/prboom_libretro.so"
[ -f "$SO" ] || { echo "acs_selftest: build produced no .so" >&2; exit 2; }

# Minimal headless libretro host: init, load the IWAD, run one frame (which
# fires the self-test once), capture its log lines, and grade them.
cat > "$WORK/run.c" <<'EOF'
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <stdarg.h>
#include <dlfcn.h>
#include <link.h>
#include "libretro.h"
static void *core;
#define LD(T,n) ((T)dlsym(core,n))
static void hlog(enum retro_log_level l,const char*f,...){va_list a;va_start(a,f);vfprintf(stderr,f,a);va_end(a);}
static bool env(unsigned c,void*d){switch(c){
  case RETRO_ENVIRONMENT_GET_SYSTEM_DIRECTORY:case RETRO_ENVIRONMENT_GET_SAVE_DIRECTORY:*(const char**)d=".";return true;
  case RETRO_ENVIRONMENT_SET_PIXEL_FORMAT:return true;
  case RETRO_ENVIRONMENT_GET_CAN_DUPE:*(bool*)d=true;return true;
  case RETRO_ENVIRONMENT_GET_VARIABLE:{struct retro_variable*v=d;v->value=NULL;return false;}
  case RETRO_ENVIRONMENT_GET_LOG_INTERFACE:{struct retro_log_callback*cb=d;cb->log=hlog;return true;}
  default:return false;}}
static void vcb(const void*a,unsigned w,unsigned h,size_t p){}
static void acb(int16_t a,int16_t b){} static size_t abcb(const int16_t*d,size_t f){return f;}
static void ipcb(void){} static int16_t iscb(unsigned a,unsigned b,unsigned c,unsigned e){return 0;}
int main(int argc,char**argv){int i;
  core=dlopen(argv[1],RTLD_NOW|RTLD_GLOBAL); if(!core){fprintf(stderr,"dlopen: %s\n",dlerror());return 2;}
  struct link_map *lm=NULL; dlinfo(core,RTLD_DI_LINKMAP,&lm); uintptr_t base=lm?(uintptr_t)lm->l_addr:0;
  void (*Defer)(int,const char*) = (void(*)(int,const char*))(base+strtoull(argv[3],NULL,16));
  LD(void(*)(retro_environment_t),"retro_set_environment")(env);
  LD(void(*)(retro_video_refresh_t),"retro_set_video_refresh")(vcb);
  LD(void(*)(retro_audio_sample_t),"retro_set_audio_sample")(acb);
  LD(void(*)(retro_audio_sample_batch_t),"retro_set_audio_sample_batch")(abcb);
  LD(void(*)(retro_input_poll_t),"retro_set_input_poll")(ipcb);
  LD(void(*)(retro_input_state_t),"retro_set_input_state")(iscb);
  LD(void(*)(void),"retro_init")();
  struct retro_game_info gi; memset(&gi,0,sizeof gi); gi.path=argv[2];
  if(!LD(bool(*)(const struct retro_game_info*),"retro_load_game")(&gi)){fprintf(stderr,"load_game failed\n");return 2;}
  void(*run)(void)=LD(void(*)(void),"retro_run");
  for(i=0;i<8;i++) run();
  Defer(2, argv[4]);            /* bring a real level up so a player actor exists */
  for(i=0;i<30;i++) run();      /* the self-test fires once gamestate==GS_LEVEL */
  return 0;
}
EOF

CC=${CC:-cc}
"$CC" -O2 -I"$ROOT/libretro/libretro-common/include" -o "$WORK/run" "$WORK/run.c" -ldl

# Resolve the deferred-map entry symbol so the runner can start a level.
DEF=$(nm "$SO" | awk '/ G_DeferedInitNewName$/{print $1}')
MAP="${ACS_SELFTEST_MAP:-E1M1}"
cp "$IWAD" "$WORK/IWAD.wad"
( cd "$WORK" && ./run "$SO" IWAD.wad "$DEF" "$MAP" ) > "$WORK/log.txt" 2>&1 || true

echo "--- ACS self-test checks ---"
grep -aE "ACS-SELFTEST" "$WORK/log.txt" || true

if grep -aqE "ACS-SELFTEST FAIL" "$WORK/log.txt"; then
  echo "acs_selftest: FAILED"
  exit 1
fi
if ! grep -aqE "ACS-SELFTEST: 0 failure" "$WORK/log.txt"; then
  echo "acs_selftest: self-test did not run (no result line)"
  exit 1
fi
echo "acs_selftest: PASSED"
exit 0
