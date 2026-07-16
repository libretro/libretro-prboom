# libretro-prboom

A libretro core based on the **PrBoom** Doom engine, extended with modern
demo-compatibility levels, UMAPINFO, and an extensive compatibility layer for
loading and playing **ZDoom-targeted mods** on a Boom/MBF-class engine.

It renders through an 8-bit paletted software renderer, needs no GL, and runs
anywhere libretro does — desktop, mobile, and consoles.

---

## Supported games

The core auto-detects the IWAD and configures itself for the right game:

- **Doom**, **The Ultimate Doom**, **Doom II**, **Final Doom** (TNT: Evilution,
  The Plutonia Experiment), and the shareware **doom1.wad**
- **Heretic** and **Hexen** (full game logic, not just map loading)
- **FreeDoom** (Phase 1 & 2)
- **Chex Quest** (including the ZDoom-targeted `chex3.wad`) and **HacX**
- Standalone / total-conversion IWADs that ship as Doom-format wads

You still need a valid IWAD — the engine provides the code, not the game data.

## Content formats

- **IWAD / PWAD** `.wad` files, including multi-PWAD load orders
- **PK3 / ZIP** archives (`.pk3`, `.zip`) with ZDoom folder-namespace layout
- **DeHackEd / BEX** patches (`.deh`, `.bex`), standalone or in-wad (`DEHACKED`)
- **`.lmp`** demos and **`.m3u`** playlists for multi-file sets
- Embedded `prboom.wad` resource lump is baked into the core

## Feature highlights

### Rendering
- Selectable internal resolution well beyond the original 320×200
- Hor+ **widescreen** with an in-menu aspect-ratio selector
- Per-sector **3D skyboxes** (SkyViewpoint / SkyPicker)
- **Wall bullet decals** (optional; off by default to match ZDoom mods that
  place their own)
- Translucency, deep water, independent floor/ceiling lighting, animated and
  per-level skies, and the full Boom/MBF visual feature set

### Compatibility & demos
- Behaves as any of vanilla Doom v1.9, Boom, MBF, or **MBF21**
- Forced compatibility-level core option, or automatic per-wad selection
- Plays vanilla, Boom, and MBF demos; savegames and demos store full game
  parameters and the loaded-wad list

### Level info & mod metadata
- **UMAPINFO** (reference parser) for map names, music, sky, par, boss actions,
  intermission text, and custom episode structure
- ZDoom old-syntax **MAPINFO** translated into UMAPINFO
- **MUSINFO** dynamic per-map music
- **LANGUAGE** string-table lookups feeding the above

### Audio & music
- **OPL3** emulation (DOSBox/Nuked-derived `dbopl`) for authentic AdLib MIDI
- **FluidSynth** SoundFont playback
- **Native MIDI** out to the frontend
- **MUS→MIDI** conversion, plus high-quality resampling with a selectable
  output sample rate

### Input
- Gamepad, mouse, and keyboard; analog stick with configurable deadzone
- Optional mouse input while on a gamepad device
- Rumble on supported pads

### libretro integration
- Full **VFS** file I/O
- Frontend memory-status reporting used to size the zone cache
- Optional **memory-mapped WAD loading** (see core options) to cut load time
  and memory use on large wads
- Savestates and the standard libretro option/variable interfaces

## Core options

| Option | Purpose |
| --- | --- |
| Internal Resolution *(restart)* | Render resolution. |
| Compatibility Level *(restart)* | Force a demo-compat level (vanilla → Boom → MBF → MBF21), or auto. |
| Cache Size | Limit on the asset (lump) cache pool. |
| Memory-Map WAD Files *(restart)* | Load wads by `mmap` instead of a full read; falls back to a normal read where mapping is unavailable. Default off. |
| Sound Sample Rate | Audio/resampler output rate. |
| Wall Bullet Decals | Stamp hitscan scuff marks on walls. Default off. |
| Mouse Active When Using Gamepad | Allow mouse input on a gamepad device. |
| Analog Deadzone (Percent) | Gamepad stick deadzone. |
| Look on Parent Folders for IWADs | Scan parent folders for IWADs (disable for SIGIL). |
| Rumble Effects | Haptic feedback on rumble pads. |

---

## ZDoom-format mod support

This engine is **not** GZDoom. It cannot run ZScript, and it renders in 8-bit
software. What it *does* have is a deliberate compatibility layer that lets a
large class of ZDoom-targeted wads **load and play** by translating or aliasing
their content onto the engine's native Boom/MBF/Hexen capabilities. Where a
feature has no equivalent, the loader degrades gracefully rather than refusing
the wad.

### Implemented

- **PK3 / ZIP archives.** Full ZDoom folder-namespace handling: `maps/`,
  `sprites/`, `flats/`, `textures/`, `patches/`, `graphics/`, `sounds/`,
  `music/`, `acs/`, and unknown folders routed to the global namespace by format
  sniffing. Root-level `.wad` members are expanded inline with their own
  directories honoured.
- **ACS scripting.** A near-complete ACS virtual machine covering the ACS0,
  ACSE, and ACSe (little-enhanced) object formats and the full 386-entry ZDoom
  pcode set, from both per-map `BEHAVIOR` lumps and global ACS libraries.
  Opcodes the engine can't honour execute as stack-disciplined no-ops and
  announce themselves once by name, so scripts keep running instead of
  desyncing.
- **Map formats.** Doom-binary, **Hexen-binary**, and **UDMF** text maps
  (`Doom`, `Heretic`, `Hexen`, `DSDA`, `ZDoom` namespaces).
- **Hexen features.** Polyobjects and the Hexen line-special / ACS action set.
- **3D floors** on binary ZDoom/Hexen maps (`Sector_Set3DFloor`).
- **3D skyboxes** (`SkyViewpoint` 9080 / `SkyPicker` 9081).
- **Sloped floors and ceilings.** ZDoom `Plane_Align` (special 181, on binary
  and Doom-in-Hexen maps) and thing-based **vertex slopes** (slope-vertex things
  1504 / 1505) are spawned and drawn as **tilted visplanes** in the 8-bit
  software renderer; the play sim (movement, thing Z, hitscan) follows the
  slope plane.
- **DECORATE actor aliasing.** Actor headers (`name`, `: Parent`,
  `replaces`, doomednum) are parsed and resolved to a base-game editor number by
  walking parent/replaces links, so modded things spawn in place and the wad's
  own sprite replacements supply the look. A curated **safe subset** of behavior
  is also captured: spawn frame-chains, the five engine weapon states
  (Ready/Select/Deselect/Fire/Flash), `ACS_NamedExecuteAlways` on frames, and
  self-contained codepointers.
- **Lump parsers:** ZDoom **TEXTURES**, **ANIMDEFS**, **SNDINFO**, **DECALDEF**,
  and **LANGUAGE**.
- **Editor-only things** (particle fountains, interpolation/camera/view-stack
  points) are recognized and skipped silently instead of spamming
  unknown-thing warnings.

### Partial / with caveats

- **DECORATE is translated, not executed.** There is no scripting VM for actor
  logic. Custom firing collapses to the replaced weapon's native attack; actors
  that root in a class with no editor number (brand-new decorations or wholly
  custom behaviors) cannot be aliased and stay unspawned. Arbitrary state logic,
  custom inventory/powerups, and ZDoom-only action functions are not reproduced.
- **MAPINFO:** both the classic **brace-less** and the newer **block/`{ }`**
  ZDoom syntaxes are translated (`{`/`}` skipped, optional `=`, `;` as a
  same-line statement separator). A curated subset of keys is consumed
  (level name, next/secretnext with the EndGame/endbunny sentinels, sky,
  music incl. `$MUSIC_*` indirection, par, titlepatch, cluster, boss
  specialactions); unrecognized keys are skipped rather than mapped.
- **UDMF** consumes the engine-carried field subset (the DSDA set) plus line
  `special` + `arg0..4`, so special-driven features apply on text maps as on
  binary ones — `Sector_Set3DFloor` (3D floors) and `Plane_Align` (slopes)
  included. UDMF-native structured portal fields are not read.
- **GLDEFS:** only skybox-relevant handling; glow/brightmap/light definitions
  are not consumed.

### Not supported

- **ZScript** — no support. Mods whose gameplay lives in ZScript won't run it.
- **Line / sector portals** — inert.
- **3D models (MODELDEF), voxels, dynamic/point lights** — out of scope for the
  8-bit software renderer.
- **Truecolor rendering.**

The practical result: map-and-resource-driven ZDoom wads — new levels, sprite
and texture replacements, ACS-scripted set pieces, reskinned monsters, Hexen-
style hubs, 3D floors, slopes and skyboxes — generally play. Wads built around ZScript
classes or GL-renderer features do not.

---

## Building

```
make                # host (auto-detects platform=unix)
make platform=osx   # macOS
```

`HAVE_MMAP` is enabled on the unix, linux-portable, osx, ios, and android
builds, which activates the memory-mapped WAD path behind its (default-off)
core option. Windows and console targets build the same source with the option
present but inert.

## Credits & license

PrBoom is derived from Boom, MBF, LxDoom, and the original id Software Doom
source. UMAPINFO parsing is from Christoph Oelckers and Fernando Carmona Varo.
See `AUTHORS` for the full lineage.

Released under the **GNU General Public License v2**; see `COPYING`.
