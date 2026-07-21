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
- **`.lmp`** demos and **`.m3u`** playlists for multi-file sets — playlist
  entries may freely mix WADs, PK3/ZIP archives, and DEH/BEX patches
- Embedded `prboom.wad` resource lump is baked into the core

## Feature highlights

### Rendering
- Selectable internal resolution well beyond the original 320×200
- Hor+ **widescreen** with an in-menu aspect-ratio selector
- Per-sector **3D skyboxes** (SkyViewpoint / SkyPicker), rendered only into
  the sky pixels actually visible each frame
- **GLDEFS dynamic point lights**, drawn by the software renderer: walls,
  floors, ceilings and sprites are brightened — and colour-tinted — by
  nearby `pointlight` / `pulselight` / `flickerlight` definitions, with
  per-view light culling to keep it cheap
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
- **OPL2** emulation (DOSBox-derived `dbopl`) for authentic AdLib MIDI,
  clocked at the chip's true hardware rate and band-limited to the output
- **FluidSynth** SoundFont playback, **native MIDI** out to the frontend,
  and streamed **MP3**, **Ogg Vorbis**, and ProTracker **MOD** music lumps
- **Float audio output** negotiated with the frontend where supported, with
  a quantization-free path from every synth/decoder that has one
- **MUS→MIDI** conversion, selectable output sample rate, and per-backend
  music position save/restore so runahead and rewind stay seamless
- Full detail in [Sound system](#sound-system) below

### Input
- Gamepad, mouse, and keyboard; analog stick with configurable deadzone
- Optional mouse input while on a gamepad device
- Rumble on supported pads

### libretro integration
- Full **VFS** file I/O
- Frontend memory-status reporting used to size the zone cache
- Optional **memory-mapped WAD loading** (see core options) to cut load time
  and memory use on large wads
- Savestates and the standard libretro option/variable interfaces;
  savestates from incompatible core builds are rejected cleanly (the game
  keeps running) instead of crashing

## Core options

| Option | Purpose |
| --- | --- |
| Internal Resolution *(restart)* | Render resolution. |
| Compatibility Level *(restart)* | Force a demo-compat level (vanilla → Boom → MBF → MBF21), or auto. |
| Cache Size | Limit on the asset (lump) cache pool. |
| Memory-Map WAD Files *(restart)* | Load wads by `mmap` instead of a full read; falls back to a normal read where mapping is unavailable. Default off. |
| Sound Sample Rate | Audio/resampler output rate. |
| Wall Bullet Decals | Stamp hitscan scuff marks on walls. Default off. |
| Dynamic Light Wall Falloff | Vertical falloff bands for point-light wall illumination (softer, slightly costlier). Default off. |
| Mouse Active When Using Gamepad | Allow mouse input on a gamepad device. |
| Analog Deadzone (Percent) | Gamepad stick deadzone. |
| Look on Parent Folders for IWADs | Scan parent folders for IWADs (disable for SIGIL). |
| Rumble Effects | Haptic feedback on rumble pads. |

---

## Sound system

The audio path has been reworked end to end around two rules: **quantize
once** (nothing is narrowed to int16 and widened again mid-pipeline) and
**verify empirically** (every change is A/B'd against the previous output,
bit-exactly where the change claims equivalence).

### Output pipeline

- The core negotiates **float32 output** with the frontend
  (`RETRO_ENVIRONMENT_GET_AUDIO_SAMPLE_BATCH_FLOAT`) and falls back to
  int16 everywhere else.  On the float lane, backends with a
  higher-precision native stage hand their samples over without ever
  touching int16; on the int16 lane the arithmetic is unchanged from the
  classic path.
- **Selectable output rate** — 32, 44.1, 48, or 96 kHz — applied at
  runtime: the SFX step tables are retuned in place, the synth backends
  are re-initialised at the new rate, and the current song resumes from
  its saved sample position.  Higher rates lower latency and push
  aliasing images above the audible band.
- **Deterministic frame pacing**: the mixer runs once per `retro_run`,
  producing frames from a 16.16 fixed-point accumulator so the long-run
  average is exactly `sample_rate / fps` with no drift and no frontend
  resampler engagement.

### Music backends

The MIDI synth (OPL, FluidSynth, or raw MIDI out to the frontend) is
chosen by the *MIDI Hardware* menu setting; non-MIDI lumps are
autodetected by decode attempt.

- **OPL2 (`dbopl`)** — the chip is emulated at its true hardware rate
  (14.318 MHz / 288 ≈ 49716 Hz) so envelopes, vibrato/tremolo and phase
  accumulators advance exactly as on real silicon, then the mono chip
  output is band-limited to the frontend rate through a **polyphase
  Kaiser-windowed-sinc resampler** (32 taps × 512 phases, cutoff tracked
  to the output Nyquist on downsample; unity ratio is a bit-exact
  passthrough).  The raw 32-bit chip sum feeds the filter directly —
  gain is applied in float to the filtered sample and the result is
  quantized exactly once at emission, so hot passages are no longer
  hard-clipped per native sample ahead of the filter.
- **FluidSynth** — SoundFont MIDI with volume folded into the synth
  gain; renders int16 or float natively per the negotiated lane.
- **MP3 (libmad)** — the decoder's 28-bit fixed-point synthesis is kept
  at full precision until a single conversion; the float lane converts
  fixed→float once and resamples in float, skipping the int16 narrowing
  the classic lane performs.
- **Ogg Vorbis (stb_vorbis)** — float-native decode, interpolation and
  volume on the float lane; int16 mirror lane for classic output.
- **ProTracker MOD (pocketmod)** — deliberately **integer-only**: the
  decoder was rewritten to emit interleaved s16 directly with
  deterministic fixed-point arithmetic, so its output is reproducible
  bit-for-bit across platforms.
- **Raw MIDI out** — event stream handed to the frontend's MIDI
  interface for external hardware/soft synths.

### Sound effects

- Sources: classic **DMX** 8-bit lumps, **WAV** (8/16-bit; multi-channel
  lumps are averaged to mono with proper rounding), and **Ogg** lumps
  from pk3s.
- Samples are stored at their **native rate** and resampled per-channel
  at mix time with 16.16 stepping — a quarter of the resident memory of
  the old upsample-at-load scheme, and no per-lump resample pass at
  startup.
- The per-channel **linear interpolation carries a 15-bit weight**,
  within 1 LSB of the exact 64-bit result across the whole sample/cursor
  space (the historical 8-bit weight truncated by up to ~250 LSB).  Both
  output lanes share the single interpolation body.
- 32 mixing channels; the mixer collects active channels into a compact
  list and mixes in chunks bounded by the nearest channel end, so the
  inner loop runs with no per-sample end-of-data checks.  Vanilla-style
  random pitch variation (and the Heretic/Hexen jitter) is applied
  through a libm-free step table.

### Save states, runahead, rewind

Every music backend serializes its own playback position — OPL per-track
MIDI iterator positions, FluidSynth event-index replay, Ogg decoder
sample-offset seek, MP3 frame byte-offset plus intra-frame cursor, MOD
channel state — so per-frame save/restore (runahead, rewind) resumes
music in place instead of re-decoding from the start.  Backends without
a fast path fall back to a generic render-replay driven by a
samples-played counter.

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
- **Voxel models.** ZDoom `VOXELDEF` bindings replace a sprite with a Ken
  Silverman **KVX** voxel model, rasterised by the software renderer
  (`R_DrawVoxel`) as projected per-voxel splats, with the model's 6-bit
  palette remapped to PLAYPAL.
- **Dynamic point lights.** ZDoom GLDEFS `pointlight` / `pulselight` /
  `flickerlight` definitions and their `object`/sprite bindings light the
  world from within the software renderer: reached walls, flats and sprites
  get a light-level boost and a colour tint toward the light's RGB, with
  pulse/flicker animation, per-view culling, and an optional vertical
  falloff on walls (core option).
- **Brightmaps.** ZDoom GLDEFS `brightmap` blocks for **textures, flats and
  sprites** are parsed, built into per-texel masks (`U_BuildBrightmasks`) and
  applied by the software column/span drawers, so masked texels draw at full
  brightness regardless of sector light — the usual glowing screens, lamps and
  lava veins.
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
  included. UDMF-native structured portal fields are not read (portals are
  taken from the thing pairs and line specials described below).
- **GLDEFS:** skybox handling, **brightmap** definitions, dynamic
  point-**light** definitions (with their sprite bindings), and sector
  **glow** blocks are all consumed: glowing flats draw fullbright and light
  nearby walls, and glowing wall textures (lava falls, waterfalls) draw
  fullbright themselves and pool colour onto the floors and ceilings beside
  them, with colours taken from the definition or derived from the texture
  itself.
- **Sector portals (look-only):** a floor or ceiling can be a window onto
  somewhere else.  Authored either as `UpperStackLookOnly`/
  `LowerStackLookOnly` thing pairs (stacked sectors) or with
  `Sector_SetPortal` (line special 57), whose **view** (0), **copied** (1),
  **skybox** (2), **fixed plane** (3) and **horizon** (4) types all resolve;
  the Eternity `Portal_Plane*` and `Portal_Horizon*` line types convert to
  the last two.  A view portal
  shows the linked region drawn from the viewer displaced by the pair's
  offset; a copied portal hands an existing window to sectors that need
  their own tag, such as a lift; a skybox portal shows a `SkyCamCompat`
  camera's surroundings on any plane, with or without the sky flat; and a
  horizon portal extends a sector's own floor and ceiling to infinity; and a
  fixed plane portal does the same but measured from the camera, so the
  surface looks identical from anywhere in the level.
  Windows composite into their visible pixels only, so geometry and sprites
  in front of one occlude it correctly, and things inside a viewed region
  are drawn.  One portal depth: a window seen through a window draws its own
  flat.  The opacity argument selects the window flat's own transparency —
  unset or zero is a clear window, 255 leaves the flat solid and no window
  at all, and values between blend the view through against the flat.
- **Line portals (look-only):** `Line_SetPortal` (line special 156) turns a
  wall into a window onto its partner line's surroundings.  The view is
  taken from the viewer's own position carried through to the partner line
  — offset, rotated by the angle between the two lines — so a pair at any
  relative angle works, and the window turns with the player.  The
  `planeanchor` argument is honoured, shifting the view to match the
  partner's floor or ceiling where the two sides differ in height.  Both
  one-sided lines (the whole wall becomes the window) and two-sided ones
  (the opening between the upper and lower textures) are supported, with
  the surrounding textures still drawn, and the exit line is found however
  the map format names it — a UDMF line id, `Line_SetPortal`'s own
  `thisline` argument, or `Line_SetIdentification`.  Geometry and sprites
  in front of a portal occlude it correctly.  Types 0-3 all render this
  window, since the visual half is common to them; what the interactive
  types add on top is not implemented, so nothing moves through a portal.
  One portal depth, as with sector portals.

### Colour depth

The **Color Format** core option selects the output pixel format: `16bits`
(RGB565, the default and the historical renderer), `24bits (truecolor)`
(XRGB8888) or `30bits (HDR10)`. The truecolor formats are native pipelines,
not a conversion stage — the palette and composed colour tables are built at
the output's channel width, so nothing is quantised to 565 on the way.

Distance light still snaps to the 32 colormaps the DOS engine used, so a lit
wall resolves to the same set of palette colours in every format; what
changes is that those colours are exact rather than rounded to 5/6/5, and
that everything computed *between* them keeps its precision — translucent
and additive surfaces, filtered texture sampling, coloured dynamic-light
tints and the underwater volume all blend at 8 or 10 bits per channel
instead of 5/6, and native-colour art is blitted losslessly.

`30bits` is genuine HDR, not a wider SDR container. The surface carries
PQ-encoded Rec.2020 samples at absolute luminance, so the core — not the
frontend — decides how bright each pixel is. Ordinary content is mapped to
the frontend's paper white setting, which makes an HDR frame match the SDR
one everywhere except where the renderer marks a colour emissive:
self-illuminated sprites (muzzle flashes, plasma, rockets, explosions,
powerups) and brightmapped texels are pushed above SDR white, so they
actually glow on an HDR display. **HDR Emissive Boost** sets how far (off,
2x, 4x or 8x paper white).

Because PQ is strongly non-linear, the read-modify-write blend kernels
convert each channel to its gamma-encoded equivalent, run the same integer
arithmetic the SDR paths use, and convert back — so translucency, fuzz,
water and light tints look identical in every format. Blending clears the
emissive scale, which is what you want: a highlight seen through glass is no
longer a highlight.

HDR10 requires a frontend that presents it natively; PQ samples read as SDR
look badly wrong, so the format is refused rather than silently narrowed and
the core falls back to `24bits`.

Changing the option requires a restart.

### Not supported

- **ZScript** — no support. Mods whose gameplay lives in ZScript won't run it.
- **Interactive portals** — nothing moves through a portal: `Line_SetPortal`
  types 1-3 (teleporter, interactive, static) and `Sector_SetPortal` types 5
  (copy to line) and 6 (interactive) are inert, as are structured UDMF
  portal fields and ACS portal activation.  The portals described above are
  view-only.
- **3D models (MODELDEF)** — out of scope for the 8-bit software renderer.

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
