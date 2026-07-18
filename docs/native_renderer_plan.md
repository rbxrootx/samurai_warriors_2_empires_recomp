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
| `sw2e_native_renderer_gpu_replay` | `false` | Captures supported live draws and replays them through an in-process native D3D11 path. |
| `sw2e_native_renderer_gpu_replay_path` | `extracted/native_render_samples/native_gpu_replay.bmp` | BMP output path for the D3D11 replay. |
| `sw2e_native_renderer_gpu_replay_draw_limit` | `7` | Maximum live draws captured before early native GPU replay completion; `0` waits for swap, useful for full supported-pass capture. |
| `sw2e_native_renderer_gpu_replay_live_present` | `false` | Presents the captured title/menu replay in a child D3D11 window inside the game window. |
| `sw2e_native_renderer_gpu_replay_live_present_limit` | `0` | Caps child-window live presents for smoke runs; `0` means unlimited. |
| `sw2e_native_renderer_gpu_replay_suppress_backend_swap` | `false` | Opt-in presenter handoff: after a successful native live replay present of a fully covered frame, suppresses that frame's compatibility backend swap. |
| `sw2e_native_renderer_gpu_replay_complete_on_swap` | `true` | Completes the current native GPU replay batch at each swap so high draw limits can capture frame-shaped supported passes. |
| `sw2e_native_renderer_gpu_replay_include_solid_geometry` | `false` | Opt-in capture/replay for solid rectangle-list and triangle-strip title/menu families. |
| `sw2e_native_renderer_gpu_replay_include_transform_gaps` | `false` | Opt-in capture/replay for experimental gameplay transform-gap draw families. |
| `sw2e_native_renderer_gpu_replay_transform_gaps_only` | `false` | Restricts replay capture to experimental transform-gap draws. |
| `sw2e_native_renderer_gpu_replay_transform_gap_min_vertices` | `32` | Minimum decoded vertex count for unresolved projected transform-gap replay draws. Promoted supported projected families only require a valid triangle. |
| `sw2e_native_renderer_gpu_replay_projected_min_indices` | `0` | Optional minimum expanded index count for projected transform-gap replay draws. |
| `sw2e_native_renderer_gpu_replay_projected_vertex_shader_filter` | empty | Optional hex vertex-shader hash filter for projected transform-gap replay. |
| `sw2e_native_renderer_gpu_replay_projected_pixel_shader_filter` | empty | Optional hex pixel-shader hash filter for projected transform-gap replay. |
| `sw2e_native_renderer_gpu_replay_projection_strategy` | `shader-final-or-heuristic` | Projection strategy for transform-gap replay. `heuristic` keeps the old bounded first-eight constant scorer; `shader-final` uses known final blocks from dumped ucode; `shader-bone0-final` applies the first upstream shader matrix block before the final projection; `shader-skinned-final` evaluates the shared shader's weight/index palette path before the final projection; `shader-final-or-heuristic` compares the older final/heuristic paths. |
| `sw2e_native_renderer_gpu_replay_debug_fit_projected_gaps` | `false` | Normalizes projected transform-gap vertices into visible clip space for debug output. |
| `sw2e_native_renderer_gpu_replay_normalize_projected_gaps` | `false` | Applies the best constant projection first, then normalizes projected XY for visibility diagnostics. |
| `dump_shaders` | empty | ReXGlue GPU cvar. When set to a folder, ReXGlue writes analyzed shader ucode and translated shader files there. Use only in short focused probes. |

Each `draw` line records frame/draw index, primitive type, index-buffer metadata, render-target
register state, scissor/window state, active vertex/pixel shader hashes, shader microcode size,
vertex binding/attribute counts, texture binding counts, constant usage, memexport masks, compact
render-state summaries, compact float4 constant snapshots, compact vertex fetch summaries, and
compact texture fetch summaries. Render-state summaries include raw RB color/depth/blend registers,
ReXGlue's normalized color mask, normalized depth control, pixel shader color-target mask, and all
four RT blend controls. The constant snapshots are capped by the native-render event stream summary
limit (`128` in the current source-SDK build) and include both readable float values and raw bit
patterns. Vertex
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

That is now classified as a constant-selector screen-space quad family rather than a model-space
mesh. Dumped shader ucode shows the single float vertex stream selects one of four constant rows:
`c7..c10` provide screen positions, `c11..c14` provide color, and `c15..c18` provide UVs. The pixel
shader samples `tf0` and modulates against captured constants; the current native replay captures
the geometry, texture, vertex color, and `c19.x` alpha path, while exact pixel constant modulation remains a
follow-up accuracy task.

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

The first opt-in projected gameplay-gap replay is now live:

```powershell
.\run_recomp_native_projected_gap_replay.bat
```

This launcher keeps `native_render_events=false`, writes no large event JSON, and enables only the
experimental D3D11 path for `unsupported_textured_transform` draws. Validation
`runtime.native-transform-probe-20260718-005916.log` exited with code `0`, touched no JSON event
file, wrote
`extracted\native_render_samples\native_projected_gap_replay_20260718-005916.bmp`, and submitted
12 captured D3D11 draws. The BMP is `1280x720` with `370263` nonzero RGB channels out of
`2764800`, mean RGB `16.4959`, and visible character-shaped gameplay geometry. This is the first
native D3D11 output from real gameplay transform-gap meshes.

The important runtime fix was primitive-restart handling for indexed triangle strips. Several useful
gameplay transform-gap draws contain `0xFFFF` restart indices; treating that value as a normal max
vertex index made the replay reject those draws as impossible `vertex_size` requests. The capture
path now ignores restart markers when bounding indexed vertex copies and splits triangle strips at
restart markers before uploading replay indices.

The current projected-gap output deliberately uses `debug_fit_projected_gaps=true`, which chooses a
large raw vertex-axis pair and fits it into clip space for inspection. That makes the mesh visible
and proves the D3D11 gameplay submission path, but it is not the final Xbox vertex-shader transform.
The next step is to replace debug-fit with the real per-draw camera/model/projection constants,
depth state, material shader replacement, and render-target ownership.

Transform-gap replay now keeps the strongest projected gameplay draws until swap when
`transform_gaps_only=true`, instead of completing the pass as soon as the first small draws fill the
draw limit. The ranking favors projected draw kind, decoded vertex count, and expanded index count,
so the output pass now retains larger gameplay meshes such as frame `2850` draw `44`
(`1542` vertices, `2853` indices) and frame `2885` draw `151`
(`854` vertices, `4611` indices).

Validation `runtime.native-transform-probe-20260718-011839.log` used
`-ProjectedGapMode constant-fit`, which applies the current best captured constants and then
normalizes the projected XY bounds for visibility. It kept `native_render_events=false`, touched no
event JSON, wrote
`extracted\native_render_samples\native_projected_gap_replay_20260718-011839.bmp`, and produced a
`1280x720`, 32-bit BMP with `78504` nonzero pixels, mean RGB `10.4241`, and max RGB `182`. Visually
that output is large projected gameplay geometry, not only the earlier raw-axis character debug-fit.

Validation `runtime.native-transform-probe-20260718-012011.log` used
`-ProjectedGapMode constant`, the strict unnormalized constant-projection path. It also exited with
code `0`, touched no event JSON, and wrote
`extracted\native_render_samples\native_projected_gap_replay_20260718-012011.bmp`, a `1280x720`,
32-bit BMP with `170348` nonzero pixels, mean RGB `22.5564`, and max RGB `182`. The image is visible
but badly over/under-projected, which confirms the current heuristic is not the final camera solve.
The next renderer step is to recover the real shader transform semantics rather than relying on
four-constant scoring alone.

Focused projected-gap probes now support shader-hash and index-count filters. Validation
`runtime.native-transform-probe-20260718-013024.log` used:

```powershell
.\run_recomp_native_transform_probe.ps1 `
  -ProjectedGapReplay `
  -ProjectedGapMode constant-fit `
  -ProjectedVertexShader 0xED8D12865D27DEBF `
  -ProjectedGapMinVertices 256 `
  -ProjectedGapMinIndices 1000 `
  -ReplayDrawLimit 8 `
  -DurationSeconds 60 `
  -InitialDelaySeconds 5
```

It exited with code `0`, kept `native_render_events=false`, touched no JSON event file, and wrote
`extracted\native_render_samples\native_projected_gap_replay_20260718-013024.bmp`. The log now
records retained replay draws after pruning. The `ED8D12865D27DEBF / 7703E4142DFBD4D4` family
recurs as two stable stride-12 indexed shapes: `845` vertices with `2415` expanded indices, and
`1542` vertices with `2853` expanded indices. The resulting BMP is nonblank but collapses into a
thin projected streak, proving the family is captured repeatably while also proving the current
four-constant projection is not the real vertex transform.

ReXGlue's built-in shader dump path is now wired into the same safe probe via `-DumpShaders`.
Validation `runtime.native-transform-probe-20260718-013325.log` ran for `30` seconds with the same
`ED8D12865D27DEBF` filter plus `-DumpShaders`. It exited with code `0`, kept event JSON off, wrote
`180` shader dump files totaling about `1.7 MB`, and produced the target files:

```text
shader_ED8D12865D27DEBF.ucode.vert
shader_45C4DDDAAA10F75F.ucode.vert
shader_1A2E173CABDD3E80.ucode.vert
shader_7703E4142DFBD4D4.ucode.frag
```

The `ED8D12865D27DEBF` and `45C4DDDAAA10F75F` vertex shaders share the same transform skeleton:
they fetch the model position/UV, do indexed matrix work through `c[4+a0]`, `c[5+a0]`, and
`c[6+a0]`, then export `oPos` through a final `c0..c3` projection block. The stride-10
`1A2E173CABDD3E80` shader uses a different transform path with final constants `c3..c6` and object
work through `c[7+a0]`, `c[8+a0]`, and `c[9+a0]`. This is the concrete reason the old heuristic
was unstable: the final render path needs shader-specific transform evaluation and more constant
coverage, not just brute-force four-row matrix scoring.

`-ProjectedGapMode shader-final-fit` now routes through `projection_strategy=shader-final` and tests
the final projection block seen in dumped ucode. Validation
`runtime.native-transform-probe-20260718-014425.log` filtered to `VS=0xED8D12865D27DEBF`, exited
with code `0`, kept event JSON off, and wrote
`extracted\native_render_samples\native_projected_gap_replay_20260718-014425.bmp`. The log shows
the retained stride-12 shapes again (`845/2415` and `1542/2853`) with
`source=shader-final-c0-c3` and constants `c0,c1,c2,c3`. The pre-normalization NDC range is finite
but completely outside the clip region, for example
`(-1.5006,-4.9612,3.3707)..(-1.4916,-4.9605,3.3742)`. That is the best current proof that final
projection alone is not enough: native gameplay rendering needs the upstream skin/world phase from
`c[4+a0]..c[6+a0]`.

The project now includes `tools\apply_rexglue_native_render_wide_constants.ps1` for source-SDK
builds. It changes ReXGlue's native-render event stream constant cap from `8` to `128`; the runtime
and project side must both be rebuilt after applying it. The project heuristic remains capped at the
first eight constants, so widening the SDK does not explode the old scorer, but exact shader-guided
lookups can now see the higher palette constants used by gameplay skinning. Validation
`runtime.native-transform-probe-20260718-014734.log` first proved a local `64`-constant patch,
rebuilt cleanly, ran a bounded gap sample probe, kept `native_render_events=false`, touched no event
JSON, and wrote `64` rows to
`extracted\native_render_samples\native_gap_probe_20260718-014734\samples.jsonl`
(`542107` bytes). Every row had `64` vertex float constants and the maximum captured vertex
constant index was `63`; the first `45C4DDDAAA10F75F / 7703E4142DFBD4D4` rows now include constants
`c0..c63`. The later ED8D indexed-vertex check found packed palette offsets up to `93`, requiring
constants through `c99`, so the default source-SDK patch was raised to `128`.

`-ProjectedGapMode shader-bone0-final-fit` now tests the first upstream transform block found in the
`ED8D12865D27DEBF` and `45C4DDDAAA10F75F` shaders before the final `c0..c3` projection. It uses the
constant rows `c6,c5,c4` with the shader-observed `wzxy` swizzle as a deliberate "bone/matrix zero"
diagnostic, not as the final skinning solution.

Validation `runtime.native-transform-probe-20260718-015443.log` filtered to
`VS=0xED8D12865D27DEBF`, exited with code `0`, kept event JSON off, and wrote
`extracted\native_render_samples\native_projected_gap_replay_20260718-015443.bmp`. Candidate logs
show `source=shader-bone0-c4-c6-c0-c3`, upstream constants `c6,c5,c4`, and `inside=1.000` for the
retained stride-12 draws (`845/2415` and `1542/2853`). The BMP is a recognizable projected mesh
rather than the earlier thin streak, proving the upstream shader/world block is necessary and useful.

`-ProjectedGapMode shader-skinned-final-fit` now evaluates the shared ED8D/45C4 shader skeleton's
weight/index path. For `ED8D12865D27DEBF`, the stride-12 vertex layout provides two float weights at
words `3/4` and packed `FMT_8_8_8_8` palette offsets at word `5`; the dumped shader uses those
offsets as `a0` against `c[4+a0]..c[6+a0]` before the final `c0..c3` projection.

Validation `runtime.native-transform-probe-20260718-022724.log` used the `128`-constant source-SDK
patch, filtered to `VS=0xED8D12865D27DEBF`, exited with code `0`, kept event JSON off, and wrote
`extracted\native_render_samples\native_projected_gap_replay_20260718-022724.bmp` using two captured
D3D11 draws. Candidate logs show `source=shader-skinned-c4-c6-c0-c3`,
`upstream=skinned:c4+a0..c6+a0`, `finite=1.000`, and `inside=1.000` for retained draws such as
frame `2868` draws `43/44`. This is the current best proof that native gameplay rendering should be
driven by shader-specific transform replay, not arbitrary matrix guessing.

Validation `runtime.native-transform-probe-20260718-015800.log` filtered to
`VS=0x45C4DDDAAA10F75F`, also exited with code `0`, kept event JSON off, and wrote
`extracted\native_render_samples\native_projected_gap_replay_20260718-015800.bmp`. The candidate
math is finite and inside clip space for the stride-9 family, but the BMP is still stretched. That
means this shader family needs fuller evaluation of the branch/weight/index path from the dumped
ucode, not just the first-matrix diagnostic.

Validation `runtime.native-transform-probe-20260718-022845.log` reran `45C4DDDAAA10F75F` through
`shader-skinned-final-fit` after the `128`-constant rebuild. It wrote
`extracted\native_render_samples\native_projected_gap_replay_20260718-022845.bmp` using four
captured draws, and the candidate metrics stayed finite/inside (`1.000`), but the BMP still renders
as a long thin strip. Treat this hash as a separate draw family until the branch semantics and
texture/layout classification are mapped more exactly.

The projected replay now carries texture format/tiled metadata into D3D11 and supports the first
render-target feedback texture family: Xenos `k_8_8_8_8` (`fmt=6`) tiled 2D textures. The replay
path copies the padded tiled footprint, untile-converts it with the Xenos 2D tiled offset math, and
uploads it as `DXGI_FORMAT_R8G8B8A8_UNORM` alongside the existing linear BC3 path.

Validation `runtime.native-transform-probe-20260718-025213.log` used relaxed ED8D thresholds
(`-ProjectedGapMinVertices 3 -ProjectedGapMinIndices 0 -ReplayDrawLimit 12`), exited with code `0`,
kept `native_render_events=false`, touched no JSON event stream, and wrote
`extracted\native_render_samples\native_projected_gap_replay_20260718-025213.bmp` using eight
captured D3D11 draws. Retained draw logs repeatedly show
`texture_fmt=6 tiled=1 texture=1280x720` for frames such as `2838` draws `43-48/139/140`; no D3D11
texture or SRV creation failures appeared. ImageMagick identified the BMP as `1280x720`
TrueColorAlpha with nonzero mean/standard deviation. This proves the
first tiled render-target texture fetch can enter native replay without the large JSON dump path.

The D5 stride-9 transform family now has its own shader-final candidate. Dumped ucode for
`VS=0xD5CCD0C915DDCC0B / PS=0x7B81C162CBA6D195` shows a direct `oPos` path from position fetch
`r7.xyz1` through `c7,c8,c9,c10`, unlike the shared ED8D/45C4 skinned `c[4+a0]..c[6+a0]` path.
The native replay code now builds direct rows from those constants under
`source=shader-direct-c7-c10`.

Validation `runtime.native-transform-probe-20260718-030209.log` ran:

```powershell
.\run_recomp_native_transform_probe.ps1 `
  -ProjectedGapReplay `
  -ProjectedGapMode shader-final-fit `
  -ProjectedVertexShader 0xD5CCD0C915DDCC0B `
  -ProjectedGapMinVertices 3 `
  -ProjectedGapMinIndices 0 `
  -ReplayDrawLimit 12 `
  -DurationSeconds 30 `
  -InitialDelaySeconds 5
```

It exited with code `0`, kept `native_render_events=false`, touched no event JSON, and wrote
`extracted\native_render_samples\native_projected_gap_replay_20260718-030209.bmp`. Candidate logs
show repeated four-vertex strips with `constants=c7,c8,c9,c10`, `source=shader-direct-c7-c10`,
`finite=1.000`, and retained D3D11 draws using linear BC3 `512x512` textures. The BMP is nonblank
`1280x720` TrueColorAlpha output and visually resolves as a long textured terrain/ground strip.
The first observed D5 quads are outside clip before normalization (`inside=0.000`), so this is a
draw-family/projection unlock and pass-classification aid rather than final native scene rendering.

Follow-up bounded sample `runtime.native-transform-probe-20260718-030541.log` wrote `96` gap rows
with vertex/pixel constants and no event JSON. It confirmed the D5 vertex row includes `c7..c10`
and that each captured non-indexed D5 draw submits only `index_count=4` vertices. The OBJ exporter
now clamps non-indexed previews to that submitted count instead of treating the whole sampled vertex
buffer as one mesh, so D5 gap previews export as four vertices and two faces instead of thousands of
unsubmitted backing-buffer vertices.

Standard native replay now supports the DE7 constant-selector quad family:

```text
VS=0xDE7F9AF93C668314 PS=0x8CBAD34FCE165328
selector stream: stride_words=1, fmt36, one float per vertex
positions: c7..c10
colors:    c11..c14
uvs:       c15..c18
```

Validation `runtime.native-standard-replay-20260718-031228.log` ran a normal no-JSON replay and
wrote `extracted\native_render_samples\native_standard_replay_20260718-031228.bmp`. The frame
summary moved from the earlier `native_supported=12 native_tex=0 native_solid=12` and
`unsupported_output(... layout ...)=729` shape to `native_supported=740 native_tex=728
native_solid=12` with only one remaining layout gap in the same gameplay scene. Retained draw lines
show repeated DE7 captures such as `vertices=4`, `indices=6`, `stride_words=1`, and `texture=32x32`.
The BMP is a valid nonblank `1280x720` replay with visible UI/roster text, proving this family is no
longer a layout wall. The generic textured replay shader now multiplies sampled textures by captured
vertex color/alpha. DE7 captures also have a dedicated texture-lerp pixel mode that carries pixel
`c0` and a per-vertex factor into the D3D11 replay; the current DE7 capture uses the observed
factor-`1` path, so the remaining exactness work is recovering the shader branch/`c20` factor path.

Post-change smoke `runtime.native-standard-replay-20260718-033713.log` ran for 30 seconds with
audio muted, `native_render_events=false`, no sample dumps, standard GPU replay enabled, and the
window mover targeting the nonprimary monitor. The process was closed by the probe and exited `0`.
It wrote `extracted\native_render_samples\native_standard_replay_20260718-033713.bmp` using `64`
captured D3D11 draws; a `24px` grid sanity check over the `1280x720` BMP found `392/1620` nonblack
samples. The same run retained DE7 draws such as `VS=0xde7f9af93c668314 /
PS=0x8cbad34fce165328`, `stride_words=1`, `vertices=4`, `indices=6`, `texture=32x32`, and the
gameplay summary again reached `native_supported=740`, `native_tex=728`, `native_solid=12`. The
compatibility backend still emits non-fatal resolve errors like `k_1_REVERSE` and an occasional
surface-pitch resolve warning, so native render ownership is not complete yet.

The D3D11 replay now carries captured `normalized_depthcontrol` per draw and owns a native
`D24_UNORM_S8_UINT` depth target for both offscreen BMP replay and the live child swapchain. The
depth-state cache maps Xenos depth enable/write/compare bits onto D3D11 compare/write state; stencil
bits are detected and logged but intentionally ignored until the event stream captures stencil
ref/mask registers. Validation `runtime.native-depth-replay-20260718-034526.log` rebuilt the
standard 64-draw replay with the depth target active, exited `0`, and wrote
`extracted\native_render_samples\native_depth_replay_20260718-034526.bmp`. A `24px` grid over that
`1280x720` BMP again found `392/1620` nonblack samples with mean RGB-sum `114.29`, and retained
draw logs showed captured depth states like `depth=0x00724f30` and `depth=0x00724f36`. Capped live
validation `runtime.native-depth-live-present-20260718-034637.log` made two successful native child
swapchain presents, exited `0`, and reported no native live-replay/depth creation failures.

The no-color depth rectangle family is now a first-class native replay bucket:
`supported_depth_only`. Its stable signature is `VS=0x0a6d1dd7767fdf27`,
`PS=0x2e372ea28cc404b7`, `primitive=8`, `index_count=3`, `indexed=false`, `stride_words=7`,
`normalized_depthcontrol=0x00008777`, `pixel_needed=false`, `stencil=true`, `color_mask=0`, and
zero texture fetches. The capture path reuses the solid-rectangle vertex decode, but emits
`ReplayDrawKind::kDepthOnlyTriangles`; the D3D11 replay binds no pixel shader and uses an RT0 write
mask of zero, so only the native depth target is affected. The presenter/ownership guard now also
requires at least one supported visible-color draw before a native pass can present or suppress the
compatibility backend. That matters because validation
`runtime.native-depthonly-guard-20260718-035920.log` captured `frame 28 draw 48` as
`kind=depth_only` and correctly completed the depth-only-only pass with `owns_frame=false`.
Follow-up live validation `runtime.native-depthonly-live-present-20260718-040253.log` exited `0`,
captured the same depth rectangle, made two normal child-window D3D11 presents, reported zero
assertions, zero `[error]` lines, and zero `owns_frame=true` lines, and wrote the nonblank
`extracted\native_render_samples\native_depthonly_live_present_20260718-040253.bmp`
(`1280x720`, sampled `2296/2304` nonblack points, mean RGB `147.91`).

Swap-completed full supported-pass capture is now possible. `sw2e_native_renderer_gpu_replay_draw_limit=0`
used to disable capture because the draw-limit guard returned early; it now means "do not complete
early by draw count, keep capturing until swap." The cvar range was raised from `0..256` to
`0..4096`, so a gameplay frame with hundreds of supported draws can be tested without recompiling.
Validation `runtime.native-swap-unlimited-menu-20260718-041016.log` exited `0`, produced zero
`draw-limit` completions, completed `18` native replay passes by `swap`, and wrote nonblank
`extracted\native_render_samples\native_swap_unlimited_menu_20260718-041016.bmp`. A longer
auto-input validation `runtime.native-swap-unlimited-gameplay-20260718-041111.log` reached the
large gameplay/UI pass and wrote
`extracted\native_render_samples\native_swap_unlimited_gameplay_20260718-041111.bmp` using `751`
captured D3D11 draws. That BMP is `1280x720`, sampled `2304/2304` nonblack points, mean RGB
`102.4`. This is still not full native scene ownership: those frames still report
`unsupported_output(indexed/shape/layout/texture/transform)=0/5/1/0/401` to
`0/5/1/0/423`, so transform/shape work remains before backend swap suppression can be enabled for
the battle scene. The run's `[error]` hits were the known compatibility-backend resolve failures,
not native replay assertions.

The D5 direct-projection family is now promoted out of generic transform-gap accounting as
`supported_projected_transform` when the draw matches `VS=0xD5CCD0C915DDCC0B` /
`PS=0x7B81C162CBA6D195`, has a decodable non-screen-space vertex fetch, no memexport/viz side
effects, and a usable texture fetch. The standard replay path accepts this family alongside
supported textured/solid/depth draws when `sw2e_native_renderer_gpu_replay_include_transform_gaps`
is enabled; unresolved transform gaps still obey the `transform_gap_min_vertices` diagnostic filter,
but promoted D5 draws only require a valid triangle. Validation
`runtime.native-d5-projected-standard-20260718-042928.log` exited `0`, reported zero assertions and
zero native replay failures, and reached repeated gameplay frames with `native_supported=1130`,
`native_tex=728`, `native_solid=12`, `native_depth=10`, `native_projected=380`, and
`unsupported_output(indexed/shape/layout/texture/transform)=0/5/1/0/297`. The offscreen replay wrote
`extracted\native_render_samples\native_d5_projected_standard_20260718-042928.bmp` using `1388`
captured D3D11 draws; the `1280x720` BMP sampled `3072/3072` nonblack points with mean RGB `120.88`.
Visually it is still a projected terrain/strip diagnostic, not a correct full gameplay frame. The
next visible-scene blockers are the indexed stride-11 `VS=0x1C9E2812AEBDBE4E` /
`PS=0x7703E4142DFBD4D4` transform family, the single indexed layout gap, the five shape gaps, and
native ownership of render-target composition.

The indexed stride-11 `VS=0x1C9E2812AEBDBE4E` / `PS=0x7703E4142DFBD4D4` family is now also promoted
as `supported_projected_transform`. The dumped shader path matches the shared skinning skeleton:
position words `0..2`, a single blend scalar at word `3`, packed palette indices at word `4`, and
UV words `9..10`. Native replay decodes `weight0 = 1 - blend`, `weight1 = blend`, applies the
indexed `c[4+a0]..c[6+a0]` palette block, then applies the final `c0..c3` projection block. The
default `shader-final-or-heuristic` projection strategy now automatically routes known shared-skin
projection hashes through this shader-skinned path instead of leaving them on the older matrix
scorer.

Focused validation `runtime.native-transform-probe-20260718-044013.log` exited `0`, kept event JSON
off, reported no assertions and no native replay failures, retained the 1C9E indexed strip family
with `842` vertices and `2088` expanded indices, and wrote
`extracted\native_render_samples\native_projected_gap_replay_20260718-044013.bmp`. The BMP is
nonblank and recognizable as projected gameplay mesh chunks, but it is still diagnostic output, not
a solved full scene. Standard no-JSON validation
`runtime.native-1c9e-projected-standard-20260718-044240.log` exited `0` and reached repeated
gameplay frames with `native_supported=1220`, `native_tex=728`, `native_solid=12`,
`native_depth=10`, `native_projected=470`, and
`unsupported_output(indexed/shape/layout/texture/transform)=0/5/1/0/207`. The replay wrote
`extracted\native_render_samples\native_1c9e_projected_standard_20260718-044240.bmp` using `1388`
captured D3D11 draws; a sampled `1280x720` grid found `3072/3072` nonblack points with mean RGB
`120.88`.

The indexed stride-11 `VS=0x1B2E9C6960B0C86E` / `PS=0xD10452A3E31F9C61` character/weapon family is
now promoted as `supported_projected_transform`. The dumped ucode maps position words `0..2`, blend
word `3`, packed palette indices word `4`, normal/lighting data at word `5`, color/aux data at word
`8`, and UV words `9..10`. Unlike the 1C9E path, this shader builds its skinned source through
`c[15+a0]..c[17+a0]`, reorders the skinned rows as the ucode does, then applies the final
`c11..c14` projection block.

Focused validation `runtime.native-transform-probe-20260718-050328.log` exited `0`, kept event JSON
off, retained the family with `842` vertices and `2088` expanded indices per draw, and wrote
`extracted\native_render_samples\native_projected_gap_replay_20260718-050328.bmp`. The shader-fit
candidate reached `finite=1.000` and `inside=1.000` on repeated draws; the BMP shows a centered
skinned character/armor silhouette, with a 16-pixel sample grid finding `707/3600` nonblack points
and mean RGB sum `71.98`. Standard no-JSON validation
`runtime.native-1b2e-projected-standard-20260718-050931.log` exited `0`, reported no assertion,
fatal, crash, exception, or native replay failure lines, and reached repeated gameplay frames with
`native_supported=1309`, `native_tex=728`, `native_solid=12`, `native_depth=10`,
`native_projected=559`, and `unsupported_output(indexed/shape/layout/texture/transform)=0/5/1/0/117`.
The replay wrote
`extracted\native_render_samples\native_1b2e_projected_standard_20260718-050931.bmp` using `1388`
captured D3D11 draws; the BMP is nonblank across a 16-pixel sample grid, but it is still diagnostic
composition, not a correct full gameplay frame. The matched `[error]` lines in that run were the
known compatibility-backend resolve warnings.

The indexed stride-10 `VS=0xA395C843676E6C8D` / `PS=0x850DBBBA56015D1A` family is now promoted as
`supported_projected_transform`. The dumped ucode maps position words `0..2`, a single packed
palette byte at word `3`, normal-ish data at word `4`, color/aux data at word `7`, and UV words
`8..9`. The transform path applies `c[13+a0]..c[15+a0]`, reorders the skinned source to match the
shader, then applies the `c9..c12` projection block.

Focused validation `runtime.native-transform-probe-20260718-052248.log` exited `0`, kept event JSON
off, reached `finite=1.000` and `inside=1.000`, and wrote
`extracted\native_render_samples\native_projected_gap_replay_20260718-052248.bmp`. Standard no-JSON
validation `runtime.native-a395-projected-standard-20260718-052432.log` exited `0`, reported no
assertion, fatal, crash, exception, or native replay failure lines, and reached repeated gameplay
frames with `native_supported=1348`, `native_tex=728`, `native_solid=12`, `native_depth=10`,
`native_projected=598`, and `unsupported_output(indexed/shape/layout/texture/transform)=0/5/1/0/78`.
The replay wrote
`extracted\native_render_samples\native_a395_projected_standard_20260718-052432.bmp` using `1397`
captured D3D11 draws. The matched `[error]` lines in that run were the known compatibility-backend
resolve warnings.

The indexed stride-9 `VS=0x45C4DDDAAA10F75F` / `PS=0x7703E4142DFBD4D4` family is now promoted as
`supported_projected_transform` behind a strict layout gate: indexed triangle strips, stride `9`,
two vertex attributes, position `fmt57@w0->t1i1`, and UV `fmt37@w7->t1i2`. The dumped ucode shares
the ED8D/1C9E skeleton: `c[4+a0]..c[6+a0]` with the final `c0..c3` projection block. The current
decode uses the observed zero-offset/default-weight path for standard coverage; the richer `b0/b1`
branch and selector semantics remain a future accuracy target.

Focused validations `runtime.native-transform-probe-20260718-053139.log` and
`runtime.native-transform-probe-20260718-053607.log` exited `0`, kept event JSON off, and wrote
`extracted\native_render_samples\native_projected_gap_replay_20260718-053139.bmp` and
`extracted\native_render_samples\native_projected_gap_replay_20260718-053607.bmp`. The fit run
confirmed `finite=1.000` and `inside=1.000`; the unnormalized run showed the expected thin
coverage-only pieces rather than a full scene. Standard no-JSON validation
`runtime.native-45c4-projected-standard-20260718-054002.log` exited `0`, reported no assertion,
fatal, crash, exception, or native replay failure lines, and reached repeated gameplay frames with
`native_supported=1366`, `native_tex=728`, `native_solid=12`, `native_depth=10`,
`native_projected=616`, and `unsupported_output(indexed/shape/layout/texture/transform)=0/5/1/0/61`.
The replay wrote
`extracted\native_render_samples\native_45c4_projected_standard_20260718-054002.bmp` using `1397`
captured D3D11 draws. A pixel diff against the A395 standard BMP found `0` changed pixels, so this
promotion improves coverage without changing the current diagnostic composition.

The indexed stride-9 `VS=0x6B722207E8ECA2B6` / `PS=0xD10452A3E31F9C61` family is now promoted as
`supported_projected_transform` behind an exact four-attribute layout gate: position
`fmt57@w0->t1i6`, normal-ish `fmt57@w3->t1i4`, packed `fmt6@w6->t1i1`, and UV
`fmt37@w7->t1i0`. The vertex shader fetches UV into `r0._xy_`, so the native decoder reads UV
directly from words `7..8` for this layout instead of taking the generic swizzle's first two
components. The transform path applies `c15..c17` to raw position with the shader's `r6.wzxy`
ordering, reorders the skinned source as `{z,x,y}`, and projects through `c11..c14`. Saved samples
also showed `0x00FF` appearing as a local strip separator candidate in this family; native strip
expansion now treats it as an additional restart only for the exact 6B72 layout.

Focused validations `runtime.native-transform-probe-20260718-055958.log` and
`runtime.native-transform-probe-20260718-060209.log` exited `0`, kept event JSON off, and wrote
`extracted\native_render_samples\native_projected_gap_replay_20260718-055958.bmp` and
`extracted\native_render_samples\native_projected_gap_replay_20260718-060209.bmp`. The fit run
retained repeated `6B722207E8ECA2B6 / D10452A3E31F9C61` projected draws; the unfit run shows narrow
weapon/prop-like strips in their real screen positions, so the stretched fit image should not be
mistaken for full-screen coverage. Standard no-JSON validation
`runtime.native-6b72-projected-standard-20260718-060328.log` exited `0`, reported no assertion,
fatal, crash, exception, or native replay failure lines, and reached repeated gameplay frames with
`native_supported=1382`, `native_tex=728`, `native_solid=12`, `native_depth=10`,
`native_projected=632`, and `unsupported_output(indexed/shape/layout/texture/transform)=0/5/1/0/44`.
The replay wrote
`extracted\native_render_samples\native_6b72_projected_standard_20260718-060328.bmp` using `1397`
captured D3D11 draws.

The indexed stride-12 `VS=0xED8D12865D27DEBF` / `PS=0x7703E4142DFBD4D4` family is now promoted as
`supported_projected_transform` behind an exact four-attribute shared-skin layout gate: position
`fmt57@w0->t1i1`, weights `fmt37@w3->t1i0`, packed palette `fmt6@w5->t1i3`, and UV
`fmt37@w10->t1i2`. The native decoder reads position in the shader's `r1.1zyx` / `r1.xywz`
ordering, reads UV directly from words `10..11`, skins through the shared `c[4+a0]..c[6+a0]`
block, and projects through `c0..c3`. Saved samples also showed frequent `0x00FF` strip separators,
so strip expansion treats that value as an additional restart only for the exact ED8D layout.

Focused validations `runtime.native-transform-probe-20260718-061114.log` and
`runtime.native-transform-probe-20260718-061413.log` exited `0`, kept event JSON off, and wrote
`extracted\native_render_samples\native_projected_gap_replay_20260718-061114.bmp` and
`extracted\native_render_samples\native_projected_gap_replay_20260718-061413.bmp`. The fit run shows
textured cloth/banner-like geometry; the unfit run keeps it as a small real screen-space draw rather
than a stretched full-screen artifact. Standard no-JSON validation
`runtime.native-ed8d-projected-standard-20260718-061536.log` exited `0`, reported no assertion,
fatal, crash, exception, or native replay failure lines, and reached repeated gameplay frames with
`native_supported=1390`, `native_tex=728`, `native_solid=12`, `native_depth=10`,
`native_projected=640`, and `unsupported_output(indexed/shape/layout/texture/transform)=0/5/1/0/36`.
The replay wrote
`extracted\native_render_samples\native_ed8d_projected_standard_20260718-061536.bmp` using `1397`
captured D3D11 draws.

The indexed stride-10 `VS=0x6E10B025BC817893` / `PS=0x1C9617B76D4A368A` family is now promoted as
`supported_projected_transform` behind an exact five-attribute stage/building layout gate: position
`fmt57@w0->t1i9`, packed model index `fmt6@w3->t1i1`, normal-ish `fmt57@w4->t1i3`, packed material
`fmt6@w7->t1i2`, and UV `fmt37@w8->t1i1`. The vertex shader fetches `r9.xyz1`, applies the
model block through `c13..c15`, reorders the projected source as `{z,x,y}`, and projects through
`c9..c12`. Saved samples use `0x00FF` strip separators in this family too, so strip expansion treats
that value as an additional restart only for the exact 6E10 layout.

Focused validation `runtime.native-transform-probe-20260718-062457.log` exited `0`, kept event JSON
off, and wrote `extracted\native_render_samples\native_projected_gap_replay_20260718-062457.bmp`;
the debug-fit BMP shows a coherent textured building/stage structure. The unfit validation
`runtime.native-transform-probe-20260718-062613.log` also exited `0` and retained repeated 6E10
draws with valid shader-model candidates, but the retained NDC ranges sit outside the captured
visible clip, so its unfit BMP is black and should be treated as an offscreen geometry validation
rather than visible scene coverage. Standard no-JSON validation
`runtime.native-6e10-projected-standard-20260718-062807.log` exited `0`, reported no assertion,
fatal, crash, exception, or native replay failure lines, and reached repeated gameplay frames with
`native_supported=1398`, `native_tex=728`, `native_solid=12`, `native_depth=10`,
`native_projected=648`, and `unsupported_output(indexed/shape/layout/texture/transform)=0/5/1/0/28`.
The replay wrote
`extracted\native_render_samples\native_6e10_projected_standard_20260718-062807.bmp` using `1397`
captured D3D11 draws.

The indexed stride-12 `VS=0x83BD204594EECAB8` / `PS=0xD10452A3E31F9C61` family is now promoted as
`supported_projected_transform` behind an exact six-attribute weighted officer/character layout
gate: position `fmt57@w0->t1i8`, weights `fmt37@w3->t1i5`, packed skin indices
`fmt6@w5->t1i1`, normal-ish `fmt57@w6->t1i7`, packed material `fmt6@w9->t1i2`, and UV
`fmt37@w10->t1i3`. The vertex shader fetches the two explicit weights, derives the third weight as
`1.0 - w0 - w1`, blends the three `c15..c17` model/skin row groups selected from the packed index
bytes, reorders the projected source as `{z,x,y}`, and projects through `c11..c14`. Saved index
samples use `0x00FF` strip separators, so strip expansion treats that value as an additional restart
only for the exact 83BD layout.

Bounded shader/sample validation `runtime.native-transform-probe-20260718-063146.log` exited `0`,
kept event JSON off, and wrote only 32 capped native samples plus 180 shader dumps. Focused
validation `runtime.native-transform-probe-20260718-063554.log` exited `0` and wrote
`extracted\native_render_samples\native_projected_gap_replay_20260718-063554.bmp`; the debug-fit BMP
shows a coherent textured character/officer mesh. The unfit validation
`runtime.native-transform-probe-20260718-063710.log` also exited `0`, retained visible in-clip 83BD
draws, and wrote `extracted\native_render_samples\native_projected_gap_replay_20260718-063710.bmp`.
Standard no-JSON validation `runtime.native-83bd-projected-standard-20260718-064107.log` exited `0`,
reported no assertion, fatal, crash, exception, or native replay failure lines, and reached repeated
gameplay frames with `native_supported=1406..1407`, `native_tex=728`, `native_solid=12`,
`native_depth=10`, `native_projected=656..657`, and
`unsupported_output(indexed/shape/layout/texture/transform)=0/5/1/0/20`. The replay wrote
`extracted\native_render_samples\native_83bd_projected_standard_20260718-064107.bmp` using `1397`
captured D3D11 draws.

The indexed stride-10 `VS=0xB21C8D7A8DB9B17A` / `PS=0x270B573E744D1ACB` family is now promoted as
`supported_projected_transform` behind an exact five-attribute single-palette strip/effect layout:
position `fmt57@w0->t1i4`, palette byte `fmt6@w3->t1i0`, normal-ish `fmt57@w4->t1i6`, packed
material `fmt6@w7->t1i1`, and UV `fmt37@w8->t1i0`. The vertex shader reads the palette byte through
`r0.y`, applies the selected `c13+a0..c15+a0` model rows, reorders the projected source as `{z,x,y}`,
and projects through `c9..c12`. The UV fetch lands in `r0.zw`, so native replay uses the raw
word-8/word-9 UVs instead of the generic `texcoord[0..1]` path. Saved index samples use `0x00FF`
strip separators, so strip expansion treats that value as an additional restart only for the exact
B21C layout.

Bounded shader/sample validation `runtime.native-transform-probe-20260718-064544.log` exited `0`,
kept event JSON off, and wrote 31 capped sample rows plus 180 shader dumps. Focused debug-fit
validation `runtime.native-transform-probe-20260718-065016.log` exited `0` and wrote
`extracted\native_render_samples\native_projected_gap_replay_20260718-065016.bmp` using one captured
D3D11 draw; the BMP shows a coherent long strip/effect. The unfit validation
`runtime.native-transform-probe-20260718-065130.log` also exited `0`, moved the first transform gap
forward, and wrote `extracted\native_render_samples\native_projected_gap_replay_20260718-065130.bmp`,
but the real-clip replay is black, so this family is treated as valid offscreen/sliver coverage.
Standard no-JSON validation `runtime.native-b21c-projected-standard-20260718-065307.log` exited `0`,
reported no assertion, fatal, crash, exception, or native replay failure lines, and reached repeated
gameplay frames with `native_supported=1408..1409`, `native_tex=728`, `native_solid=12`,
`native_depth=10`, `native_projected=658..659`, and
`unsupported_output(indexed/shape/layout/texture/transform)=0/5/1/0/18`. The replay wrote
`extracted\native_render_samples\native_b21c_projected_standard_20260718-065307.bmp` using `1398`
captured D3D11 draws.

The non-indexed stride-8 `VS=0x3094A52CE2571823 / PS=0x969CA710A35A4251` effect/post quad family is
now mapped behind an exact two-attribute layout gate: position
`fmt38@w0->t1i1m15u15s1672` and UV `fmt37@w4->t1i0m3u3s2312`
(`attr_sig=0xBA6F4B89B307862B`). Ucode validation from
`runtime.native-transform-probe-20260718-065854.log` shows the shader writes clip space directly:
`clip.x = 2 * (vertex.x + c0.x) + c255.x`, `clip.y = -2 * (vertex.y + c0.y) + c255.y`,
`clip.z = vertex.z`, and `clip.w = vertex.w`; sampled constants used `c255=(-1,1,0,0)` and a
small half-pixel-style `c0` offset on some draws. The pixel shader is a multi-tap/effect composite
over `tf0`, so the current native replay treats it as visibility scaffolding, not a final native
post-processing implementation. Later broad no-JSON probes did not hit this pair again on the same
route, but the exact decoder/projection path now exists for the next time the family appears.

The latest capped blocker pass also exposed an indexed transform lead:
`VS=0x2E01DF902B14A323 / PS=0xD10452A3E31F9C61`, primitive `triangle_strip`, `index_count=18`,
texture `c0`, and D104 alpha/color modulation. The vertex ucode fetches `r6.xyz1` from offset 0,
another vector at offset 3, and packed data at offset 6, then applies `c15+a0..c17+a0` model rows
and projects through `c11..c14`. That family is now promoted behind the exact compact layout
`stride_words=7 attrs=3 attr_sig=0xe5158a04df6b7bd3`
(`fmt57@w0->t1i6m15u7s2696`, `fmt57@w3->t1i4m7u7s2184`,
`fmt6@w6->t1i1m15u15s90`) and reuses the proven 6B72 model projection path. The initial UV/color
decode is deliberately conservative because the capped sample only captured the target texture row,
not a clean target vertex row. MSVC validation passed, and two post-patch no-JSON probes
(`runtime.native-transform-probe-20260718-073258.log` and
`runtime.native-transform-probe-20260718-073749.log`) ran without native-render asserts but did not
reproduce the exact `2E01/D104` draw before the probe close. The repeated layout blocker is still
`VS=0x5A550226A224F581` / `PS=0x7703E4142DFBD4D4`, formerly the repeated indexed stride-7 attrs-1
layout blocker, is also now promoted behind exact layout
`stride_words=7 attrs=1 attr_sig=0x9e400b94e9164690`
(`fmt57@w0->t1i1m15u7s85`). The shader fetches position from words 0..2, uses generated
`o0.xy=(0,0)` UV scaffolding, applies `c4..c6` upstream rows through the shared-skin evaluator, and
projects through `c0..c3`. Focused validation
`runtime.native-transform-probe-20260718-075241.log` exited `0`, queued repeated draw-25 projected
replay draws (`12 vertices, 18 indices`), wrote
`extracted\native_render_samples\native_projected_gap_replay_20260718-075241.bmp`, and reported
`LayoutGapCount=0`. The captured route is finite but fully outside the visible clip region
(`inside=0.000`, NDC roughly `x=-3.79..-1.52`, `y=-12.16..-5.04`, `z=3.42..7.48`), so the BMP is
black RGB with alpha only; this is a coverage/projection checkpoint, not a final visible render.
The remaining shape gaps, transform families, and native render-target composition still block full
native scene ownership.

The child swapchain is temporary scaffolding, not the final renderer shape. The full-native target is
to replay/classify enough of the Xbox draw stream that the project-side renderer can own render
targets, frame pacing, final presentation, and native options like AA without depending on the
compatibility renderer for those responsibilities.

## Next Native Renderer Milestones

The sidecar now proves we can see and sample the live Xbox draw stream. The next native-rendering
work should proceed in this order:

1. Replace the child-window live preview with ownership of the real game presenter/swap path for the
   title/menu pass once those families match the compatibility output.
2. Extend depth/stencil ownership beyond the first no-color rectangle family. Depth-only rectangles
   now replay into the native depth target, but real stencil ref/mask semantics and any additional
   no-color draw shapes still need targeted capture before full ownership.
3. Capture and classify one gameplay/battle scene with focused shader filters and bounded shader
   dumps. Compatible triangle strips plus the D5, 1C9E, 1B2E, A395, 45C4, 6B72, ED8D, 6E10, 83BD,
   B21C, 3094, 2E01, and 5A projected/effect families now have native replay paths; the remaining
   big gameplay gaps are positive visual validation for 2E01, visible-fit refinement for 5A, other
   stride-8/9/10/11 model vertex layouts, shader constants, and shader transforms rather than
   primitive expansion alone. Use gap-only samples, compact reject-layout logs, OBJ previews, and
   ucode dumps to keep that work bounded and visually inspectable.
4. Generalize texture decode/upload beyond the first confirmed linear BC3 and tiled `k_8_8_8_8`
   paths: additional Xenos formats, mip tails, arrays, swizzles, and render-target ownership.
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
