# Native Renderer Plan

The target is a full Samurai Warriors 2 Empires native renderer, not only a post-process layer on
top of the existing generic Xenos translation path.

## Current Graphics Boundary

ReXGlue sets up the guest GPU through `rex::graphics::GraphicsSystem`. Guest MMIO writes update the
GPU register file, and the command processor consumes the ring buffer on the `GPU Commands` thread.

Important entry points:

| Area | Current code | Native-renderer relevance |
| --- | --- | --- |
| GPU setup/MMIO | `src/graphics/graphics_system.cpp` | Owns GPU register MMIO, vblank, tracing, and command processor creation. |
| Ring-buffer decode | `src/graphics/command_processor.cpp` | Decodes primary/indirect buffers and packet types. |
| Draw dispatch | `CommandProcessor::ExecutePacketType3Draw` | Reads `VGT_DRAW_INITIATOR`, index-buffer metadata, and calls backend `IssueDraw`. |
| Present/swap | `CommandProcessor::ExecutePacketType3_XE_SWAP` | Reads frontbuffer pointer/size and calls backend `IssueSwap`. |
| Existing D3D12 backend | `src/graphics/d3d12/command_processor.cpp` | Reference for texture cache, render target cache, shader translation, and swap presentation. |
| Existing Vulkan backend | `src/graphics/vulkan/command_processor.cpp` | Second reference backend; useful for separating generic semantics from D3D12-only details. |
| GPU trace writer | `src/graphics/trace_writer.cpp` | Captures packet/memory/register streams to `.xtr` for native-renderer analysis. |

## Direction

Build an SW2E-specific native renderer beside the existing renderer first, then move ownership over
piece by piece. The compatibility renderer stays as the reference path until the native path can draw
known scenes.

The native path should consume the same high-level facts the command processor already exposes:

- PM4 packet stream and draw packet boundaries.
- Xenos register state at each draw.
- Shader guest addresses and shader ucode.
- Texture fetch constants and guest texture memory.
- Index buffers and vertex fetch layouts.
- Resolve/copy operations.
- Swap/frontbuffer selection.

## First Milestones

1. Capture GPU traces for title, menus, scenario select, and an actual battle.
2. Summarize each trace with `tools/summarize_gpu_trace.py` and save CSVs under `extracted\gpu_trace_analysis`.
3. Classify the trace into passes: UI/menu quads, map UI, movie/texture blits, stage geometry, character geometry, effects, shadows, resolves, and final swap.
4. Add a native-renderer sidecar interface that receives decoded draw/swap events without changing generated recomp code.
5. Implement native texture upload/decode for the formats SW2E actually uses.
6. Implement native mesh submission for G1M-derived stage/character geometry.
7. Replace final presentation with native render targets.
8. Add real native AA options once native render targets are owned: MSAA where geometry path supports it, plus TAA/SMAA-style options for alpha/effects/UI.

## Live Native-Render Event Stream

The first sidecar boundary is now a disabled-by-default event tap in the shared ReXGlue command
processor. It runs before the current D3D12/Vulkan backend receives each draw or swap, so it captures
the game's GPU intent without depending on a specific compatibility renderer.

A second, in-process consumer now sits in the project layer at `src/native_renderer`. It registers
through `SamuraiWarriors2EmpiresApp::OnPostSetup` when `--sw2e_native_renderer=true` is passed. This
is deliberately still a sidecar: it observes and classifies live draw/swap events while the existing
renderer keeps presenting the game. The next native-renderer steps should turn this sink into a real
render graph and then begin owning selected draws, textures, and render targets.

Use:

```powershell
.\run_recomp_native_events.bat
```

That launcher writes:

```text
extracted\native_render_events\native_render_events.jsonl
```

Relevant cvars:

| Cvar | Default | Purpose |
| --- | --- | --- |
| `native_render_events` | `false` | Enables the live draw/swap event stream. |
| `native_render_events_path` | `native_render_events.jsonl` | Output path for JSONL events. |
| `native_render_event_limit` | `0` | Caps event count for short captures; `0` means unlimited. |
| `sw2e_native_renderer` | `false` | Enables the SW2E project-side native-renderer sidecar sink. |
| `sw2e_native_renderer_log_interval` | `60` | Frame interval for sidecar summary logging. |
| `sw2e_native_renderer_hash_memory` | `true` | Hashes guest-memory samples touched by draw fetches. |
| `sw2e_native_renderer_memory_hash_bytes` | `4096` | Maximum sample bytes hashed from each vertex/texture fetch. |
| `sw2e_native_renderer_dump_samples` | `false` | Dumps unique guest-memory samples touched by draw fetches. |
| `sw2e_native_renderer_dump_priority_samples_only` | `false` | When dumping samples, skip ordinary title/menu draws and keep the budget for indexed, strip, stride-8+ model-layout, or multi-texture draws. |
| `sw2e_native_renderer_dump_gap_samples_only` | `false` | When dumping samples, keep only unsupported native layout/transform gap draws. |
| `sw2e_native_renderer_sample_root` | `extracted/native_render_samples` | Output folder for dumped samples. |
| `sw2e_native_renderer_dump_sample_limit` | `256` | Maximum unique samples to dump per run. |
| `sw2e_native_renderer_dump_full_textures` | `false` | Dumps complete footprints for supported texture formats instead of only short hash samples. |
| `sw2e_native_renderer_full_texture_max_bytes` | `8388608` | Maximum bytes allowed for one full texture dump. |
| `sw2e_native_renderer_gpu_replay` | `false` | Captures supported live menu draws and replays them through an in-process native D3D11 path. |
| `sw2e_native_renderer_gpu_replay_path` | `extracted/native_render_samples/native_gpu_replay.bmp` | BMP output path for the D3D11 replay. |
| `sw2e_native_renderer_gpu_replay_draw_limit` | `7` | Maximum title/menu draws captured for the native GPU replay. |
| `sw2e_native_renderer_gpu_replay_live_present` | `false` | Presents the captured title/menu replay in a child D3D11 window inside the game window. |
| `sw2e_native_renderer_gpu_replay_live_present_limit` | `0` | Caps child-window live presents for smoke runs; `0` means unlimited. |
| `sw2e_native_renderer_gpu_replay_suppress_backend_swap` | `false` | Opt-in presenter handoff: after a successful native live replay present of a fully covered frame, suppresses that frame's compatibility backend swap. |
| `sw2e_native_renderer_gpu_replay_complete_on_swap` | `true` | Completes the current native GPU replay batch at each swap so high draw limits can capture frame-shaped supported passes. |
| `sw2e_native_renderer_gpu_replay_include_solid_geometry` | `false` | Opt-in capture/replay for solid rectangle-list and triangle-strip title/menu families. |

Each `draw` line records frame/draw index, primitive type, index-buffer metadata, render-target
register state, scissor/window state, active vertex/pixel shader hashes, shader microcode size,
vertex binding/attribute counts, texture binding counts, constant usage, memexport masks, compact
render-state summaries, compact float4 constant snapshots, compact vertex fetch summaries, and
compact texture fetch summaries. Render-state summaries include raw RB color/depth/blend registers,
ReXGlue's normalized color mask, normalized depth control, pixel shader color-target mask, and all
four RT blend controls. The constant snapshots are capped to the first eight referenced float4
registers per shader stage and include both readable float values and raw bit patterns. Vertex
summaries include fetch constant, guest buffer address/size, stride, attribute count, and compact
per-attribute declarations: Xenos data format, word offset, exponent adjust, signed/integer flags,
shader result target/index, write mask, used source components, and result swizzle. Texture
summaries include fetch constant, guest base/mip addresses, dimensions, format, tiling, pitch, and
dimension type. Each `swap` line records the frontbuffer pointer and dimensions plus the number of
submitted draws in that frame.

The sidecar logs frame-level summaries with draw counts, indexed draw counts, vertex/texture fetch
counts, guest vertex-buffer ranges, frontbuffer pointer/dimensions, and the dominant shader pair for
the frame. A short muted smoke run on July 17 verified `--native_render_events=false` and
`--sw2e_native_renderer=true` together: the game produced 600+ swaps through the sidecar, texture
fetches appeared after the early title frames, and the sidecar unregistered cleanly during shutdown.
The only GPU error in that smoke log occurred after `Window closing` and `Execution complete`, so it
is currently treated as close-during-frame noise rather than a sidecar failure.

The sidecar can now also receive a read-only guest-memory context for each draw. It samples and hashes
the physical memory backing vertex fetches, texture fetches, and indexed draw buffers, then logs
`vmem_hash`, `tmem_hash`, `imem_hash`, `hashed_vfetches`, `hashed_tfetches`, `hashed_indices`, and
sampled byte counts. This is not native rendering yet, but it is the bridge we need for it: runtime
draw calls can now be correlated to exact guest buffer, index, and texture contents without editing
generated recomp code.

Validation:

```text
runtime.native-sidecar-hash-smoke-20260717-165831.log
```

That run exited cleanly, produced no fatal/assert/error lines, and showed texture-memory hashes once
texture fetches began around the early title/menu frames. Example frame shape:

```text
draws=110 vfetch_draws=14 tfetch_draws=7 vmem_hash=... tmem_hash=... sample_bytes=1252/28672
```

Optional sample dumping is also available for offline decoding. It writes bounded, unique binary
samples named by kind, frame, draw, fetch constant, guest address, sample size, and content hash, plus
a `samples.jsonl` manifest with draw/shader/fetch metadata for each dumped file. Validation:

```text
extracted\native_render_samples\texture-smoke-20260717-170143
```

That run exited cleanly and wrote 128 files: 120 vertex samples and 8 texture samples. The texture
samples came from runtime addresses such as `0x1BFF3000`, `0x1B0E7000`, `0x1B067000`, and
`0x1BFE3000`. These are the first raw runtime bytes for the native texture/vertex replay path.

Index-buffer capture has been added for the gameplay renderer path. For indexed draws, the sidecar
now hashes and optionally dumps the bounded index span actually needed by `index_count`, recording
`kind="index"` manifest rows with base address, full sampled span, element size, format, and endian
mode. Current title/menu captures have no indexed draws, so validation
`runtime.native-index-sample-smoke-20260717-183020.log` shows `indexed=0`, `index_samples=0`,
`hashed_indices=0`, and a manifest with vertex samples only. That is expected for the title path; the
next battle/gameplay capture should now expose the runtime index buffers needed for native mesh
submission. `tools\summarize_native_render_events.py` also reports top index-buffer bases, lengths,
needed bytes, formats, endianness, and counts when indexed draws are present.

Use this launcher for the next manual gameplay/battle capture:

```powershell
.\run_recomp_native_gameplay_capture.bat
```

It keeps the compatibility renderer active and muted, moves the window to a non-primary monitor,
writes bounded vertex/index/texture samples plus `samples.jsonl` to
`extracted\native_render_samples\gameplay_capture`, and leaves the JSON event stream disabled by
default so gameplay runs do not create multi-GB logs. It also enables
`sw2e_native_renderer_dump_priority_samples_only=true`, because the first bounded gameplay manifest
was consumed by title/menu samples before it reached battle draws. If a one-off event stream is
needed, use `run_recomp_native_events.bat`, which is capped by `native_render_event_limit`.

The old large gameplay capture remains useful as evidence: it recorded `810836` draw events and
showed the main missing native-runtime bucket is indexed `triangle_strip` gameplay geometry, not more
title/menu quads.

The first full-texture capture is:

```text
extracted\native_render_samples\fulltex-smoke-20260717-170903
```

That run exited cleanly with no fatal/error/assert lines. It wrote 240 metadata-backed samples,
including 13 full texture footprints. All 13 textures were linear Xenos format `20`, which ReXGlue
maps to `k_DXT4_5` / PC `BC3_UNORM` (`DXT5`). The sidecar metadata now records `dump_full`,
`expected_full_size`, `pc_format`, `row_pitch_bytes`, block counts, and block size so the native
renderer can turn those samples into upload descriptors.

`tools\export_native_texture_dds.py` converts full linear BC3 samples to DDS for inspection:

```powershell
python .\tools\export_native_texture_dds.py `
  .\extracted\native_render_samples\fulltex-smoke-20260717-170903
```

The exported DDS files were accepted by ImageMagick and produced a contact sheet showing recognizable
title/menu art: the SW2E logo, Press Start text, Japan map silhouette, copyright text, and menu paper
backgrounds. This proves the current event tap can capture live guest texture memory and convert at
least one real SW2E runtime texture format into normal PC texture assets.

Current-build title pass catalog:

```text
runtime.native-events-title-current-20260717-173721.log
extracted\native_render_events\title_current_native_render_events_20260717-173721.jsonl
extracted\native_render_events\title_current_native_render_events_20260717-173721.summary.csv
```

That capture exited with code `0`, recorded `2949` draw events and `51` swaps, and produced no error
lines. The draw stream breaks into a few families:

| Count | Shape | Notes |
| ---: | --- | --- |
| `2670` | `point_list`, 1 vertex, non-indexed | Dominant no-fetch/no-texture pair `VS=0xC049A8C9E556F129`, `PS=0x2E372EA28CC404B7`; likely clear/copy/resolve-style work that must be classified before presenter ownership. |
| `130` | `rectangle_list`, 3 vertices, non-indexed | Screen-space rectangle work; needs mapping to native copy/fill/fullscreen paths. |
| `126` | `triangle_list`, 6 vertices, non-indexed | Textured title/menu quads with shader pair `VS=0xDD8FAA33F9FAB9DB`, `PS=0xFA3E1E7C5EC7C961`, one vertex fetch, one pixel texture, and linear format `20`/BC3 textures. This is the first native GPU replay target. |
| `23` | `triangle_strip`, 4 vertices, non-indexed | Small strip family, probably UI/transition work; decode after the triangle-list path. |

`tools\replay_native_menu_draws.py` is the first offline native draw replay. It pairs full texture
samples with the vertex samples from the same frame/draw, decodes the Xenos BC3 texture blocks,
decodes the 24-byte big-endian menu vertices, and alpha-composes the captured triangle-list quads
onto a PC `1280x720` PNG:

```powershell
python .\tools\replay_native_menu_draws.py `
  .\extracted\native_render_samples\fulltex-smoke-20260717-170903 `
  --out .\extracted\native_render_samples\fulltex-smoke-20260717-170903\native_menu_replay.png
```

The current replay draws six live-captured title/menu submissions:

```text
frame=102 draw=78 texture=256x256 rect=(0,0)-(1280,720)
frame=102 draw=79 texture=1024x1024 rect=(282,108)-(998,640)
frame=102 draw=80 texture=1024x512 rect=(290,112)-(990,512)
frame=102 draw=81 texture=512x64 rect=(408,520)-(872,562)
frame=103 draw=82 texture=512x64 rect=(408,520)-(872,562)
frame=103 draw=83 texture=1024x64 rect=(304,640)-(976,688)
```

The output reconstructs the SW2E title screen from guest draw facts outside the compatibility
renderer. It is not yet the runtime renderer replacement, but it proves the native path can already
consume captured vertex buffers, texture memory, and draw order for a known menu pass.

`src\native_renderer\sw2e_native_gpu_replay.cpp` is the first in-process native GPU replay. When
`--sw2e_native_renderer_gpu_replay=true` is used, the live sidecar captures supported menu draws
directly from guest memory, uploads their BC3 textures and converted vertices through D3D11, draws
them with replacement HLSL shaders and alpha blending, reads the render target back, and writes a BMP.
When `--sw2e_native_renderer_gpu_replay_live_present=true` is also passed, it creates a child D3D11
swapchain inside the main game window and presents the captured title pass there as soon as the draw
limit is reached.

Validated run:

```text
runtime.native-gpu-replay-7draw-smoke-20260717-172110.log
extracted\native_render_samples\native_gpu_replay-7draw-20260717-172110.bmp
```

That run captured one seven-draw title pass in-process:

```text
frame=83 draw=78 texture=256x256
frame=83 draw=79 texture=1024x1024
frame=83 draw=80 texture=1024x512
frame=83 draw=81 texture=512x64
frame=83 draw=82 texture=512x64
frame=83 draw=83 texture=1024x64
frame=83 draw=84 texture=1024x64
```

The output is a valid `1280x720` BMP and visually reconstructs the title screen. This is the first
actual PC GPU render path for live SW2E guest draw data.

Validated live-present run:

```text
runtime.native-gpu-live-present-smoke-20260717-172855.log
extracted\native_render_samples\native_gpu_live_present-20260717-172855.bmp
```

That run initialized the child-window swapchain during setup, captured seven title-pass draws from
frames `96-97`, logged `SW2E native GPU live replay presented 7 captured D3D11 draws`, wrote the
readback BMP on shutdown, and exited with code `0`. ImageMagick identifies the BMP as a valid
`1280x720` image, and it visually matches the title screen. The three GPU error lines in that log
are the existing compatibility resolve warnings before the native replay, not a D3D11 replay failure.

The first guarded presenter handoff is also in place. `native_render::EmitSwap` now returns a
sidecar-controlled suppression decision, and `CommandProcessor::ExecutePacketType3_XE_SWAP` skips
the compatibility `IssueSwap` only when the active event sink reports that it already presented a
native frame. The SW2E sidecar sets that flag only after `PresentMenuReplayD3D11Child` succeeds, the
current frame is fully covered by supported native replay output draws, and
`--sw2e_native_renderer_gpu_replay_suppress_backend_swap=true` is passed. The cvar defaults to
`false`, so normal gameplay still uses the known-good backend presentation.

Validation:

```text
runtime.native-gpu-suppress-swap-guard-20260717-182344.log
extracted\native_render_events\native_gpu_suppress_swap_guard-20260717-182344.jsonl
extracted\native_render_events\native_gpu_suppress_swap_guard-20260717-182344.summary.csv
extracted\native_render_samples\native_gpu_suppress_swap_guard-20260717-182344.bmp
```

That muted smoke run exited with code `0`, produced no fatal/error/assert lines, made 12 successful
native live presents, and logged `owns_frame=true, suppress_backend=true` on the first
native-presented swap passes. Once the configured live-present cap was reached, later fully covered
passes logged `owns_frame=true, suppress_backend=false`, which confirms the compatibility backend
resumes when the native presenter is no longer actively presenting.
The readback BMP is a valid nonblank `1280x720` title screen (`mean=44227.7`,
`stddev=8467.81`). The event summary stayed consistent with the earlier title/menu coverage:
`4430` `skip_no_output`, `343` `supported_textured`, `140` `supported_solid`, and `3`
`depth_or_noncolor_output`, with no unsupported visible color-output draw families.

The live presenter child swapchain is now reusable across size changes. If a child window already
exists and the requested target dimensions change, the D3D11 path releases the current back-buffer
RTV, resizes the swapchain, and recreates the RTV instead of trying to create a second child window.
Validation `runtime.native-gpu-live-present-resize-smoke-20260717-182629.log` exited with code `0`,
produced zero fatal/error/assert lines, made four capped live presents after the refactor, and wrote
the valid nonblank `1280x720` title-screen BMP
`extracted\native_render_samples\native_gpu_live_present_resize_smoke-20260717-182629.bmp`
(`mean=44696.9`, `stddev=8453.9`).

The D3D11 replay draw model can now carry native `uint32_t` index buffers. When a replay draw has
indices, the D3D11 path uploads an index buffer and submits it with `DrawIndexed`; existing title/menu
draws without indices still use the original `Draw` path. Validation
`runtime.native-gpu-indexed-replay-dormant-smoke-20260717-183520.log` exited with code `0`, produced
zero fatal/error/assert lines, made four capped live presents, and wrote the valid nonblank
`1280x720` title-screen BMP
`extracted\native_render_samples\native_gpu_indexed_replay_dormant-20260717-183520.bmp`
(`mean=44681.7`, `stddev=8459.19`). This is intentionally dormant until a gameplay capture provides
real indexed SW2E draw families and the matching vertex layout/material semantics.

The native path is now live-present capable for the title/menu quad pass, but still opt-in and
limited. The compatibility renderer still owns the actual game render targets, swap timing, and final
presentation.

Validated repeated live-present run after the pass loop was enabled:

```text
runtime.native-gpu-live-present-repeat-logfix-20260717-173546.log
extracted\native_render_samples\native_gpu_live_present_repeat_logfix-20260717-173546.bmp
```

That run used `--sw2e_native_renderer_gpu_replay_live_present_limit=4`, completed four native
child-window presents, kept capturing additional title/menu passes after the live-present cap, wrote a
valid nonblank `1280x720` BMP (`mean=45137.4`, `min=0`, `max=65535`), and exited with code `0`.
Per-draw capture logs are now capped after the first 32 supported draws; later runs log pass
completion instead. This keeps longer native-render instrumentation runs readable.

Validated swap-batched live-present run:

```text
runtime.native-gpu-swap-batch-verify-20260717-174042.log
extracted\native_render_samples\native_gpu_swap_batch_verify-20260717-174042.bmp
```

That run used `--sw2e_native_renderer_gpu_replay_draw_limit=256` and
`--sw2e_native_renderer_gpu_replay_complete_on_swap=true`. The current binary exited with code `0`,
made two capped child-window presents, completed native replay batches by `swap`, completed zero
batches by `draw-limit`, wrote a valid nonblank `1280x720` BMP (`mean=45259.5`, `min=0`,
`max=65535`), and produced zero error lines. This changes the native replay unit from "first seven
matching draws" to "the supported textured-title draw family collected up to the frame/swap boundary,"
which is the shape needed for eventually taking over the real presenter.

The replay vertex format now carries `COLOR0`, and the D3D11 replay has a shared textured/solid draw
helper used by both child-window live presentation and shutdown BMP readback. With
`--sw2e_native_renderer_gpu_replay_include_solid_geometry=true`, the sidecar also captures:

| Family | Native handling |
| --- | --- |
| `rectangle_list`, 3 guest vertices | CPU-expanded to six screen-space vertices and rendered through a solid-color D3D11 pixel shader. Seven-float vertices use guest RGBA; two-float vertices use the captured first pixel float4 constant. |
| `triangle_strip`, 4 guest vertices | Converted from clip/NDC coordinates to screen pixels, expanded to six vertices, and rendered through the same solid-color path. |

Validated opt-in solid-coverage run:

```text
runtime.native-gpu-solid-coverage-20260717-174803.log
extracted\native_render_samples\native_gpu_solid_coverage-20260717-174803.bmp
```

That run exited with code `0`, logged `30` rectangle captures and `2` strip captures before per-draw
log suppression, made two capped child-window presents, completed logged batches by `swap`, completed
zero batches by `draw-limit`, produced zero error lines, and wrote a valid nonblank `1280x720` BMP
(`mean=16450.8`, `min=0`, `max=65535`). A baseline textured-only run after the same change,
`runtime.native-gpu-textured-baseline-20260717-174839.log`, kept rectangle/strip capture counts at
zero and wrote `extracted\native_render_samples\native_gpu_textured_baseline-20260717-174839.bmp`.
The three error lines in that baseline were the existing compatibility resolve warnings, not native
D3D11 replay failures.

The event stream now carries compact shader float constants, and the native solid replay uses the
first captured pixel constant for two-float solid vertices. Validation:

```text
runtime.native-gpu-constant-solid-20260717-175410.log
extracted\native_render_events\native_gpu_constant_solid-20260717-175410.jsonl
extracted\native_render_events\native_gpu_constant_solid-20260717-175410.summary.csv
extracted\native_render_samples\native_gpu_constant_solid-20260717-175410.bmp
```

That run exited with code `0`, produced zero fatal/error/assert lines, recorded `4916` draw events
and `84` swaps, and wrote a valid `1280x720` BMP. The latest shutdown BMP is mostly black because
the last completed pass at close was a black solid/fade-style family, not the full title composite.
The useful native-renderer result is in the constant summary: textured menu quads expose pixel
constants like `c0=(2,2,2,2)` and `c1=(0,0,0,0)`, while the solid rectangle/strip family exposes
`c0=(0,0,0,1)`. So the old black placeholder happened to match this title/menu solid shader family.

The D3D11 replay now also carries per-draw RT0 write mask and RT0 blend control from the event stream
and creates native blend states from the captured Xenos factors/ops instead of using one hardcoded
alpha-blend state for every draw. Validation:

```text
runtime.native-gpu-renderstate-20260717-180135.log
extracted\native_render_events\native_gpu_renderstate-20260717-180135.jsonl
extracted\native_render_events\native_gpu_renderstate-20260717-180135.summary.csv
extracted\native_render_samples\native_gpu_renderstate-20260717-180135.bmp
```

That run exited with code `0`, produced zero fatal/error/assert lines, recorded `4916` draw events
and `84` swaps, and wrote a valid nonblank `1280x720` BMP that visually reconstructs the SW2E title
screen. The new render-state summary separates the title capture into useful buckets:

| Count | Normalized color mask | Depth | RT0 blend | Meaning |
| ---: | --- | --- | --- | --- |
| `3806` | `0x00000000` | `0x00000000` | `0x07060706` | Dominant no-color-write family; likely copy/resolve or dead/no-op point traffic to classify before presenter ownership. |
| `291` | `0x0000000F` | `0x00724F36` | `0x07060706` | Alpha-blended color writes, used by the visible title/menu texture work. |
| `82` | `0x0000000F` | `0x00008777` | `0x00010001` | Identity-blend color writes. |
| `57` | `0x00000008` | `0x00724F30` | `0x00010001` | Alpha-only writes from one solid/strip family. |

The solid path is still a coverage/debug step, not final correctness: render-target ownership and the
dominant no-color-write point-list/copy/resolve bucket must be classified before rectangle-list,
triangle-strip, and resolve work can be considered pixel-accurate.

The event stream now carries draw-effect summaries from ReXGlue's draw helpers: whether the primitive
is polygonal, whether rasterization can happen, whether the pixel shader is needed, and whether the
output merger has any color/depth/stencil writes. `tools\summarize_native_render_events.py` reports
these as draw-effect signatures and pass families. Validation:

```text
runtime.native-gpu-effects-20260717-180656.log
extracted\native_render_events\native_gpu_effects-20260717-180656.jsonl
extracted\native_render_events\native_gpu_effects-20260717-180656.summary.csv
extracted\native_render_samples\native_gpu_effects-20260717-180656.bmp
```

That run exited with code `0`, produced zero fatal/error/assert lines, recorded `4916` draw events
and `84` swaps, and wrote a valid nonblank `1280x720` BMP. The old dominant no-color-write bucket
is now much less mysterious:

| Count | Family | Draw effects | Native-renderer meaning |
| ---: | --- | --- | --- |
| `2677` | `point_list`, 1 vertex, no fetches/textures | `raster=true`, `pixel_needed=true`, `om_writes=false` | Pixel shader may run, but there are no color/depth/stencil writes, no memexport, and no viz query in this capture. Treat as a skip candidate unless later traces prove a side effect. |
| `1584` | `point_list`, 1 vertex, no fetches/textures | `raster=true`, `pixel_needed=false`, `om_writes=false` | Strong skip candidate for the native presenter path. |
| `169` | Mostly rectangle/point no-output work | `raster=false`, `om_writes=false` | Strong skip candidate. |
| `400` | Textured triangle/menu work | `polygonal=true`, `pixel_needed=true`, `om_writes=true` | Visible native replay priority. |
| `83` | Solid/non-polygonal UI work | `pixel_needed=true`, `om_writes=true` | Solid rectangle/transition priority. |

For the title/menu path, the native renderer should therefore focus on the output-writing families
first and explicitly drop the no-output point-list family in the native replay once the skip rule is
guarded by `om_writes=false`, `memexport=0`, and no active viz-query side effect.

The live sidecar now uses the same classification in frame summaries and requires output-merger
writes before capturing textured or solid D3D11 replay draws. This keeps opt-in solid replay from
accidentally drawing invisible rectangle-list traffic. Validation:

```text
runtime.native-gpu-skipfilter-20260717-181029.log
extracted\native_render_events\native_gpu_skipfilter-20260717-181029.jsonl
extracted\native_render_events\native_gpu_skipfilter-20260717-181029.summary.csv
extracted\native_render_samples\native_gpu_skipfilter-20260717-181029.bmp
```

That run exited with code `0`, produced zero fatal/error/assert lines, recorded `4916` draw events
and `84` swaps, and wrote a valid nonblank `1280x720` BMP using `122` captured D3D11 replay draws.
The new live counters show the native-renderer workload directly. Early title frames can be as small
as `154` total draws with only `5` output-writing draws and `149` skip candidates. Later title/menu
frames settle around `204` total draws, `154` output-writing draws, `50` skip candidates, `0`
memexport draws, and `0` viz-query draws. This is good news for the full-native path: the frame is
mostly normal textured/solid work plus a stable no-output family that can be dropped by rule.

Native replay coverage is now reported both live and offline. The sidecar frame log includes
`native_supported`, `native_tex`, `native_solid`, `depth_only`, and four unsupported-output buckets
for indexed, shape, layout, and texture-format gaps. `tools\summarize_native_render_events.py` writes
the same classification under `native_replay_support`. Validation:

```text
runtime.native-gpu-coverage-20260717-181355.log
extracted\native_render_events\native_gpu_coverage-20260717-181355.jsonl
extracted\native_render_events\native_gpu_coverage-20260717-181355.summary.csv
extracted\native_render_samples\native_gpu_coverage-20260717-181355.bmp
```

That run exited with code `0`, produced zero fatal/error/assert lines, recorded `4916` draw events
and `84` swaps, and wrote a valid nonblank `1280x720` BMP. The offline coverage summary was:

| Count | Coverage bucket | Meaning |
| ---: | --- | --- |
| `4430` | `skip_no_output` | Guarded no-output traffic; no color/depth/stencil writes, no memexport, no viz query. |
| `343` | `supported_textured` | Color-writing title/menu BC3 triangle-list draws supported by native D3D11 replay. |
| `140` | `supported_solid` | Color/alpha-writing rectangle/strip draws supported by native D3D11 replay. |
| `3` | `depth_or_noncolor_output` | Depth/non-color writes still need render-target ownership before they can be handled properly. |

No unsupported color-output families appeared in this title/menu capture. That means the remaining
title/menu gap is not "decode more draw shapes"; it is presenter/render-target ownership, plus the
small depth-only path. Gameplay scenes are a separate coverage problem and must be measured the same
way before claiming full native rendering.

The replay path now has index-buffer plumbing for the gameplay renderer path. `ReplayDraw` can carry
normalized `uint32_t` indices, the D3D11 replay uploads an index buffer and calls `DrawIndexed` when
indices are present, and the live capture can decode indexed triangle-list draws that match the
known 24-byte textured menu vertex layout. Validation:

```text
runtime.native-gpu-indexed-capture-dormant-smoke-20260717-183929.log
extracted\native_render_events\native_gpu_indexed_capture_dormant-20260717-183929.jsonl
extracted\native_render_events\native_gpu_indexed_capture_dormant-20260717-183929.summary.csv
extracted\native_render_samples\native_gpu_indexed_capture_dormant-20260717-183929.bmp
```

That run exited with code `0`, produced zero fatal/error/assert lines, made four capped native live
presents, recorded `4916` draw events and `84` swaps, and wrote a valid nonblank `1280x720` BMP.
The title/menu path still reported `native_indexed=0`, so this is a safe dormant capability rather
than proof of native 3D gameplay yet. The next battle capture should show whether SW2E's stage,
character, and effect geometry uses this layout or needs additional vertex decoders.

`tools\summarize_native_render_events.py` now also reports `Top vertex layouts`, grouping primitive,
indexed/non-indexed status, shader pair, fetch constant, stride, attribute count, texture count, and
native replay support bucket. The current title capture reports the expected menu buckets:
`343` supported textured triangle-list draws at `stride_words=6/attrs=3`, `140` supported solid
rectangle/strip draws, and no indexed layouts. This should make the first battle capture immediately
actionable for native mesh decoder work.

The same summary now reports `Top vertex attribute signatures`, which keeps the raw Xenos vertex
declaration evidence beside each layout bucket. Validation
`runtime.native-attribute-smoke-clean-20260717-190157.log` exited cleanly after the timed close
request, with no fatal/assert/crash/exception lines, and wrote:

```text
extracted\native_render_events\native_attribute_smoke-clean-20260717-190157.jsonl
extracted\native_render_events\native_attribute_smoke-clean-20260717-190157.summary.csv
```

That 800-event capped title/menu smoke confirmed the new payload on live draws. Example signature:
`fmt=57@0:si:exp=0 -> t1[1] mask=0xF/used=0x7/swz=0xA88` plus
`fmt=38@3:si:exp=0 -> t1[0] mask=0xF/used=0xF/swz=0x688` for the seven-word solid rectangle family.
The known missing-movie warnings and one existing compatibility resolve warning still appear in this
short title-path smoke; they are not caused by the attribute capture.

A supporting low-limit sample-manifest check wrote the same declarations into dumped vertex sample
metadata under `extracted\native_render_samples\attribute-smoke-20260717-190345\samples.jsonl`. That
forced-stop run wrote 16 bounded vertex samples, and the manifest rows include
`attribute_summary_count` plus the per-attribute declaration array next to each dumped vertex buffer.

The live D3D11 replay path now caches its fixed shaders, input layout, sampler, rasterizer state,
viewport constant buffer, blend states, and BC3 shader-resource views instead of recreating them every
native present. Replay textures carry their source guest base address so repeated UI textures can use
a cheap address/shape key; full byte hashing remains only as a fallback for addressless replay data.
The capture side also shares duplicate texture byte payloads inside a replay pass, then clears that
bounded cache at pass/reset boundaries. Validation `runtime.lean-native-smoke-20260717-225813.log`
exited with code `0`, had no
fatal/assert/crash/exception lines, kept `native_render_events=false`, wrote no new JSON event
capture, and produced the nonblank title-screen BMP
`out\build\win-amd64-debug\extracted\native_render_samples\lean_native_smoke_texture_shared_20260717-225813.bmp`.
The only matched warning/failure lines were the known disabled/missing boot movie probes.

The runtime sidecar now accepts compatible textured Xenos `triangle_strip` draws (`primitive=6`) and
expands them into D3D11 triangle-list indices before replay. This applies to both indexed and
non-indexed strips, skips degenerate strip triangles, and preserves the existing linear-BC3/stride-6
layout gate. Validation `runtime.lean-native-smoke-20260717-230239.log` rebuilt cleanly, exited with
code `0`, kept `native_render_events=false`, wrote no new JSON event capture, and produced the
nonblank title-screen BMP
`out\build\win-amd64-debug\extracted\native_render_samples\lean_native_smoke_strip_20260717-230239.bmp`.
The only fatal/error/assert search hits were the known missing boot-movie warnings.

The gameplay sample launcher now uses priority-only dumping: indexed draws, `triangle_strip` draws,
stride-8+ vertex layouts, or multi-texture draws. This keeps the 512-sample budget focused on the
battle/model evidence needed for native gameplay rendering while leaving the event JSON stream off by
default. Validation `runtime.lean-native-smoke-20260717-230604.log` rebuilt cleanly, exited with code
`0`, wrote no JSON event capture, and produced the nonblank title-screen BMP
`out\build\win-amd64-debug\extracted\native_render_samples\lean_native_smoke_priority_20260717-230604.bmp`.
The only fatal/error/assert search hits were the known missing boot-movie warnings.

The native GPU replay readback now keeps the best completed replay pass for shutdown BMP output
instead of using the most recent completed pass unconditionally. This avoids late empty/fade/shutdown
passes overwriting the useful title/menu evidence. Validation
`runtime.lean-native-smoke-20260717-232129.log` exited with code `0`, kept
`native_render_events=false`, produced no fatal/assert/crash/exception lines, and wrote the nonblank
title-screen BMP
`out\build\win-amd64-debug\extracted\native_render_samples\lean_native_smoke_bestpass_20260717-232129.bmp`
(`1280x720`, mean `0.675269`).

Native replay support classification now keeps the current D3D11 replay honest about the remaining
gameplay gap. Attribute-decodable textured draws are no longer treated as safe to submit unless they
use the known screen-space menu fetch shape (`stride_words=6`, `attribute_count=3`). If a draw has
decodable float position/UV attributes but still needs vertex shader/model-view-projection work, the
live sidecar reports it under
`unsupported_output(indexed/shape/layout/texture/transform)` and the offline summary reports
`unsupported_textured_transform`. The summary tool also recognizes textured `triangle_strip` draw
shapes, matching the runtime strip replay gate. Validation
`runtime.lean-native-smoke-20260717-233955.log` rebuilt cleanly, exited with code `0`, kept
`native_render_events=false`, produced no fatal/assert/crash/exception lines, and wrote the nonblank
title-screen BMP
`out\build\win-amd64-debug\extracted\native_render_samples\lean_native_smoke_transform_bucket_20260717-233955.bmp`
(`1280x720`, mean `0.675269`). Re-summarizing the existing title coverage capture still reports the
expected `343` supported textured and `140` supported solid buckets.

The sidecar now records a bounded top transform-gap family per logged frame. When gameplay hits
`unsupported_textured_transform`, the live log will add one concise `SW2E native transform gap` line
with primitive/indexed state, shader pair, vertex fetch constant, stride, attribute count, texture
count, and first texture format/dimension/tiling. This keeps the next battle capture actionable
without turning on the large JSON event stream. Validation
`runtime.lean-native-smoke-20260717-234514.log` rebuilt cleanly, exited with code `0`, kept
`native_render_events=false`, created no default `native_render_events.jsonl`, and wrote the nonblank
title-screen BMP
`out\build\win-amd64-debug\extracted\native_render_samples\lean_native_smoke_transform_topgap_20260717-234514.bmp`
(`1280x720`, mean `0.675269`). The title path has no transform gaps, so the extra line stayed quiet
there.

For unattended gameplay evidence, use:

```powershell
.\run_recomp_native_transform_probe.bat
```

The probe launches muted, moves the window to a non-primary monitor when available, enables bounded
runtime synthetic Start/A through `--sw2e_auto_probe_input=true`, keeps `native_render_events=false`,
disables sample dumping and GPU replay, then closes the process after the requested duration. Its
summary prints the log path, whether any JSON event file was touched, first native frame summaries,
problem lines, and up to six `SW2E native transform gap` lines. This is the preferred next-step
capture for native gameplay rendering because it targets shader-transform/layout evidence without
external keyboard automation or multi-GB JSON.

The first 75-second probe with OS-level key pulses did not reach gameplay because the game-side
`XamInputGetState` heartbeat stayed at `buttons=0` after the boot helper finished. It still verified
the log path and no-JSON behavior: `runtime.native-transform-probe-20260717-235343.log` recorded
`4256` native sidecar frame summaries, no transform gaps, no indexed draws, and no event JSON writes.

Validation `runtime.native-transform-probe-20260718-000127.log` rebuilt cleanly and exited with code
`0`. It kept `native_render_events=false`, touched no JSON event file, and used direct
`XamInputGetState` probe pulses. That run recorded `3103` native sidecar frame summaries, including
`1466` gameplay-class frames with indexed draws. The first indexed gameplay frame had `1477` total
draws, `282` indexed draws, `1453` vertex-fetch draws, `1424` texture-fetch draws, and unsupported
output buckets:

```text
unsupported_output(indexed/shape/layout/texture/transform)=0/5/729/0/677
```

The top transform-gap family was stable across all `1466` transform-gap lines:

```text
prim=6 indexed=false VS=0xd5ccd0c915ddcc0b PS=0x7b81c162cba6d195
vfetch_c=95 stride_words=9 attrs=4 attr_sig=0x5d8c9d1f8fea13a1
a0=fmt57@w0->t1i7m15u7s2696 a1=fmt57@w3->t1i4m7u7s2184
a2=fmt6@w6->t1i1m15u15s90 a3=fmt37@w7->t1i0m3u3s2312
tfetches=5 tex0_format=20 tex0_dim=1 tex0_tiled=0
```

This is likely model-space gameplay geometry: two 3-float attributes at word offsets `0` and `3`,
one packed/unknown attribute at word offset `6`, and one 2-float attribute at word offset `7`.
The next native renderer implementation step is to decode this stride-9 family through shader
constants/model-view-projection instead of forcing it through the current screen-space menu path.

The same compact logger now covers layout gaps. Validation
`runtime.native-transform-probe-20260718-000517.log` kept `native_render_events=false`, touched no
JSON event file, reached `Execution complete`, and recorded `3135` native sidecar summaries. It found
`1466` transform-gap lines and `1466` layout-gap lines. The top layout family was:

```text
prim=6 indexed=false VS=0xde7f9af93c668314 PS=0x8cbad34fce165328
vfetch_c=95 stride_words=1 attrs=1 attr_sig=0x07e7aa1e6ddfa9a7
a0=fmt36@w0->t1i0m1u1s2336
tfetches=1 tex0_format=20 tex0_dim=1 tex0_tiled=0
```

That looks like a point/strip-driven textured effect or particle-style gameplay family rather than
the main stage mesh layout. The priority order is now: decode the stride-9 transform family for
model-space geometry, then classify whether the stride-1 layout family is effects, billboards, or
another non-mesh pass.

For bounded gap bytes plus an offline mesh preview bridge, use:

```powershell
.\run_recomp_native_gap_sample_probe.bat
```

This is the same muted no-JSON gameplay probe, but it enables
`sw2e_native_renderer_dump_gap_samples_only=true`, hashes/dumps only unsupported native layout or
transform draws, and writes a timestamped `samples.jsonl` under
`extracted\native_render_samples\native_gap_probe_*`. Validation
`runtime.native-transform-probe-20260718-001120.log` exited with code `0`, touched no JSON event
file, wrote `128` bounded sample rows, and split them as `index=53`, `texture=46`, `vertex=29`.
The captured native gap support buckets were `unsupported_transform=123` and `unsupported_layout=5`.

`tools\export_native_gap_obj.py` converts the paired vertex/index rows from that manifest into
simple model-space OBJ previews:

```powershell
python .\tools\export_native_gap_obj.py `
  .\extracted\native_render_samples\native_gap_probe_20260718-001120 `
  --max-draws 10

python .\tools\export_native_gap_obj.py `
  .\extracted\native_render_samples\native_gap_probe_20260718-001120 `
  --support unsupported_layout --max-draws 4
```

That check exported ten transform-gap OBJ previews and two layout-gap OBJ previews. Useful first
targets include indexed stride-9 draw `VS=0x45C4DDDAAA10F75F / PS=0x7703E4142DFBD4D4` with 388
vertices and 239 faces, indexed stride-12 draw `VS=0xED8D12865D27DEBF` with 1365 vertices and 805
faces, and a stride-10 transform draw `VS=0x1A2E173CABDD3E80 / PS=0xA444707D877567A5` with 854
vertices and 1537 faces. These OBJ files are diagnostic geometry previews, not finished native
render output, but they prove the no-JSON runtime probe can hand real gameplay vertex/index data to
Blender-facing tooling.

The sample manifest now carries the compact vertex and pixel float constant snapshots already
available on each live `DrawEvent`. Validation
`runtime.native-transform-probe-20260718-002329.log` exited with code `0`, kept
`native_render_events=false`, touched no JSON event file, and wrote another `128` gap-only sample
rows: `53` index, `46` texture, and `29` vertex. All `128` rows included vertex constants and pixel
constants. The first stride-9 transform sample exposes vertex constants `c0-c7` and pixel constants
`c0`, `c1`, `c254`, and `c255`, giving the native renderer a bounded way to inspect gameplay
transform/material state without the large event stream.

`tools\export_native_gap_obj.py` now writes `gap_obj_manifest.csv` beside the OBJ files with raw
position bounds, shader hashes, constant indices, and a heuristic projection-candidate report. The
projection fields intentionally are not proof of the real Xbox vertex shader. They score possible
four-constant row/column groupings so the next renderer pass has concrete candidates to test in
D3D11 instead of guessing. The fresh run wrote
`extracted\native_render_samples\native_gap_probe_20260718-002329\obj\gap_obj_manifest_projection.csv`;
for example, the first indexed stride-9 draw records raw bounds
`(-19.8618736,-7.46388149,-18.582634)..(130.643448,7.46388197,7.53691673)` and a candidate
`c0,c2,c4,c6` in column-major orientation. Treat that as a lead for shader analysis, not a final
projection formula.

The child swapchain is temporary scaffolding, not the final renderer shape. The full-native target is
to replay/classify enough of the Xbox draw stream that the project-side renderer can own render
targets, frame pacing, final presentation, and native options like AA without depending on the
compatibility renderer for those responsibilities.

## Next Native Renderer Milestones

The sidecar now proves we can see and sample the live Xbox draw stream. The next native-rendering
work should proceed in this order:

1. Replace the child-window live preview with ownership of the real game presenter/swap path for the
   title/menu pass once those families match the compatibility output.
2. Handle the remaining depth/non-color title/menu output path under native render-target ownership.
3. Capture and classify one gameplay/battle scene with the new `native_replay_support` summary,
   including indexed vertex/index-buffer families. Compatible triangle strips can now be replayed;
   the remaining big gameplay gap is decoding the stride-8/9/10 model vertex layouts and shader
   transforms rather than primitive expansion alone. Use gap-only samples and OBJ previews to keep
   that work bounded and visually inspectable.
4. Generalize texture decode/upload beyond the first confirmed linear BC3 path: tiled textures,
   additional Xenos formats, mip tails, arrays, and render-target textures.
5. Decode dumped battle/stage vertex and index samples using the observed fetch layout and compare
   them against exported `G1M_` meshes.
6. Extend the replay path to one stage/character mesh family, still side-by-side with the existing
   renderer.
7. Own render targets and final presentation after known passes can be replayed correctly.
8. Add native AA once render targets are ours, not before: MSAA where geometry allows it, plus a
   post-AA option for alpha/effects/UI.

Summarize a capture with:

```powershell
python .\tools\summarize_native_render_events.py `
  .\extracted\native_render_events\native_render_events.jsonl `
  --csv .\extracted\native_render_events\native_render_events.summary.csv
```

This is the feed the native renderer should consume first: classify passes from these events, then
add texture/vertex-buffer extraction for the shader pairs and render-target states that appear in
real SW2E scenes.

## Trace Capture

Use the trace launcher only for short sessions because streaming traces can grow quickly:

```powershell
.\run_recomp_gpu_trace.bat
```

The trace is written under `traces` as `<title-id>_stream.xtr`. Summarize it with:

```powershell
python .\tools\summarize_gpu_trace.py .\traces\<trace-file>.xtr --csv .\extracted\gpu_trace_analysis\<name>.csv
```

## Immediate Research Questions

- Which PM4 draw opcodes dominate battle gameplay?
- How many swaps and draw packets happen per battle frame?
- Which register ranges change before stage geometry vs UI draws?
- Which texture formats appear in battle and UI?
- Does SW2E rely on memexport, EDRAM resolves, or readback-sensitive effects in normal gameplay?
- Can stage/character G1M assets be correlated to runtime vertex/index buffers cleanly enough to bypass shader translation?

## Guardrails

- Keep generated recomp output untouched.
- Keep the existing renderer bootable until the native path can present a complete frame.
- Capture traces and summaries before replacing behavior.
- Prefer SW2E-specific renderer knowledge over trying to build a new generic Xbox 360 renderer.
