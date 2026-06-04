# Tiered RHI Renderer — Design Document

**Status:** Draft for discussion
**Scope:** A Render Hardware Interface (RHI) abstraction for a Doom-family
software/hardware renderer, with capability tiers spanning fixed-function
GL 1.x through Vulkan compute.

---

## 1. Motivation and the one fact that shapes everything

Doom's renderer splits into two stages with very different computational
character:

1. **Scene determination** (CPU). BSP traversal, view/angle clipping,
   and draw-list construction — deciding *what* surfaces are visible and
   assembling per-surface data (texture coords, light level, sprite frame
   selection). This scales with **map/scene complexity** (seg count,
   sprite count). It is a serial, pointer-chasing, data-dependent walk over
   a tiny dataset.

2. **Fill** (CPU in software; GPU in every hardware renderer to date).
   Per-pixel texture sampling, colormap/light remap, and writing pixels.
   This scales with **resolution and overdraw**.

Every existing Doom hardware renderer (including DSDA-doom's GL path)
offloads only stage 2. Stage 1 stays on the CPU in both the software and
the GL renderers; the GL path merely branches at the BSP leaves to build
GPU draw lists instead of running the software column/span drawers.

**Consequence that drives this whole design:** hardware acceleration in
Doom is a "resolution solution," not a "complex-scene solution." It helps
enormously when fill dominates (high resolution, overdraw) and almost not
at all when scene determination dominates (slaughtermaps, dense sprites).
A tiered RHI must be honest about this — the tiers are a **capability**
ladder, not a strict **performance** ladder.

A second fact, equally load-bearing: **Doom's lighting is a table lookup,
not screen-space math.** The lit color of a texel is
`colormap[light_index][texel_index]`. The light index is a function of
sector brightness and distance. This is a palette remap, not per-pixel
computation — which is why it can live in several different places (CPU
texture-build time, a fixed-function texture combiner, or a fragment
shader) and still be pixel-exact. The tiers differ largely in *where* this
lookup happens.

A third fact, often overlooked: **texture filtering/sampling quality is
itself a tier-dependent capability, not a free knob.** The software
renderer can choose its sampling behavior per span/column/patch — point,
linear, rounded, edge-slope, dither, and independent choices for walls vs.
floors vs. sprites/patches — because the CPU drawer owns the texel fetch.
A *fixed-function* GPU (tiers 0a/0b) does **not** have this freedom: the
texture unit offers exactly `GL_NEAREST` (point) and `GL_LINEAR`
(bilinear), and nothing else is expressible. A *programmable* GPU (tiers 1,
2a, 2b) regains it: arbitrary sampling — including bit-exact reproduction
of the software filters, sharp-bilinear, higher-order interpolation, and
per-surface-type filter selection — becomes a shader/compute concern.
Filtering is therefore a third axis the tiers turn on (§4.7), alongside
*where lighting happens* and *who determines visibility*.

---

## 2. Design principles

- **P1 — Capability tiers, not a performance ladder.** Each tier targets a
  class of hardware/API capability. A higher tier is not guaranteed faster
  on all workloads; crossovers are expected and documented (§4.6).

- **P2 — Pixel-exactness to the software renderer is the default
  correctness bar.** Every tier must be able to reproduce the software
  renderer's output (paletted colormap lighting included). Tiers may
  *offer* enhanced modes (smooth light fade, higher-precision color) but
  the reference look must be reachable on every tier.

- **P3 — Scene determination is a swappable stage *above* the RHI line.**
  The single biggest architectural risk (§5) is that the compute tier
  determines visibility itself, while all other tiers consume a CPU-built
  visibility result. The interface must treat "produce drawable surfaces
  for this view" as a replaceable component so the compute tier is a clean
  implementation rather than an interface-warping special case.

- **P4 — Scope each tier tightly.** "Move what makes sense to the GPU" is
  unbounded at Doom's data scale (§4.4). Tiers are defined by *specific*
  capabilities they exploit, not aspirations.

- **P5 — Decisions are grounded in measurement, not workload intuition.**
  The fill-vs-scene-determination fraction on the worst-case target
  (§6) gates whether the higher tiers are worth building.

---

## 3. Architecture overview

```
                +-----------------------------------------------+
                |              Game / playsim                   |
                |   (owns the BSP for line-of-sight, collision) |
                +-----------------------------------------------+
                                  |
                                  v
        +-------------------------------------------------------+
        |   Scene-determination stage  (SWAPPABLE — see P3)     |
        |                                                       |
        |   CPU determiner            |   GPU determiner        |
        |   (BSP walk + clip +        |   (compute culling /    |
        |    draw-list build)         |    binning)             |
        |                             |                         |
        |   used by tiers 0,1,2a      |   used by tier 2b       |
        +-------------------------------------------------------+
                                  |
                                  v  "drawable surface set for this view"
        +-------------------------------------------------------+
        |                     RHI interface                     |
        |   (resource creation, surface submission, present)    |
        +-------------------------------------------------------+
              |            |            |              |
              v            v            v              v
          Tier 0       Tier 1       Tier 2a        Tier 2b
        GL1.x / GS   GL2 / D3D9   VK/D3D11/12     VK compute
        fixed-func    shaders      modern submit  tiled binning
```

The key structural point: the **"drawable surface set"** contract is the
seam. For tiers 0/1/2a it is produced by the CPU determiner. For tier 2b
the determiner is itself on the GPU, so tier 2b consumes geometry buffers
and a view, not a pre-culled surface list. See §5.

---

## 4. The tiers

### 4.0 Tier 0 — Pure rasterization, CPU owns the lighting model

**Target:** OpenGL 1.x, OpenGL ES 1.x, PS2 (GS), and any device that can
texture a depth-tested quad. The minimum-requirement tier.

**What the GPU does:** nothing but rasterize textured, depth-tested
polygons. No shaders, no lighting model on the GPU side.

**Where lighting happens:** on the CPU / in texture space. This tier has a
**hidden sub-boundary** that must be named explicitly, because the two
sub-modes have very different hardware floors:

- **Tier 0a — Pre-lit textures (true floor).** The CPU bakes the colormap
  remap into the texels it uploads; the GPU receives final-color (or final
  palette-resolved) textures and does pure fill. Runs on literally any
  rasterizer (GL 1.1, GLES1, GS). **Cost:** Doom's per-surface distance
  gradient does not bake into a single texture. The choice is either
  flat-lit surfaces (wrong look, trivially portable) or subdividing
  surfaces into per-light-level spans (correct look, but reintroduces
  CPU draw-list/geometry cost — which is the cost we are *not* trying to
  grow). VRAM multiplication (one texture per texture×lightlevel) is **no
  longer a constraint** on modern hardware, but the gradient/geometry
  tradeoff is not a memory problem and does not go away with hindsight.

- **Tier 0b — Paletted texture + fixed-function colormap lookup.** The CPU
  ships unlit paletted textures plus a per-vertex/texcoord *light index*;
  the GPU does the colormap remap as a dependent texture read / texture
  combiner (`ARB_texture_env_combine`, paletted-texture or equivalent).
  Preserves the distance gradient pixel-exact (the light coordinate
  interpolates across the polygon), keeps one copy per texture (no VRAM
  multiplication), and is still fixed-function. **Floor:** roughly GL 1.3
  multitexture / combiner capability — *not* GL 1.1.

> **Decision required:** is Tier 0's mandate "runs on the absolute floor"
> (then 0a is the real tier 0 and 0b is a 0.5), or "runs on fixed-function
> hardware with a combiner" (then 0b is tier 0 and 0a is a portability
> fallback)? Do not let tier 0 silently assume multitexture and drift into
> requiring 1.3 — name the sub-boundary in the capability table.

**Reference point:** this is essentially how 1996-era hardware Doom worked,
minus the 1997 VRAM constraint that pushed the industry to GPU-side
lighting.

**Filtering limit:** tier 0 can only offer the texture unit's hardwired
nearest/bilinear sampling. PrBoom's richer per-surface software filters
(rounded, edge-slope, dither) are not expressible on a fixed-function
backend — see §4.7. This is a feature regression vs. the software renderer
and must be surfaced in the UI, not silently substituted.

### 4.1 Tier 1 — Shaders for the colormap/lighting lookup

**Target:** OpenGL 2.0 / GLSL 1.10, Direct3D 9 (SM2/SM3). Same capability
class.

**What the GPU does:** rasterize + run a fragment shader that reproduces
Doom's colormap lighting per fragment: texture stores the palette index in
a channel, the shader recomputes the light index from depth + a per-surface
light-level uniform, and does a dependent COLORMAP texture lookup. Pixel-
exact to software; optionally offers smooth (interpolated) light fade that
software cannot do.

**Where lighting happens:** on the GPU, as a table lookup expressed in a
shader (not as "real" lighting math).

**Status:** this is a known-good design — it is what DSDA-doom's
indexed-lightmode renderer already ships, which gives us a proven,
pixel-exact reference implementation to validate against.

**Critical honesty (P1):** Tier 1 offloads only fill+colormap. Its CPU
frontend is identical to tier 0's. Therefore **tier 1 does not beat tier 0
on CPU-bound (complex-scene) workloads** — it only wins where fill
dominates (high resolution, overdraw). Expect and document the crossover.

**Why not just require shaders and drop tier 0?** Because the GL 2.0 floor
(forced by NPOT) means most shader-capable hardware can also run tier 1 —
so tier 0 buys little *hardware reach*. But tier 0 survives for two
non-hardware reasons: (1) driver robustness — drivers that advertise shader
extensions but miscompile or software-emulate GLSL (old Intel, virtualized
GL, software GL); (2) speed-over-fidelity on weak-but-shader-capable GPUs,
where fixed-function fill is cheaper than the colormap shader. Keeping tier
0 is therefore a **platform-support decision**, not a hardware-tier
necessity. For modern-desktop-only targets, tier 0 can be dropped; for the
libretro long tail (PS2, old ARM, embedded, software GL), it earns its
place.

### 4.2 Tier 2 — Modern explicit APIs

**Target:** Vulkan, Direct3D 11/12, OpenGL 4.5. Split into two
sub-tiers with very different risk profiles.

**Packaging alternative (recommended, validated by §9).** Tiers 2a and 2b
need not be separate *backends*. A single modern backend can expose both as
a **runtime mode switch** — a graphics-pipeline mode (hardware raster +
shader colormap lookup, the 2a/tier-1-equivalent) and a compute mode (the
2b-equivalent). This is simpler to ship and maintain than two backends, and
turns the fidelity-vs-speed tradeoff into a user setting rather than a build
target. The libretro tyrquake RHI (§9) does exactly this with a
`compute_rendering` toggle inside one Vulkan backend, defaulting to compute
for closest fidelity to the software look. Treat "2a vs 2b" below as
*modes* of the high-end backend, not necessarily distinct backends.

#### 4.3 Tier 2a — Modern submission model (+ optional GPU culling)

**Mandate (deliberately narrow, per P4):** exploit a modern API's
*submission* efficiency, not relocate computation. Concretely:

- Persistent / bindless resources; static geometry stays resident,
  only per-frame visibility is updated.
- `drawIndirect` / multi-draw to collapse draw-call and state-change
  overhead.
- **Optional** GPU-driven culling: a compute pass does frustum + coarse
  occlusion over all surfaces in parallel and compacts a draw-indirect
  buffer. This is the *one* legitimate "move work off the CPU" item in 2a —
  it replaces the CPU clipper. At Doom's data scale it is underutilized and
  launch-latency-bound, so the win is modest and may only pay off on
  extreme maps; ship it as an option, measure before defaulting it on.

**What 2a does NOT do:** it does not move the BSP walk or the bulk of
scene determination. Most of the CPU cost does not move cleanly — the BSP
is serial and the dataset is tiny. Resisting scope creep here is the whole
point of the narrow mandate. "Move as much as makes sense" must not become
"rewrite the visibility architecture"; at Doom's scale, very little moves
cleanly, so 2a is "modern submission + optional cull," full stop.

**Where lighting happens:** same colormap-lookup shader as tier 1, ported
to the modern API's shader stage. No change in the lighting model.

#### 4.4 Tier 2b — Compute tiled-binning renderer

**Mandate:** reimplement the rasterizer entirely in compute, using tiled
binning, in the lineage of Themaister's parallel-gs / parallel-rdp /
retrowarp. Geometry and the colormap are uploaded as buffers; the GPU bins
surfaces into screen tiles, then rasterizes per-tile in compute, doing the
colormap lookup inline. Vulkan (or D3D12 compute) only.

**What it buys you:** the *one* architecture that attacks **both** cost
axes — fill *and* the per-surface work — by brute-force parallelism across
thousands of GPU lanes. On the pathological cases (slaughtermaps at 4K,
nuts.wad-class scenes) this is potentially transformative, because it is
the only tier whose throughput is not gated by the serial CPU frontend.
Can also be made bit-deterministic if the binner is designed to reproduce
software output.

**Honest caveat — the analogy is right in technique but not in workload.**
parallel-rdp/parallel-gs emulate a *fixed hardware rasterizer* doing
*substantial* per-frame work, where accuracy is non-negotiable and the CPU
genuinely cannot keep up. Doom's dataset is featherweight — a CPU draws a
normal scene in ~2 ms. A multi-stage compute pipeline (bin → sort →
raster) has fixed dispatch/latency overhead that can make tier 2b **slower
than tier 1 on normal maps**. Tier 2b is therefore the most impressive tier
and the *least often* the right default: brilliant on the slaughtermap/4K
extreme, a potential regression on E1M1. Treat it as a specialist mode, not
the flagship.

**Where lighting happens:** in the compute rasterizer, as the same colormap
table lookup, applied per binned fragment.

### 4.5 Capability summary

| Tier | API floor | GPU does | Lighting location | Filtering | Scene determ. | Best at | Weak at |
|------|-----------|----------|-------------------|-----------|---------------|---------|---------|
| 0a   | GL 1.1 / GLES1 / GS | fill only | CPU texture-bake | nearest / bilinear only | CPU | absolute portability | gradient (flat or geo cost) |
| 0b   | GL 1.3 (combiner) | fill + colormap lookup | FF texture combiner | nearest / bilinear only | CPU | portable + correct look | no custom filters |
| 1    | GL 2.0 / D3D9 | fill + colormap shader | fragment shader | arbitrary (shader) | CPU | high resolution + filters | complex scenes (CPU-bound) |
| 2a   | VK / D3D11-12 / GL4.5 | fill + colormap + submit eff. + opt. cull | shader | arbitrary (shader) | CPU (opt. GPU cull) | draw-call-bound scenes | won't move BSP cost |
| 2b   | VK / D3D12 compute | everything (binned compute raster) | compute lookup | arbitrary (compute) | **GPU** | slaughtermaps @ 4K | light scenes (latency-bound) |

### 4.6 The crossover map (P1, restated as guidance)

- **Low resolution, simple map:** all tiers CPU-bound at ~equal cost; pick
  by portability/maintenance, not speed.
- **High resolution, simple map:** tiers 1/2a/2b win big over tier 0a
  (flat) and over software; fill-bound regime.
- **Low resolution, slaughtermap:** tiers 0/1/2a roughly equal (CPU
  frontend dominates); only tier 2b can move the needle, and only if its
  dispatch overhead is amortized by the scene's size.
- **High resolution, slaughtermap:** the only regime where tier 2b is
  unambiguously the right tool.

### 4.7 Texture filtering as a tier capability

PrBoom's software renderer exposes per-surface filtering — independent
sampling choices for walls (columns), floors/ceilings (spans), and
sprites/patches, across modes such as point, bilinear, "rounded," and
edge-slope/dither variants. This is possible because the CPU drawer owns
the texel fetch and can run any interpolation it likes per surface class.

A hardware tier inherits this freedom only if it has a programmable stage:

- **Tiers 0a / 0b (fixed-function):** filtering is limited to what the
  texture unit hardwires — `GL_NEAREST` (point) and `GL_LINEAR`
  (bilinear). The richer software filters (rounded, edge-slope, dither)
  and per-surface-class selection are **not expressible**. A tier-0 backend
  can match the software renderer's *point* and (approximately) *bilinear*
  output, but cannot reproduce its other filter modes. This is a genuine
  feature regression vs. software for users who rely on those modes — and
  it must be surfaced in the UI as "filter modes unavailable on this
  backend," not silently ignored or silently substituted.

- **Tiers 1 / 2a / 2b (programmable):** filtering moves into the
  fragment/compute stage and becomes arbitrary. These tiers can:
  - reproduce PrBoom's software filters bit-exactly (the sampling math is
    just relocated from the CPU drawer to the shader), satisfying P2 for
    filtering as well as lighting;
  - offer filters software never had (sharp-bilinear, higher-order /
    bicubic, anti-aliased nearest), as opt-in enhancements;
  - select the filter per surface class via a uniform / push-constant or
    per-draw state, preserving PrBoom's wall-vs-floor-vs-patch
    independence.

**Implication for P2 (pixel-exactness):** the "reproduce software output"
bar is fully reachable only on tiers 1+. On tiers 0a/0b it is reachable
*for the point-filtered look* and approximately for bilinear, but the
software renderer's non-trivial filter modes are out of reach by
construction. The capability table (§4.5) records this; the UI must not
present a filter mode the active backend cannot honor.

**Design note:** because tier 1's colormap lookup is *already* a fragment
shader, adding software-filter reproduction there is incremental — the
shader fetches texels (now with the chosen filter) and then does the
colormap lookup, in the same pass. Filtering and lighting share the same
programmable stage and the same "relocate the software math into the
shader" strategy; they are not independent subsystems on tiers 1+.

---

## 5. The hard part: the visibility seam (P3 in depth)

Tiers 0, 1, and 2a share the **entire CPU frontend** — same BSP walk, same
clip, same draw-list build — differing only in how the resulting surfaces
are shaded and submitted. Tier 2b **does not**: it determines visibility on
the GPU and therefore consumes geometry buffers + a view, *not* a
pre-culled surface list.

This means there are two different *interfaces* in play, not two
implementations of one interface:

- Tiers 0/1/2a: `RHI.submit(drawable_surface_set, view)` where the surface
  set was produced on the CPU.
- Tier 2b: `RHI.render(geometry_buffers, view)` where visibility is the
  backend's job.

If the RHI is designed around the first interface and tier 2b is bolted on
later, tier 2b either gets shoehorned awkwardly or forces a leakier, more
abstract interface onto everyone.

**Mitigation (decided up front):** make scene determination a **swappable
stage above the RHI line**, behind a single contract:

```
SceneDeterminer.produce(view) -> DrawableSurfaceSet
```

- `CpuBspDeterminer` implements it with the existing BSP walk + clip +
  draw-list build. Used by tiers 0/1/2a.
- `GpuComputeDeterminer` implements it as part of the tier-2b compute
  pipeline — the binning pass *is* the determiner, and the
  `DrawableSurfaceSet` it produces lives in GPU buffers.

Both satisfy the same contract; the RHI consumes a `DrawableSurfaceSet`
whose backing (CPU list vs. GPU buffer) is opaque to it. Tier 2b becomes a
clean fourth implementation that happens to fuse determination and raster,
rather than a special case that deforms the interface.

The playsim retains its own BSP for line-of-sight and collision regardless
of renderer tier — visibility-for-rendering and visibility-for-gameplay are
separate consumers and must not be conflated.

---

## 6. Grounding step before committing (P5)

Before building tiers 1, 2a, or 2b, take one measurement: on the worst-case
target map at the target resolution, profile the **fill vs.
scene-determination** fraction of frame time.

- If fill dominates → tier 1 is worth building (there is fill to offload),
  and tiers 0a-flat / software are the things it beats.
- If scene determination dominates and fill is small → tier 1 buys little;
  the only tier that helps is 2b, and only if the scene is large enough to
  amortize compute dispatch.
- If *both* are small (normal content) → the higher tiers are about
  resolution headroom and visual enhancements (smooth fade), not raw speed,
  and that should be stated plainly to set expectations.

This single measurement converts the tier roadmap from architecture
astronomy into a grounded build order. The existing render profiler
(per-phase BSP / planes / masked timing) is the right instrument; extend it
to separate setup/draw-list cost from fill cost explicitly.

---

## 7. Build order (high-end first)

> **Revised.** An earlier draft proposed building the lowest-risk tier
> first (tier 1 as the reference, then down to tier 0, then up to tier 2).
> That order is wrong, for the reason in §7.1 below. Build the highest tier
> you intend to support *first*. The libretro tyrquake RHI (§9) is a working
> instance of this order and validates it.

1. **RHI interface + `SceneDeterminer` contract (§5).** Get the seam right
   first; everything else plugs into it. Implement `CpuBspDeterminer`
   against the existing frontend with no behavior change.
2. **The highest tier you intend to support, built first** — tier 2
   (modern API). Within it, the graphics-pipeline mode (tier-1-equivalent:
   shaders for the colormap lookup, pixel-exact to software) is the
   correctness reference and the simpler of the two modes, so stand it up
   first; then the compute mode (tier-2b-equivalent). Expose the two as a
   runtime mode switch within the one backend (§4.2 alternative), not as
   separate build targets. **Validate pixel-exactness against the software
   renderer throughout.**
3. **Tier 0b** (fixed-function combiner colormap) — the first deliberately
   *less-capable* backend. Its real job is to test whether the RHI is a
   true abstraction or "the modern backend with extra steps" (§7.2). Reuses
   the colormap correctness model at a lower API floor; good portability win
   for the long tail.
4. **Tier 0a** (pre-lit) only if a genuinely fixed-function-1.1 / GS target
   requires it.
5. **Tier 1 as a standalone GL2/D3D9 backend**, if a target needs the
   shader tier without a modern API available. (Functionally this is the
   graphics-pipeline mode of step 2 re-homed on an older API; build it only
   when a real target demands it.)

Note the tier-2b compute path is *not* deferred to last here, unlike the
earlier draft: when the high-end backend has compute available, its
compute mode is part of step 2, gated by the §6 measurement on whether it
beats the graphics-pipeline mode on the target's worst case.

### 7.1 Why high-end first

The top tier stress-tests the abstraction hardest, so it must shape the
interface. A fixed-function tier-0 backend asks almost nothing of the RHI —
it consumes the same CPU-built draw lists the software renderer already
produces and submits textured quads. Building it first would yield a
deceptively simple vtable that the modern backend then *forces to grow*
(command-buffer brackets, palette/colormap LUT publish, per-surface compute
dispatch, cache-invalidation hooks, per-feature fallback negotiation — all
the things a real high-end backend needs). Building the demanding consumer
first means the lower tiers slot in *underneath* a sufficient abstraction
rather than deforming it. The danger you want to surface early is "the
abstraction was wrong"; the hardest backend reveals that, the easiest
hides it.

### 7.2 Why the abstraction isn't *proven* until the second backend

High-end-first has one trap: while only one backend exists, the vtable is
validated against a single consumer, and an interface shaped by one backend
can silently encode that backend's assumptions even when it looks generic.
The real test of "true RHI vs. Vulkan-with-extra-steps" comes when the
*second* backend lands — and it should be as **different as possible** (a
fixed-function tier-0, not another modern API), because maximal difference
is what exercises whether the seam genuinely abstracts. This is not an
argument against high-end-first; it is an argument that the abstraction
should not be declared proven until a maximally-different second backend
goes in. Designing the RHI as a set of independently NULL-able operations
with per-feature SW fallback (§5, and as tyrquake does) is what keeps that
future second backend a *subset* implementation rather than a rewrite.

---

## 8. Open questions / decisions required

- **D1:** Tier 0 mandate — absolute floor (0a) or fixed-function-with-
  combiner (0b)? Determines the capability table's bottom row.
- **D2:** Is tier 0 in scope at all for the intended targets, or is the
  floor GL 2.0 (tier 1)? (Modern-desktop-only → drop tier 0; libretro long
  tail → keep it.)
- **D3:** Default-on vs. opt-in for tier 2a's GPU culling — pending the §6
  measurement.
- **D4:** Tier 2b determinism requirement — must it reproduce software
  output bit-exactly, or is "looks like software" sufficient? This
  materially changes the binner's complexity.
- **D5:** Which fixed-function combiner path expresses the dependent
  colormap lookup most portably across the specific GLES1/PS2-class targets
  (D1-dependent). Needs prototyping against real capability lists, not
  assumed.
- **D6:** Filtering policy across tiers (§4.7). Which software filter modes
  are reproduced on tiers 1+ (all of them, bit-exact? or a curated subset
  plus new GPU-only modes?), and how the UI degrades the filter menu on
  fixed-function tiers 0a/0b (grey out unavailable modes vs. hide them).
  Confirm the per-surface-class independence (wall/floor/patch) is carried
  through the RHI as per-draw state.

---

## 9. Reference implementation: libretro tyrquake

The libretro tyrquake (Quake 1) core is an in-progress RHI that
independently validates this design's spine. It is a useful reference and a
sanity check on the architecture. Quake is not Doom, but the renderer
shapes are close enough (CPU BSP/PVS walk + surface determination, software
span rasterizer, palette-indexed output) that the lessons transfer
directly. Key correspondences:

- **Vtable RHI with one active backend (`g_rhi`), determination above the
  line.** Matches §3 / §5. Renderer-aware code calls through the vtable and
  never sees the backend API directly; backend selection is centralized and
  each backend is a single TU exporting one vtable instance.

- **Scene determination stays on the CPU even in the compute renderer.**
  This is the design's central claim (§1, P3) and tyrquake confirms it: the
  vtable's 3D dispatch entries take *already-determined* data — projected
  sprite vertices, transformed+lit+clipped alias vertices, CPU-computed turb
  gradients and span lists, per-span sky calls. The CPU runs the equivalent
  of `R_RenderView`; the GPU rasterizes the surfaces it is handed. Full GPU
  determination (a compute rasterizer that *replaces* `R_RenderView`) is
  explicitly marked as future work — exactly the status Appendix A assigns
  to moving determination off the CPU.

- **Palette lookup as a table op on the GPU.** `textured_palette` shaders do
  the `index → RGBA` lookup (Quake's analog of Doom's colormap), published
  through the RHI — matches Appendix B and §4's lighting-placement axis.

- **Pixel-exactness to software as the bar (P2).** The compute mode is
  specified as pixel-identical to the software renderer at the same
  resolution; the GPU path reproduces the software rasterizer rather than
  diverging from it.

- **Per-feature NULL-fallback negotiation.** Each vtable entry is
  independently NULL-able; when NULL the original software path runs
  unchanged. This is a *better* expression of "capability, not a strict
  ladder" (P1) than per-tier flags: a backend can implement compute
  particles but not compute sky and each falls back independently. This
  document's §5 contract should be read as a set of independently
  negotiable operations in this spirit.

- **High-end-first build order (§7).** tyrquake built the demanding Vulkan
  backend first (with graphics-pipeline and compute modes as a runtime
  toggle), reserving enum slots for GL / D3D11 / D3D12 backends to come.
  This is the order §7 now recommends, and the reason the abstraction is
  already shaped by its hardest consumer.

**Where tyrquake differs from this doc's tiering** — and why it is not a
contradiction: it currently ships only the high-end tier, with the
tier-1-equivalent (graphics pipeline) and tier-2b-equivalent (compute) as
two *modes* of one Vulkan backend rather than separate tiers, because the
lower tiers are deferred future work (§7). Its compute path is also
**per-surface dispatch**, not the tiled-binning of §4.4 — which sidesteps
§4.4's launch-latency risk by not binning, at the cost of per-dispatch
overhead. Whether to adopt per-surface dispatch or tiled binning for the
Doom compute mode is an open question (relates to D4) that should be
settled by measurement (§6), not by analogy to either tyrquake or
parallel-rdp.

---

## Appendix A — Why "BSP traversal in compute" is *not* a tier

An early idea was a compute tier that runs the BSP walk itself on the GPU.
This is omitted deliberately. The BSP walk is a serial, data-dependent,
pointer-chasing recursion over a tiny dataset — the worst possible shape
for GPU compute (no parallel width in a single traversal, maximal warp
divergence if forced wide, and the whole walk costs microseconds on the CPU,
less than a compute dispatch's launch latency). The compute-friendly
reframing is not "do the BSP on the GPU" but "do you need the BSP for
*rendering* visibility at all?" — which is exactly what tier 2b answers by
replacing analytic BSP visibility with parallel tiled binning + a depth
buffer. The BSP is retained for the playsim, where it is cheap and correct.

## Appendix B — Why lighting placement is the axis the tiers turn on

The same colormap table lookup appears in every tier, in a different place:

| Tier | Colormap lookup performed |
|------|---------------------------|
| 0a   | CPU, baked into uploaded texels |
| 0b   | GPU fixed-function texture combiner / dependent read |
| 1    | GPU fragment shader |
| 2a   | GPU fragment shader (modern API) |
| 2b   | GPU compute, per binned fragment |

Because it is a table lookup and not screen-space math, it is pixel-exact
in all five locations. The 1997 reason to prefer GPU-side lighting (VRAM
cost of baking texture×lightlevel) is obsolete; the only remaining reason
the lookup migrates up the tiers is the distance-gradient/portability
tradeoff described in §4.0, not a computational one.
