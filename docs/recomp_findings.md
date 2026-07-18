# Recomp Findings

## Current Boot State

- As of the July 17 working run, the game boots past title/menu and the user confirmed gameplay is
  playable.
- The boot movie workaround is currently enabled. `MEV14.WMV`, `MEV15.WMV`, and `MEV17.WMV` are
  stashed under `game_files\data\movie\_rexglue_disabled_wmv`. Leave them disabled for this build
  unless the WMV path is being tested directly.
- `Escape` previously crashed because it mapped to Start. The known-good launch maps Start and A to
  `P`, runs muted, and lets the runtime window be moved off the primary monitor.
- The generated game code should not be edited by hand. Runtime fixes belong in `src/hooks`, the
  manifest, or the ReXGlue SDK source used for this project.

## Freeze Fixes

- The apparent controller/save freeze was not a dead process. Logs showed the main draw loop and
  input polling still running while the visible frame stopped advancing.
- The first major runtime fatal after the title transition was an invalid/unregistered function call
  to `0x821EDA68`. It is now added to the manifest as `sub_821EDA68`.
- ReXGlue GPU resolve asserts in `src/graphics/util/draw.cpp` were made non-fatal for this game.
  Unsupported resolve commands, bad resolve vertex-buffer descriptors, unsupported MSAA requests,
  and non-color resolve destinations now log and return instead of asserting the process.
- Resolve rectangles wider than the current EDRAM surface pitch still log, but the SDK path clamps
  them and continues. This still appears in logs sometimes and is worth mapping later, but it no
  longer blocks the working boot/gameplay path.
- Synthetic boot input is now opt-in via `--sw2e_auto_boot_input=true`. Normal play no longer
  injects Start/A after launch; smoke tests can still enable the old startup helper when needed.
- Synthetic gameplay probe input is also opt-in via `--sw2e_auto_probe_input=true`. It injects
  bounded Start/A directly through the `XamInputGetState` hook after the boot helper finishes, which
  avoids external keyboard/mouse automation during renderer probes.
- The draw packet hooks in `src/hooks/runtime_hooks.cpp` skip stale transient command cursors before
  they write into read-only guest pages. These are project-layer hooks, not generated-code edits.
- Full native rendering is tracked in `docs/native_renderer_plan.md`. The current tools for that
  path are `tools/summarize_gpu_trace.py`, which summarizes ReXGlue `.xtr` GPU traces captured with
  `run_recomp_gpu_trace.bat`, and `tools/summarize_native_render_events.py`, which summarizes the
  live decoded draw/swap stream captured with `run_recomp_native_events.bat`.

## Native Rendering Progress

- The target is full native PC rendering, not a post-process layer on top of the current D3D12
  compatibility backend.
- A disabled-by-default event tap now lives at the shared ReXGlue command-processor boundary. It
  emits decoded `draw` and `swap` JSONL events before the backend receives them, without touching
  generated recomp code.
- `run_recomp_native_events.bat` launches the game muted, with the same mouse/keyboard bindings as
  the normal launcher, and writes
  `extracted\native_render_events\native_render_events.jsonl`.
- Event cvars:
  `native_render_events`, `native_render_events_path`, and `native_render_event_limit`.
- Each draw event includes frame/draw index, primitive/index metadata, render-target registers,
  scissor/window state, active vertex/pixel shader hashes, shader microcode size, vertex
  binding/attribute counts, texture binding counts, constant usage, memexport masks, compact vertex
  fetch summaries, and compact texture fetch summaries. Each swap event includes frontbuffer pointer,
  size, and frame draw count.
- First smoke capture:
  `extracted\native_render_events\smoke_native_render_events.jsonl`; summary CSV:
  `extracted\native_render_events\smoke_native_render_events.summary.csv`.
- The 12-second smoke capture produced 490 complete draw events and 7 swap events before the timed
  shutdown truncated the final line. The summarizer now ignores a truncated final line by default.
- Early capture pattern: dominant shader pair
  `VS=0xC049A8C9E556F129 / PS=0x2E372EA28CC404B7` handled 468 draws. Primitive usage was mostly
  `point_list` draws with one-index auto draws, plus 21 `rectangle_list` draws and one
  `triangle_strip`.
- First observed swap frontbuffers alternate between `0x1F6F8000` and `0x1F360000` at `1280x720`.
  This is an important anchor for replacing final presentation later.
- Second smoke capture with shader requirement fields:
  `extracted\native_render_events\smoke2_native_render_events.jsonl`; summary CSV:
  `extracted\native_render_events\smoke2_native_render_events.summary.csv`.
- In the second capture, the dominant shader pair
  `VS=0xC049A8C9E556F129 / PS=0x2E372EA28CC404B7` used `24/9` shader dwords, no vertex or texture
  fetch bindings, and `1/0` float constants. Two smaller early shader pairs already show real
  vertex fetches:
  `VS=0x0A6D1DD7767FDF27 / PS=0x2E372EA28CC404B7` has `1` vertex binding and `2` vertex attributes;
  `VS=0x72CBCAA6A7984111 / PS=0x2E372EA28CC404B7` has `1` vertex binding and `1` vertex attribute.
- Third smoke capture with vertex/texture fetch summaries:
  `extracted\native_render_events\smoke3_native_render_events.jsonl`; summary CSV:
  `extracted\native_render_events\smoke3_native_render_events.summary.csv`.
- The third capture recorded real vertex fetch ranges from guest memory, including fetch-constant `0`
  buffers around `0x1FAA007C` / `0x1FAA00D0` and a fetch-constant `95` buffer at `0x1F35E000`.
  No texture fetches appeared in this short startup slice, so a longer menu/gameplay capture is
  needed for native texture upload/decode work.
- The first live project-side native-renderer sidecar now lives in `src\native_renderer` and is
  installed from `SamuraiWarriors2EmpiresApp::OnPostSetup` when `--sw2e_native_renderer=true` is
  passed. It consumes the decoded draw/swap stream in-process without enabling JSON output and
  without changing generated recomp code.
- Sidecar cvars: `sw2e_native_renderer` and `sw2e_native_renderer_log_interval`.
- Sidecar smoke validation:
  `runtime.native-sidecar-smoke-20260717-165327.log`. The run used
  `--native_render_events=false`, `--sw2e_native_renderer=true`, and
  `--sw2e_native_renderer_log_interval=1`. It logged 600+ swap summaries, saw real texture fetches
  after the early frames, and unregistered cleanly during shutdown. The only GPU error appeared after
  `Window closing` and `Execution complete`, so it is currently classified as forced-close timing
  noise, not a native sidecar crash.
- The sidecar now receives a read-only guest-memory context at draw time. With
  `sw2e_native_renderer_hash_memory=true`, it hashes small physical-memory samples from vertex and
  texture fetches and logs `vmem_hash`, `tmem_hash`, `hashed_vfetches`, `hashed_tfetches`, and
  sampled byte counts. Clean validation:
  `runtime.native-sidecar-hash-smoke-20260717-165831.log`; it exited with code `0`, had no
  fatal/assert/error lines, and showed nonzero texture-memory hashes once texture fetches began.
- Optional sample dumping is available behind `sw2e_native_renderer_dump_samples=false` by default.
  `sw2e_native_renderer_sample_root` controls the output folder and
  `sw2e_native_renderer_dump_sample_limit` bounds files per run. It now writes a `samples.jsonl`
  manifest beside the binary samples with draw, shader, fetch, texture, vertex-layout metadata, and
  the native replay support bucket for the sampled draw. `sw2e_native_renderer_dump_priority_samples_only=false`
  can be enabled to keep the sample budget for indexed, `triangle_strip`, stride-8+ model-layout, or
  multi-texture draws instead of title/menu quads. `sw2e_native_renderer_dump_gap_samples_only=true`
  narrows dumps further to unsupported native layout/transform gaps, which is the preferred mode for
  gameplay renderer work.
  Validation:
  `runtime.native-sidecar-texture-dump-smoke-20260717-170143.log` exited cleanly and wrote 128
  bounded samples to
  `extracted\native_render_samples\texture-smoke-20260717-170143`: 120 vertex samples and 8 texture
  samples, including texture guest addresses `0x1BFF3000`, `0x1B0E7000`, `0x1B067000`, and
  `0x1BFE3000`.
- Indexed draw buffer capture is now in the sidecar for the gameplay renderer path. For indexed
  draws, it hashes and optionally dumps the bounded index span implied by `index_count`, records
  `imem_hash`, `hashed_indices`, index range, and index sample bytes in frame summaries, and writes
  `kind="index"` manifest rows with base address, full span, format, element size, and endian mode.
  Validation `runtime.native-index-sample-smoke-20260717-183020.log` exited with code `0`, produced
  zero fatal/error/assert lines, and showed the current title/menu path still has no indexed draws
  (`indexed=0`, `index_samples=0`, `hashed_indices=0`). The smoke manifest contains 64 vertex
  samples and no index samples, which is expected until a battle/gameplay capture reaches real
  indexed geometry. `tools\summarize_native_render_events.py` now also prints and writes CSV rows for
  top index-buffer bases, lengths, needed bytes, formats, endianness, and counts when indexed draws
  are present.
- Vertex fetch summaries now include compact per-attribute declarations from the decoded shader
  fetch instructions. Each vertex fetch can report Xenos data format, word offset, exponent adjust,
  signed/integer flags, shader result target/index, write mask, used source components, and result
  swizzle. `tools\summarize_native_render_events.py` prints these under `Top vertex attribute
  signatures` and writes matching CSV rows. Validation
  `runtime.native-attribute-smoke-clean-20260717-190157.log` exited with code `0` after the timed
  close request, produced no fatal/assert/crash/exception lines, and wrote
  `extracted\native_render_events\native_attribute_smoke-clean-20260717-190157.jsonl` plus
  `extracted\native_render_events\native_attribute_smoke-clean-20260717-190157.summary.csv`. The
  title/menu smoke confirmed signatures such as the seven-word solid rectangle layout:
  `fmt=57@0:si:exp=0 -> t1[1] mask=0xF/used=0x7/swz=0xA88` and
  `fmt=38@3:si:exp=0 -> t1[0] mask=0xF/used=0xF/swz=0x688`. This is the evidence needed to map
  battle vertex/index captures into native input layouts instead of guessing from stride alone.
  A supporting forced-stop sample-manifest check also wrote 16 bounded vertex samples under
  `extracted\native_render_samples\attribute-smoke-20260717-190345`, and its `samples.jsonl` rows
  include the same `attribute_summary_count` and per-attribute declarations beside each dumped vertex
  buffer.
- `run_recomp_native_gameplay_capture.bat` is the safe next capture launcher for battle/gameplay
  renderer research. It keeps compatibility presentation active, runs muted on the non-primary
  monitor, writes bounded vertex/index/texture samples to
  `extracted\native_render_samples\gameplay_capture`, and now leaves the JSON event stream disabled
  by default to avoid multi-GB gameplay logs. Use `run_recomp_native_events.bat` only for intentional
  bounded event-stream captures; that launcher now caps `native_render_event_limit`.
- The existing large gameplay event capture was summarized once with the streaming summary path and
  remains useful evidence: it recorded `810836` draws and `3915` swaps. The main native-runtime gap
  is indexed `triangle_strip` gameplay geometry (`166189` unsupported indexed draws), while title/menu
  textured triangle-list replay is already covered.
- Full texture dumping is now opt-in through `sw2e_native_renderer_dump_full_textures=false` and
  `sw2e_native_renderer_full_texture_max_bytes`. The first validation run,
  `runtime.native-sidecar-fulltex-smoke-20260717-170903.log`, exited cleanly with no
  fatal/error/assert lines and wrote 13 full texture footprints to
  `extracted\native_render_samples\fulltex-smoke-20260717-170903`. All 13 were linear Xenos format
  `20`, which ReXGlue maps to `k_DXT4_5` / PC `BC3_UNORM` (`DXT5`). The new
  `tools\export_native_texture_dds.py` converter exported them to DDS, and ImageMagick decoded them
  into recognizable title/menu art: the SW2E logo, Press Start text, Japan map silhouette,
  copyright text, and menu paper backgrounds.
- `tools\replay_native_menu_draws.py` is the first native replay baseline. It decodes paired
  full-texture and vertex samples from `samples.jsonl`, reconstructs the six captured title/menu
  triangle-list quads from frames `102-103`, and writes a PC-side `1280x720` render to
  `extracted\native_render_samples\fulltex-smoke-20260717-170903\native_menu_replay.png`. The image
  reconstructs the title screen outside the compatibility renderer using captured draw order,
  vertices, BC3 texture memory, and alpha blending. This is still offline software replay, not the
  final runtime renderer replacement.
- `src\native_renderer\sw2e_native_gpu_replay.cpp` is the first in-process native GPU replay.
  Enabled with `--sw2e_native_renderer_gpu_replay=true`, it captures supported live title/menu draws
  directly from guest memory, uploads BC3 textures and converted menu vertices through D3D11, draws
  them with replacement HLSL shaders plus alpha blending, reads the render target back, and writes a
  BMP. Validation:
  `runtime.native-gpu-replay-7draw-smoke-20260717-172110.log` exited with code `0`, captured seven
  title-pass draws from frame `83`, and wrote
  `extracted\native_render_samples\native_gpu_replay-7draw-20260717-172110.bmp`. ImageMagick
  identifies it as a valid `1280x720` BMP, and visually it reconstructs the title screen. The
  compatibility backend still emitted the known resolve warnings during that run; the D3D11 replay
  itself wrote successfully.
- Live native presentation now exists for the same title/menu path behind
  `--sw2e_native_renderer_gpu_replay_live_present=true`. The app passes the Win32 game-window handle
  to the native sidecar, which initializes a child D3D11 swapchain during setup. Once the configured
  seven title-pass draws are captured, the sidecar presents them into that child swapchain, clears the
  capture queue, and can keep presenting later matching title/menu passes. It still writes the last
  completed pass to a readback BMP on shutdown. Validation:
  `runtime.native-gpu-live-present-smoke-20260717-172855.log` exited with code `0`, logged
  `SW2E native GPU live replay presented 7 captured D3D11 draws`, and wrote
  `extracted\native_render_samples\native_gpu_live_present-20260717-172855.bmp`. ImageMagick
  identifies the BMP as valid `1280x720`, and visually it matches the title screen. A repeated-present
  smoke, `runtime.native-gpu-live-present-repeat-logfix-20260717-173546.log`, capped live presentation
  at four presents with `--sw2e_native_renderer_gpu_replay_live_present_limit=4`, exited with code
  `0`, had zero error lines, and wrote the valid nonblank
  `extracted\native_render_samples\native_gpu_live_present_repeat_logfix-20260717-173546.bmp`.
- Native replay batches can now complete at swap boundaries via
  `--sw2e_native_renderer_gpu_replay_complete_on_swap=true`. With
  `--sw2e_native_renderer_gpu_replay_draw_limit=256`, the current-binary smoke
  `runtime.native-gpu-swap-batch-verify-20260717-174042.log` exited with code `0`, made two capped
  child-window presents, completed logged native batches by `swap`, completed zero batches by
  `draw-limit`, had zero error lines, and wrote the valid nonblank
  `extracted\native_render_samples\native_gpu_swap_batch_verify-20260717-174042.bmp`. This is closer
  to a real renderer unit than the original fixed seven-draw sample because the supported textured
  title/menu family is now gathered up to the frame/swap boundary.
- The native D3D11 replay now has an opt-in solid-geometry path behind
  `--sw2e_native_renderer_gpu_replay_include_solid_geometry=true`. It adds `COLOR0` to replay
  vertices, shares one textured/solid draw helper between live presentation and BMP readback, expands
  Xenos `rectangle_list` draws to six screen-space vertices, and expands 4-vertex `triangle_strip`
  draws to six screen-space vertices after converting clip/NDC coordinates. Validation:
  `runtime.native-gpu-solid-coverage-20260717-174803.log` exited with code `0`, logged `30`
  rectangle captures and `2` strip captures before suppression, made two capped live presents, wrote
  the valid nonblank
  `extracted\native_render_samples\native_gpu_solid_coverage-20260717-174803.bmp`, and produced zero
  error lines. The default textured path was rechecked with
  `runtime.native-gpu-textured-baseline-20260717-174839.log`; solid captures stayed off, native replay
  presented/wrote successfully, and its only errors were the known compatibility resolve warnings.
  Solid geometry is still coverage/debug-grade, but the event stream now captures compact vertex and
  pixel float4 constant snapshots. The D3D11 solid path uses the first captured pixel constant for
  two-float solid vertices. Validation
  `runtime.native-gpu-constant-solid-20260717-175410.log` exited with code `0`, produced zero
  fatal/error/assert lines, recorded `4916` draw events and `84` swaps in
  `extracted\native_render_events\native_gpu_constant_solid-20260717-175410.jsonl`, and wrote a
  valid `1280x720` BMP. The summary shows textured menu quads using pixel constants such as
  `c0=(2,2,2,2)` / `c1=(0,0,0,0)`, while the captured solid rectangle/strip family uses
  `c0=(0,0,0,1)`. Next pixel-accuracy work is therefore blend/depth/render-target state and the
  no-fetch copy/resolve bucket, not guessed solid colors.
- Native draw events now include compact render-state summaries: raw RB color/depth/blend state,
  normalized color mask, normalized depth control, pixel shader color-target mask, and RT blend
  controls. The D3D11 replay carries RT0 write mask and RT0 blend control per draw and creates native
  blend states from captured Xenos factors/ops instead of using one hardcoded alpha-blend state for
  every draw. Validation `runtime.native-gpu-renderstate-20260717-180135.log` exited with code `0`,
  produced zero fatal/error/assert lines, recorded `4916` draw events and `84` swaps in
  `extracted\native_render_events\native_gpu_renderstate-20260717-180135.jsonl`, and wrote the
  visually correct title-screen BMP
  `extracted\native_render_samples\native_gpu_renderstate-20260717-180135.bmp`. The new summary
  identifies the dominant `3806`-draw no-color-write family
  (`color_mask=0x00000000`, `rt0_blend=0x07060706`) separately from visible alpha-blended title/menu
  writes (`color_mask=0x0000000F`, `rt0_blend=0x07060706`). That is the next concrete bucket to
  classify for full presenter ownership.
- Native draw events now also carry draw-effect summaries derived from ReXGlue's draw helpers:
  primitive polygonality, rasterization possibility, pixel-shader need, and output-merger writes.
  Validation `runtime.native-gpu-effects-20260717-180656.log` exited with code `0`, produced zero
  fatal/error/assert lines, recorded `4916` draw events and `84` swaps in
  `extracted\native_render_events\native_gpu_effects-20260717-180656.jsonl`, wrote
  `extracted\native_render_events\native_gpu_effects-20260717-180656.summary.csv`, and produced the
  valid nonblank BMP
  `extracted\native_render_samples\native_gpu_effects-20260717-180656.bmp`. The dominant title/menu
  point-list traffic is now classified as no-output work: the top point-list families have no
  vertex/texture fetches, `color_mask=0x00000000`, `depth=0x00000000`, `om_writes=false`, memexport
  masks `0/0`, and `pa_sc_viz_query=0x00000000`. That makes them guarded skip candidates for native
  presenter ownership; the visible work is the much smaller set of output-writing textured
  triangle-list, rectangle-list, and triangle-strip families.
- The live sidecar now logs native workload counters (`om_writes`, `noout_skip`, `noout_point`,
  `raster_noout`, and `viz_query`) and the D3D11 replay refuses to capture textured/solid draws
  unless they have output-merger writes and a nonzero RT0 color write mask. Validation
  `runtime.native-gpu-skipfilter-20260717-181029.log` exited with code `0`, produced zero
  fatal/error/assert lines, recorded `4916` draw events and `84` swaps, wrote
  `extracted\native_render_events\native_gpu_skipfilter-20260717-181029.summary.csv`, and produced
  the valid nonblank BMP
  `extracted\native_render_samples\native_gpu_skipfilter-20260717-181029.bmp` using `122` captured
  D3D11 replay draws. Live summaries now show the renderer workload clearly: a title/menu frame can
  have `204` total draws with `154` output-writing draws and `50` guarded skip candidates, with
  memexport and viz-query counts both `0`.
- Native replay coverage is now available in both live sidecar logs and
  `tools\summarize_native_render_events.py` CSV output. Validation
  `runtime.native-gpu-coverage-20260717-181355.log` exited with code `0`, produced zero
  fatal/error/assert lines, recorded `4916` draw events and `84` swaps, wrote
  `extracted\native_render_events\native_gpu_coverage-20260717-181355.summary.csv`, and produced
  the valid nonblank BMP
  `extracted\native_render_samples\native_gpu_coverage-20260717-181355.bmp`. The offline summary
  classified the capture as `4430` `skip_no_output`, `343` `supported_textured`, `140`
  `supported_solid`, and `3` `depth_or_noncolor_output`, with no unsupported color-output draw
  families. So for the current title/menu capture, the remaining native-renderer gap is
  render-target/presenter ownership plus the tiny depth/non-color path, not unknown visible draw
  shapes. Gameplay/battle still needs its own capture and coverage classification.
- The live D3D11 replay path now caches fixed shaders, input layout, sampler, rasterizer state,
  viewport constant buffer, blend states, and BC3 shader-resource views instead of rebuilding them for
  every native present. Replay textures carry their source guest base address so repeated UI textures
  use a cheap address/shape key, with full byte hashing only as an addressless-data fallback. The
  capture side also shares duplicate texture byte payloads inside a replay pass, then clears that
  bounded cache at pass/reset boundaries. Validation `runtime.lean-native-smoke-20260717-225813.log`
  exited with code `0`, produced no
  fatal/assert/crash/exception lines, kept `native_render_events=false`, wrote no new JSON event
  capture, and produced the nonblank title-screen BMP
  `out\build\win-amd64-debug\extracted\native_render_samples\lean_native_smoke_texture_shared_20260717-225813.bmp`.
  The only matched warning/failure lines were known disabled/missing boot movie probes.
- The runtime sidecar now accepts compatible textured Xenos `triangle_strip` draws (`primitive=6`) and
  expands them into D3D11 triangle-list indices before replay. This works for indexed and non-indexed
  strips, skips degenerate strip triangles, and keeps the existing linear-BC3/stride-6 layout gate.
  Validation `runtime.lean-native-smoke-20260717-230239.log` rebuilt cleanly, exited with code `0`,
  kept `native_render_events=false`, wrote no new JSON event capture, and produced the nonblank
  title-screen BMP
  `out\build\win-amd64-debug\extracted\native_render_samples\lean_native_smoke_strip_20260717-230239.bmp`.
  The only fatal/error/assert search hits were known disabled/missing boot movie warnings. Gameplay's
  large indexed-strip bucket still needs stride-8/9/10 model layout and shader-transform decoding.
- `run_recomp_native_gameplay_capture.bat` now enables
  `sw2e_native_renderer_dump_priority_samples_only=true`. The previous bounded
  `extracted\native_render_samples\gameplay_capture\samples.jsonl` had 5000 rows but was consumed by
  title/menu samples (`3660` stride-6 title quads, no index samples), so this change makes the next
  manual gameplay capture target indexed strips/model layouts without turning JSON events back on.
  Validation `runtime.lean-native-smoke-20260717-230604.log` rebuilt cleanly, exited with code `0`,
  wrote no JSON event capture, and produced the nonblank title-screen BMP
  `out\build\win-amd64-debug\extracted\native_render_samples\lean_native_smoke_priority_20260717-230604.bmp`.
  The only fatal/error/assert search hits were known disabled/missing boot movie warnings.
- Native GPU replay shutdown readback now keeps the best completed pass instead of blindly writing
  the last completed pass. The score strongly prefers textured draws, so useful title/menu evidence
  is not overwritten by late empty/fade/shutdown passes. Validation
  `runtime.lean-native-smoke-20260717-232129.log` exited with code `0`, kept
  `native_render_events=false`, produced no fatal/assert/crash/exception lines, and wrote the
  nonblank title-screen BMP
  `out\build\win-amd64-debug\extracted\native_render_samples\lean_native_smoke_bestpass_20260717-232129.bmp`
  (`1280x720`, mean `0.675269`). This explains the earlier black BMP as a readback-selection issue,
  not a boot/runtime regression.
- Native replay coverage now separates "decodable vertex layout" from "safe to replay with the
  current menu-style shader." The D3D11 replay still only submits screen-space textured fetches
  (`stride_words=6`, `attribute_count=3`); textured draws whose float attributes expose position and
  UV data but still need vertex shader/model-view-projection work are counted as the new transform
  gap instead of being falsely marked supported. Live frame logs now label
  `unsupported_output(indexed/shape/layout/texture/transform)`, and
  `tools\summarize_native_render_events.py` reports the matching
  `unsupported_textured_transform` bucket while also recognizing textured `triangle_strip` shapes.
  Validation `runtime.lean-native-smoke-20260717-233955.log` rebuilt cleanly, exited with code `0`,
  kept `native_render_events=false`, produced no fatal/assert/crash/exception lines, and wrote the
  nonblank title-screen BMP
  `out\build\win-amd64-debug\extracted\native_render_samples\lean_native_smoke_transform_bucket_20260717-233955.bmp`
  (`1280x720`, mean `0.675269`). A summary pass over the existing title coverage capture still
  reports the expected title buckets (`343` supported textured, `140` supported solid) and now names
  the `triangle_strip` primitive family explicitly.
- Live frame summaries now keep a bounded top transform-gap bucket for the next gameplay capture.
  When a frame contains textured draws that have decodable position/UV attributes but are not safe for
  the current screen-space replay, the sidecar emits one extra `SW2E native transform gap` line with
  draw count, primitive type, indexed flag, vertex/pixel shader hashes, vertex fetch constant,
  stride, attribute count, texture count, and first texture format/dimension/tiling. This is designed
  to point directly at the next shader-transform/material family without enabling the multi-GB JSON
  stream. Validation `runtime.lean-native-smoke-20260717-234514.log` rebuilt cleanly, exited with
  code `0`, kept `native_render_events=false`, created no default `native_render_events.jsonl`, and
  wrote the nonblank title-screen BMP
  `out\build\win-amd64-debug\extracted\native_render_samples\lean_native_smoke_transform_topgap_20260717-234514.bmp`
  (`1280x720`, mean `0.675269`). The title path has no transform gaps, so no extra transform-gap
  line was emitted there; the new line should appear only on frames that actually hit that gap.
- `run_recomp_native_transform_probe.bat` is now the preferred no-JSON gameplay renderer probe. It
  launches muted, uses runtime synthetic Start/A rather than host keyboard automation, closes itself,
  and reports the first frame summaries and top transform-gap lines. Validation
  `runtime.native-transform-probe-20260718-000127.log` exited with code `0`, touched no JSON event
  file, and recorded `3103` native sidecar frames. `1466` frames had indexed gameplay-class work; the
  first such frame had `1477` draws, `282` indexed draws, and
  `unsupported_output(indexed/shape/layout/texture/transform)=0/5/729/0/677`. The dominant
  transform-gap family was `prim=6`, `indexed=false`, `VS=0xd5ccd0c915ddcc0b`,
  `PS=0x7b81c162cba6d195`, `vfetch_c=95`, `stride_words=9`, `attrs=4`,
  `attr_sig=0x5d8c9d1f8fea13a1`, with attributes `fmt57@w0`, `fmt57@w3`, `fmt6@w6`, and
  `fmt37@w7`, plus five texture fetches with first texture format `20`. This is the first concise
  live gameplay-layout target for model-space native rendering without multi-GB event logs.
- The sidecar now emits matching `SW2E native layout gap` lines. Validation
  `runtime.native-transform-probe-20260718-000517.log` kept JSON events off, reached
  `Execution complete`, and recorded `3135` native sidecar frames, `1466` transform-gap lines, and
  `1466` layout-gap lines. The top layout family is `prim=6`, `indexed=false`,
  `VS=0xde7f9af93c668314`, `PS=0x8cbad34fce165328`, `vfetch_c=95`, `stride_words=1`,
  `attrs=1`, `attr_sig=0x07e7aa1e6ddfa9a7`, `a0=fmt36@w0->t1i0m1u1s2336`, one texture fetch, and
  first texture format `20`. Treat this as an effects/billboard-style candidate until correlated
  with asset loads or screen captures; the stride-9 transform family remains the main native 3D
  geometry target.
- `run_recomp_native_gap_sample_probe.bat` now captures bounded samples only from unsupported native
  layout/transform draws. Validation `runtime.native-transform-probe-20260718-001120.log` exited
  with code `0`, kept JSON events off, wrote `128` manifest rows under
  `extracted\native_render_samples\native_gap_probe_20260718-001120`, and split them into `53` index,
  `46` texture, and `29` vertex samples. The support split was `123` unsupported-transform rows and
  `5` unsupported-layout rows. `tools\export_native_gap_obj.py` converts paired vertex/index rows
  into OBJ previews; the validation pass exported ten transform-gap OBJs and two layout-gap OBJs,
  including indexed gameplay draws with stride-9, stride-10, stride-11, and stride-12 vertex
  layouts. This is the first runtime-draw-to-Blender bridge for native gameplay renderer research.
- Gap sample metadata now includes compact vertex and pixel float constant snapshots. Validation
  `runtime.native-transform-probe-20260718-002329.log` exited with code `0`, kept
  `native_render_events=false`, touched no JSON event file, and again wrote `128` bounded gap rows:
  `53` index, `46` texture, and `29` vertex. All `128` rows had vertex and pixel constants. The OBJ
  exporter now writes a compact `gap_obj_manifest.csv` with raw bounds, constant indices, and
  heuristic projection candidates. Fresh output
  `extracted\native_render_samples\native_gap_probe_20260718-002329\obj\gap_obj_manifest_projection.csv`
  marks the first stride-9 indexed transform draw as `388` vertices, `239` faces, shader pair
  `VS=0x45C4DDDAAA10F75F / PS=0x7703E4142DFBD4D4`, vertex constants `c0-c7`, and pixel constants
  `c0,c1,c254,c255`. The projection candidates are leads for the native gameplay vertex shader, not
  proof of the final MVP formula.
- `run_recomp_native_projected_gap_replay.bat` is the first opt-in native D3D11 gameplay-gap replay.
  It leaves `native_render_events=false`, captures only `unsupported_textured_transform` draws, and
  writes a BMP instead of a large JSON dump. Validation
  `runtime.native-transform-probe-20260718-005916.log` exited with code `0`, touched no JSON event
  file, wrote
  `extracted\native_render_samples\native_projected_gap_replay_20260718-005916.bmp`, and submitted
  12 captured D3D11 draws. Pixel stats were `1280x720`, `370263` nonzero RGB channels out of
  `2764800`, mean RGB `16.4959`, and max RGB `182`; visually the BMP contains a character-shaped
  gameplay mesh.
- The projected-gap replay exposed and fixed the key indexed-strip rejection: SW2E gameplay
  triangle strips use `0xFFFF` primitive restart markers. The replay now ignores restart markers
  when computing indexed vertex bounds and splits strips at restart markers before uploading
  triangle-list indices. Before that fix, valid gameplay draws were being rejected as oversized
  `vertex_size` copies because `0xFFFF` was mistaken for vertex index `65535`.
- The projected-gap BMP is not a final camera solve. Current validation uses
  `sw2e_native_renderer_gpu_replay_debug_fit_projected_gaps=true`, which fits raw decoded vertex
  axes into visible clip space. It proves live gameplay mesh submission through native D3D11, but the
  real next renderer work is still constant mapping, vertex-shader transform replacement, depth and
  blend state, material translation, and render-target ownership.
- Transform-gap replay now keeps the strongest projected gameplay draws until swap when
  `transform_gaps_only=true`, rather than completing the native pass as soon as the first small draws
  fill the draw limit. Ranking by projected kind, vertex count, and expanded index count keeps larger
  gameplay draws such as frame `2850` draw `44` (`1542` vertices, `2853` indices) and frame `2885`
  draw `151` (`854` vertices, `4611` indices).
- `run_recomp_native_transform_probe.ps1` now accepts `-ProjectedGapMode debug-fit`,
  `constant-fit`, or `constant`. `debug-fit` is the original raw-axis visibility scaffold;
  `constant-fit` applies the current best captured constants and then normalizes projected XY for
  visibility; `constant` is the strict unnormalized projection check. Validation
  `runtime.native-transform-probe-20260718-011839.log` used `constant-fit`, kept JSON events off,
  wrote `native_projected_gap_replay_20260718-011839.bmp`, and produced a `1280x720`, 32-bit BMP
  with `78504` nonzero pixels, mean RGB `10.4241`, and max RGB `182`. Validation
  `runtime.native-transform-probe-20260718-012011.log` used strict `constant`, also touched no event
  JSON, and produced a visible but badly over/under-projected BMP with `170348` nonzero pixels and
  mean RGB `22.5564`. That confirms the D3D11 path and draw prioritization are working, while the
  real camera/shader transform is still unsolved.
- The first guarded presenter handoff is now implemented behind
  `--sw2e_native_renderer_gpu_replay_suppress_backend_swap=true`, defaulting to `false`. The shared
  ReXGlue event stream lets the SW2E sidecar return a per-swap suppression decision, and
  `ExecutePacketType3_XE_SWAP` skips the compatibility `IssueSwap` only when the sidecar has already
  successfully presented a native D3D11 replay frame and the current frame's visible output draws are
  fully covered by supported native replay buckets. Validation
  `runtime.native-gpu-suppress-swap-guard-20260717-182344.log` exited with code `0`, produced zero
  fatal/error/assert lines, made 12 native live presents, logged
  `owns_frame=true, suppress_backend=true` on the first native-presented swap passes, recorded `4916`
  draw events and `84` swaps, and wrote the valid
  nonblank `1280x720` title-screen BMP
  `extracted\native_render_samples\native_gpu_suppress_swap_guard-20260717-182344.bmp`. This proves
  native presentation can temporarily own covered title/menu frames without the compatibility backend
  swap, while still refusing partial-frame ownership. It is still an opt-in title/menu path rather
  than full gameplay render-target ownership.
- The live D3D11 child presenter now handles requested size changes by resizing the existing
  swapchain and recreating its back-buffer RTV instead of creating another child window. Validation
  `runtime.native-gpu-live-present-resize-smoke-20260717-182629.log` exited with code `0`, produced
  zero fatal/error/assert lines, made four capped live presents, and wrote the valid nonblank
  `1280x720` title-screen BMP
  `extracted\native_render_samples\native_gpu_live_present_resize_smoke-20260717-182629.bmp`.
- The D3D11 replay layer now supports native index buffers in the replay data model. `ReplayDraw`
  can carry `uint32_t` indices, and the D3D11 path uploads them and calls `DrawIndexed` when present;
  non-indexed title/menu draws still use the original `Draw` path. Validation
  `runtime.native-gpu-indexed-replay-dormant-smoke-20260717-183520.log` exited with code `0`,
  produced zero fatal/error/assert lines, made four capped live presents, and wrote the valid
  nonblank `1280x720` title-screen BMP
  `extracted\native_render_samples\native_gpu_indexed_replay_dormant-20260717-183520.bmp`. This is
  plumbing for battle/gameplay mesh submission; it still needs a gameplay capture with actual indexed
  SW2E draw families before it can be used for visible 3D scenes.
- Runtime capture now decodes indexed triangle-list draws for the known 24-byte textured layout when
  they appear. The capture path reads the guest index buffer, normalizes 16-bit or 32-bit indices to
  `uint32_t`, bounds vertex copying by the highest referenced index, and attaches the index list to
  `ReplayDraw` for the D3D11 `DrawIndexed` path. Validation
  `runtime.native-gpu-indexed-capture-dormant-smoke-20260717-183929.log` exited with code `0`,
  produced zero fatal/error/assert lines, made four capped live presents, recorded `4916` draw events
  and `84` swaps in
  `extracted\native_render_events\native_gpu_indexed_capture_dormant-20260717-183929.jsonl`, wrote
  `extracted\native_render_events\native_gpu_indexed_capture_dormant-20260717-183929.summary.csv`,
  and produced the valid nonblank `1280x720` BMP
  `extracted\native_render_samples\native_gpu_indexed_capture_dormant-20260717-183929.bmp`. The
  title/menu path still had no indexed draws (`native_indexed=0`), so this proves no regression and
  prepares for the battle capture rather than proving native 3D gameplay yet.
- Fresh current-build event capture
  `extracted\native_render_events\title_current_native_render_events_20260717-173721.jsonl`
  recorded `2949` title/menu draw events and `51` swaps with zero error lines. The pass families are:
  `2670` no-fetch/no-texture `point_list` draws, `130` `rectangle_list` draws, `126` textured
  non-indexed `triangle_list` draws using linear format `20`/BC3 textures and shader pair
  `VS=0xDD8FAA33F9FAB9DB`, `PS=0xFA3E1E7C5EC7C961`, plus `23` `triangle_strip` draws. That gives the
  native renderer a concrete title-screen takeover order instead of a vague "replace graphics" step.
- Current native output is still a capture/replay research path, not a runtime replacement. Full
  native rendering is the target, so the child-window swapchain is temporary scaffolding. The D3D11
  replay still needs to be generalized beyond the title/menu BC3 quad path: broader texture
  decode/upload, vertex/index-buffer extraction, shader/material replacement, render-target ownership,
  frame pacing, final presentation, and native AA before the compatibility renderer can be removed or
  bypassed for real gameplay.
- Focused projected-gap replay now supports shader-hash filters and minimum expanded-index filters.
  Validation `runtime.native-transform-probe-20260718-013024.log` exited with code `0`, kept
  `native_render_events=false`, touched no event JSON, and wrote
  `extracted\native_render_samples\native_projected_gap_replay_20260718-013024.bmp` while filtering
  to `VS=0xED8D12865D27DEBF`. The retained draw log shows the target family is repeatable as two
  stride-12 indexed shapes: `845` vertices / `2415` indices and `1542` vertices / `2853` indices.
  The BMP is nonblank but collapses into a thin projected streak, so this is now a focused
  transform-mapping target rather than a finished camera solve.
- ReXGlue shader dumping is now available through `run_recomp_native_transform_probe.ps1
  -DumpShaders`. Validation `runtime.native-transform-probe-20260718-013325.log` exited with code
  `0`, kept event JSON off, and wrote `180` shader dump files totaling about `1.7 MB` under
  `extracted\native_render_samples\shader_dumps_20260718-013325`. It captured ucode for the key
  gameplay families: `shader_ED8D12865D27DEBF.ucode.vert`,
  `shader_45C4DDDAAA10F75F.ucode.vert`, `shader_1A2E173CABDD3E80.ucode.vert`, and
  `shader_7703E4142DFBD4D4.ucode.frag`.
- Shader analysis explains the projection failure. `ED8D12865D27DEBF` and
  `45C4DDDAAA10F75F` both fetch model position/UV, perform indexed transform work through
  `c[4+a0]`, `c[5+a0]`, and `c[6+a0]`, and then write `oPos` through a final `c0..c3` block.
  `1A2E173CABDD3E80` uses a different path with final `c3..c6` and object work through
  `c[7+a0]..c[9+a0]`. The event stream gives compact constant snapshots capped by the SDK summary
  limit (`128` in the current source-SDK build), while the old arbitrary projection heuristic remains
  capped to its first-eight search space.
- `-ProjectedGapMode shader-final-fit` now provides a shader-guided final-block diagnostic.
  Validation `runtime.native-transform-probe-20260718-014425.log` filtered to
  `VS=0xED8D12865D27DEBF`, exited with code `0`, touched no event JSON, and wrote
  `extracted\native_render_samples\native_projected_gap_replay_20260718-014425.bmp`. Candidate logs
  show `source=shader-final-c0-c3` with constants `c0,c1,c2,c3`; the output is finite but outside
  clip space before normalization (`inside=0.000`), proving that the next implementation step is the
  upstream skin/world transform through `c[4+a0]..c[6+a0]`.
- `tools\apply_rexglue_native_render_wide_constants.ps1` patches a source ReXGlue SDK checkout so
  `kMaxFloatConstantSummariesPerDraw` is `128` instead of `8`. Validation
  `runtime.native-transform-probe-20260718-014734.log` first applied a local `64`-constant SDK patch,
  rebuilt, kept event JSON off, and wrote a bounded `64`-row gap manifest. The manifest is `542107`
  bytes; every row carried `64` vertex constants, with maximum vertex constant index `63`, including
  rows for `VS=0x45C4DDDAAA10F75F / PS=0x7703E4142DFBD4D4`. Later ED8D packed-index inspection
  found palette offsets up to `93`, requiring constants through `c99`, so the source-SDK patch
  default is now `128`. The old projection heuristic remains capped to its first-eight behavior so
  wider captures do not make runtime probing explode combinatorially.
- `-ProjectedGapMode shader-bone0-final-fit` now applies the first upstream shader matrix block
  before the final projection for the shared `ED8D12865D27DEBF` / `45C4DDDAAA10F75F` transform
  skeleton. Validation `runtime.native-transform-probe-20260718-015443.log` filtered to
  `VS=0xED8D12865D27DEBF`, kept event JSON off, and wrote
  `extracted\native_render_samples\native_projected_gap_replay_20260718-015443.bmp`; candidate logs
  show `source=shader-bone0-c4-c6-c0-c3`, upstream constants `c6,c5,c4`, and `inside=1.000` for the
  retained stride-12 meshes. The sibling `45C4DDDAAA10F75F` validation
  `runtime.native-transform-probe-20260718-015800.log` also stayed inside clip space but still
  stretches visually, so the next renderer step is full branch/weight/index evaluation from the
  dumped shader path rather than another arbitrary projection heuristic.
- `-ProjectedGapMode shader-skinned-final-fit` now reads ED8D's real stride-12 skin inputs: two
  float weights at vertex words `3/4` and packed palette offsets at word `5`. Validation
  `runtime.native-transform-probe-20260718-022724.log` used the `128`-constant source-SDK build,
  filtered to `VS=0xED8D12865D27DEBF`, kept event JSON off, and wrote
  `extracted\native_render_samples\native_projected_gap_replay_20260718-022724.bmp` with two
  captured D3D11 draws. Candidate logs show `source=shader-skinned-c4-c6-c0-c3`,
  `upstream=skinned:c4+a0..c6+a0`, and `finite=1.000 inside=1.000` for retained draws.
- The same `shader-skinned-final-fit` pass against `VS=0x45C4DDDAAA10F75F` wrote
  `extracted\native_render_samples\native_projected_gap_replay_20260718-022845.bmp` with four
  captured draws and finite/inside metrics, but still renders as a long thin strip. Keep 45C4
  classified as a separate follow-up until its branch semantics and draw-family purpose are mapped.
- Native GPU replay now records texture format/tiled metadata and supports Xenos `k_8_8_8_8`
  (`fmt=6`) tiled 2D texture fetches as `DXGI_FORMAT_R8G8B8A8_UNORM`. Validation
  `runtime.native-transform-probe-20260718-025213.log` used relaxed ED8D thresholds, exited with
  code `0`, kept event JSON off, and wrote
  `extracted\native_render_samples\native_projected_gap_replay_20260718-025213.bmp` using eight
  captured D3D11 draws. Retained lines show `texture_fmt=6 tiled=1 texture=1280x720` for eight
  projected draws in repeated passes, and there were no D3D11 texture/SRV creation failures.
  ImageMagick identified the BMP as nonblank `1280x720` TrueColorAlpha output. This replaces the
  previous `texture_fetch` blocker for the render-target feedback texture family with ordinary
  draw-family/projection work.
- `VS=0xD5CCD0C915DDCC0B / PS=0x7B81C162CBA6D195` is now mapped as a direct projected strip
  family. Its dumped ucode fetches stride-9 position/normal/packed/UV data and writes `oPos` from
  direct `c7,c8,c9,c10` rows; it is not the shared ED8D/45C4 skinned path. Validation
  `runtime.native-transform-probe-20260718-030209.log` filtered to this vertex shader, exited with
  code `0`, kept event JSON off, and wrote
  `extracted\native_render_samples\native_projected_gap_replay_20260718-030209.bmp`. Candidate logs
  show `source=shader-direct-c7-c10`, repeated four-vertex strips, finite projection metrics, and
  retained D3D11 draws with linear BC3 `512x512` textures. The output is a long textured
  terrain/ground strip after diagnostic fit; the first sampled D5 quads are outside clip before
  normalization, so the next work is pass classification and strict render-target/depth/material
  integration rather than treating D5 as a full scene mesh.
- Bounded gap sample `runtime.native-transform-probe-20260718-030541.log` confirmed the D5 rows now
  include constants through `c10` while keeping event JSON off. It also exposed an OBJ bridge issue:
  non-indexed D5 draws submit `index_count=4` even though the sampled vertex-buffer byte range can
  decode thousands of backing-buffer vertices. `tools\export_native_gap_obj.py` now clamps
  non-indexed previews to the submitted draw count, so D5 runtime previews export as true four-vertex
  strips with two faces.
- `VS=0xDE7F9AF93C668314 / PS=0x8CBAD34FCE165328` is now supported as a constant-selector
  screen-space quad family. The vertex shader reads a stride-1 float selector stream and selects
  `c7..c10` positions, `c11..c14` colors, and `c15..c18` UVs. Validation
  `runtime.native-standard-replay-20260718-031228.log` kept event JSON off, wrote the nonblank
  `extracted\native_render_samples\native_standard_replay_20260718-031228.bmp`, and raised the
  same gameplay replay summary to `native_supported=740`, `native_tex=728`, `native_solid=12`, with
  the old layout bucket reduced from `729` to `1`. The output shows UI/roster text through the
  normal replay path. The replay path now multiplies textures by captured vertex color/alpha, and
  DE7 draws use a texture-lerp pixel mode carrying pixel `c0` plus a per-vertex factor. Current DE7
  capture uses the observed factor-`1` path; exact shader branch/`c20` factor recovery remains a
  follow-up.
- Post-change standard replay smoke `runtime.native-standard-replay-20260718-033713.log` ran
  30 seconds with audio muted, `native_render_events=false`, no sample dumps, standard GPU replay
  enabled, and nonprimary-window placement. The probe closed the process cleanly with exit `0` and
  wrote `extracted\native_render_samples\native_standard_replay_20260718-033713.bmp` from `64`
  captured D3D11 draws. A sampled `24px` grid over the `1280x720` BMP found `392/1620` nonblack
  samples. The run retained DE7 draws with `stride_words=1`, `vertices=4`, `indices=6`,
  `texture=32x32`, and repeated the gameplay summary `native_supported=740`, `native_tex=728`,
  `native_solid=12`. Remaining backend messages include non-fatal `k_1_REVERSE` resolve errors and
  a surface-pitch resolve warning; those are compatibility-backend issues to eliminate as native
  ownership grows.
- Native D3D11 replay now carries captured `normalized_depthcontrol` per draw and creates a
  `D24_UNORM_S8_UINT` depth target for both offscreen BMP replay and the live child swapchain.
  Xenos depth enable/write/compare bits map to D3D11 depth state; stencil bits are only logged until
  stencil ref/mask registers are captured. Validation `runtime.native-depth-replay-20260718-034526.log`
  exited `0`, wrote `extracted\native_render_samples\native_depth_replay_20260718-034526.bmp`,
  and retained draws with depth states such as `0x00724f30` and `0x00724f36`. The `1280x720` BMP
  remained nonblank (`392/1620` sampled grid points, mean RGB-sum `114.29`). Capped live validation
  `runtime.native-depth-live-present-20260718-034637.log` made two successful native child
  swapchain presents and exited `0` with no native live-replay/depth creation failures.
- Native replay now treats the common no-color depth rectangle family as replayable
  `supported_depth_only` work instead of an ownership blocker. The stable signature is
  `VS=0x0a6d1dd7767fdf27`, `PS=0x2e372ea28cc404b7`, `primitive=8`, `index_count=3`,
  `indexed=false`, `stride_words=7`, `normalized_depthcontrol=0x00008777`,
  `pixel_needed=false`, `stencil=true`, `color_mask=0`, and no texture fetches. The D3D11 replay
  submits it as `kind=depth_only`, disables the pixel shader, writes no RT0 color channels, and lets
  the native depth target receive it. Validation `runtime.native-depthonly-guard-20260718-035920.log`
  captured `frame 28 draw 48`, retained `kind=depth_only`, and kept the depth-only-only pass at
  `owns_frame=false`. Live validation `runtime.native-depthonly-live-present-20260718-040253.log`
  exited `0`, captured the same depth rectangle, made two normal native child presents, reported
  zero `[error]` lines, zero assertions, and zero `owns_frame=true` lines, then wrote
  `extracted\native_render_samples\native_depthonly_live_present_20260718-040253.bmp`; a sampled
  `1280x720` grid found `2296/2304` nonblack points with mean RGB `147.91`.
- `sw2e_native_renderer_gpu_replay_draw_limit=0` now means "capture until swap" instead of
  disabling native replay capture, and the cvar range is now `0..4096` instead of `0..256`. This
  removes the artificial cap that prevented full supported-pass capture in gameplay frames.
  Validation `runtime.native-swap-unlimited-menu-20260718-041016.log` exited `0`, produced zero
  `draw-limit` completions, completed `18` replay passes by `swap`, and wrote nonblank
  `extracted\native_render_samples\native_swap_unlimited_menu_20260718-041016.bmp`. Longer
  auto-input validation `runtime.native-swap-unlimited-gameplay-20260718-041111.log` exited `0`,
  reached repeated gameplay frames with `native_supported=750`, `native_tex=728`, `native_solid=12`,
  `native_depth=10`, and wrote
  `extracted\native_render_samples\native_swap_unlimited_gameplay_20260718-041111.bmp` using `751`
  captured D3D11 draws. The sampled `1280x720` BMP was fully nonblack (`2304/2304`, mean RGB
  `102.4`). Native scene ownership is still blocked by the remaining unsupported transform/shape
  buckets (`0/5/1/0/401` to `0/5/1/0/423` in that run); the matched `[error]` lines were the known
  compatibility-backend resolve errors.
- The D5 direct-projection family is now classified as `supported_projected_transform` in standard
  replay, not only in the transform-gap probe path. Matching requires
  `VS=0xD5CCD0C915DDCC0B / PS=0x7B81C162CBA6D195`, decodable non-screen-space stride-9 vertex data,
  no memexport/viz side effects, and a usable texture fetch. Validation
  `runtime.native-d5-projected-standard-20260718-042928.log` exited `0`, kept event JSON and sample
  dumps off, reported zero assertions and zero native replay failures, and reached repeated gameplay
  frames with `native_supported=1130`, `native_tex=728`, `native_solid=12`, `native_depth=10`,
  `native_projected=380`, and unsupported buckets reduced to `0/5/1/0/297`. The offscreen D3D11
  replay wrote `extracted\native_render_samples\native_d5_projected_standard_20260718-042928.bmp`
  using `1388` captured draws; the `1280x720` BMP sampled `3072/3072` nonblack points with mean RGB
  `120.88`. It is still a projected terrain/strip diagnostic rather than correct full-scene output;
  the next blockers are indexed stride-11 transform draws, one indexed layout gap, five shape gaps,
  and native render-target composition.
- The indexed stride-11 `VS=0x1C9E2812AEBDBE4E / PS=0x7703E4142DFBD4D4` shared-skin projection
  family is now promoted as `supported_projected_transform` too. Native replay decodes position
  words `0..2`, a blend scalar at word `3`, packed palette indices at word `4`, and UV words
  `9..10`, then applies `c[4+a0]..c[6+a0]` before the final `c0..c3` projection. Focused validation
  `runtime.native-transform-probe-20260718-044013.log` exited `0`, retained the family with `842`
  vertices and `2088` expanded indices, and wrote nonblank
  `extracted\native_render_samples\native_projected_gap_replay_20260718-044013.bmp`. Standard
  validation `runtime.native-1c9e-projected-standard-20260718-044240.log` exited `0`, reported zero
  assertions and zero native replay failures, reached repeated gameplay frames with
  `native_supported=1220`, `native_projected=470`, and unsupported buckets reduced to `0/5/1/0/207`,
  then wrote `extracted\native_render_samples\native_1c9e_projected_standard_20260718-044240.bmp`
  using `1388` captured D3D11 draws. The next repeated projected blocker is
  `VS=0x1B2E9C6960B0C86E / PS=0xD10452A3E31F9C61`.

## Save And Storage Path

- `0x8210F1F8` and `0x82110570` both call `0x823523D0` when the selected storage device is `-1`.
- `0x823523D0` is a direct wrapper around `XamShowDeviceSelectorUI`.
- ReXGlue's `XamShowDeviceSelectorUI` path is headless and writes dummy HDD device id `1` back to
  the guest pointer, then completes the overlapped call successfully.
- ReXGlue's content layer can create/open saved-game content through `XamContentCreateEx`.
- `XamShowMessageBoxUIEx` is still a runtime stub that logs once and returns success. The visible
  "Create new Samurai Warriors 2 Empires game data?" prompt is game UI, so the next hook target is
  the game's save-confirmation state rather than the runtime message-box stub.
- `0x8249E854` (`__imp__XamInputGetState`) is now hooked in the project layer. It returns a valid
  user-0 controller state from ReXGlue, then ORs in short boot-only Start/A pulses. The hook must keep
  merging real input after boot; replacing the packet, or keeping synthetic A pulses active into real
  menus, makes the menu/save flow appear frozen when using a controller.

## Runtime Build Notes

- The working project is configured against the ReXGlue SDK source tree at
  `L:\SM2\thirdparty\rexglue-sdk-source-v0.8.0`.
- The local CMake file includes the SDK generated headers and ImGui include path needed by the host
  app build.
- The source SDK wait helpers round tiny relative waits up instead of truncating them to zero
  milliseconds. This reduced one class of tight wait polling but was not the main visible-freeze fix.

## Function Map And IDA

- Curated labels live in `src/hooks/function_map.cpp`.
- The full generated map lives in `docs/function_map.generated.csv`.
- `tools/apply_ida_labels.ps1` reads the curated map and applies IDA function names/comments through
  the MCP server only when IDA reports a matching function address.
- The IDA MCP endpoint at `http://127.0.0.1:13337` is reachable. Apply labels only when IDA has the
  Xbox XEX database loaded around `0x82xxxxxx`; if IDA is showing the native PC executable around
  `0x140001000`, the helper will skip guest labels.
- The archive/file layer is now named in IDA. Key labels:
  `sw2e_archive_filesystem_bootstrap`, `sw2e_init_linkdata_archive_tables`,
  `sw2e_archive_read_entry_sectors`, `sw2e_archive_lzp2_postprocess`, `sw2e_lzp2_decode`,
  `sw2e_file_create_open`, `sw2e_file_read`, `sw2e_file_write`, `sw2e_file_seek`,
  `sw2e_file_close`, and `sw2e_file_query_size`.
- The higher-level archive asset wrapper is now identified as `0x82347750`
  (`sw2e_archive_asset_load_wrapper`). It is a better runtime correlation hook than raw sector reads
  because caller LR points at the system that requested the entry.
- The first stage-loader chain is named in IDA:
  `sw2e_battle_stage_scene_bootstrap`, `sw2e_stage_set_first_two_bundle_loader`,
  `sw2e_stage_bundle_loader`,
  `sw2e_stage_bundle_subfile_instantiator`, `sw2e_stage_subasset_table_allocator`,
  `sw2e_stage_metadata_row_lookup`, and `sw2e_stage_subasset_table_cleanup`.
- Additional IDA-confirmed functions were named on July 17:
  `sw2e_save_game_data_write`, `sw2e_wmv_movie_playback_flow`, `sw2e_weapon_tables_init`,
  `sw2e_free_mode_setup_tables_load`, `sw2e_edit_state_assets_init`,
  `sw2e_startup_tables_load`, `sw2e_relay_camera_resource_load`, and
  `sw2e_japan_map_resource_selector`.

## IDA Debug Tooling And Dev Leftovers

- The IDA Pro MCP server at `http://127.0.0.1:13337` is working and cached 11557 strings from the
  XEX. It exposes string regex search, xrefs, decompile/disassembly, rename, comments, and debugger
  commands. The current work used read/search/decompile plus rename/comment only.
- No active hidden debug menu has been confirmed yet. Searches for debug menu, free camera, stage
  select, sound test, movie test, cheat, and related phrases did not find an obvious enabled UI path.
- A lower-level developer logging layer did survive. `0x8236D8F0` formats text through
  `__vsnprintf` and calls the Xbox `DbgPrint` import; `0x8236D978` is a callback-or-DbgPrint logger
  wrapper used by the graphics diagnostics cluster. The project now hooks `__imp__DbgPrint` so these
  messages appear in the PC runtime log.
- `0x8236BBF8` looks like a renderer diagnostic command parser rather than normal gameplay code. It
  branches on single-letter commands `a/c/d/f/g/m/p/t/x` from `input+4` and writes responses with
  `sprintf`. The project hook logs command and response buffers so runtime sessions can prove what
  is still reachable.
- IDA now confirms `0x8236C020` registers that command parser as diagnostic command `28` and
  registers `0x8236BE60` as callback id `47`. The surrounding strings are PIX-flavored
  (`PIX!Gpu`, `PIX!Trace`, `PIX!OK`, `PIX!NO`, and capture-ended markers), so this is useful render
  diagnostics infrastructure rather than a confirmed gameplay debug menu.
- A new read-only debug-tooling pass found `0x8236B8B8` as a PIX trace/capture state-machine lead.
  It is reached from strings such as `PIX Trace Capture Begin.\n`, `PIX!Trace`, `PIX!Gpu`, and
  `crashdump.pix2`. This should be named/commented in IDA next, then validated by searching runtime
  logs for `PIX` and `SM2 guest DbgPrint` during longer render probes.
- `0x8236DCE0`, `0x8236DF60`, and `0x8236E780` form the current graphics/device diagnostic cluster.
  The cluster calls config/device probes such as `ExGetXConfigSetting`, emits debug text, and has a
  missing-device trap path. These are diagnostics, not a confirmed gameplay debug menu.
- `runtime.debug-hooks-smoke.log` validates the first debug-hook build. A 25-second muted smoke run
  stayed alive and logged `0x8236E780` from the graphics init path with no fatal/assert/crash or
  dirty-disc matches. `DbgPrint` and the command parser did not emit during that short boot window.
- There are useful leftover source paths in the retail XEX. Examples:
  `.\application\chara\Weapon.cpp`, `.\application\scene\RelayCamera.cpp`,
  `.\application\free\emp_free_setup.cpp`, `.\application\gamestate\StateEdit.cpp`,
  `.\application\ui\JapanMap.cpp`, `.\application\war\emp_war_fix.cpp`, and
  `.\xbox\sys\XBSaveLoad.cpp`. These are good anchors for future function names.
- `debug/iwata/MOVIE_TELOP_OPENING.bin` and related movie-caption paths are present, but IDA reports
  no code xrefs for those strings. Treat them as stale debug-path leftovers unless a later data-xref
  pass proves otherwise.
- `/_sm4emp/ViewerCharaXB2.g1s` and `/_sm4emp/ViewerCharaClothXB2.g1s` strings are present, but
  also currently have no code xrefs. They are still worth remembering for character-viewer/model
  asset research, but they are not an enabled viewer entry point yet.
- Raw archive/string hits for `cheat`, `debug`, `test`, `panic`, `warning`, and `log` were mostly
  dialogue or binary false positives in the current pass. Example: a raw `cheat` hit maps to entry
  `65`, but context shows dialogue text, not a cheat system.
- The most useful new data leads are not debug menus; they are table loaders. `Weapon.cpp` loads
  archive entries `1578` and `1579`, with `1579` explicitly treated as `0x30`-byte records.
  `RelayCamera.cpp` loads entry `77`; `sc_ctrl.cpp` loads scenario-control entry `68`; Free
  Mode/startup load entries `118`, `120`, and `121`; Edit/new-officer state loads entry `128`; save
  data uses entry `3548` as a PNG thumbnail.
- The scenario data cluster around entries `65-68` is now partially mapped. Entry `65` is a
  seven-section scenario/battle message blob loaded by `0x82260B98`; split output lives in
  `extracted\linkdata_bns_scenario_cluster_65_68\entry_0065_sections`. Sections `0/1` contain
  battle event message templates, section `2` contains thousands of officer/dialogue callouts, and
  sections `3-6` contain smaller event-dialogue groups. Entry `66` is loaded by `0x82262678` and is
  a counted `595`-row table with `0x2C`-byte rows. Entry `67` is still an unnamed small table.
- Scenario-control entry `68` is now extracted and scanned. It is a raw `8604`-byte table:
  big-endian count `86` followed by `86` fixed `0x64`-byte records. The conservative CSV is
  `extracted\linkdata_bns_scenario_entry_68\scenario_control_entry_68.csv`, generated by
  `tools\scan_scenario_control_table.py`. Field names are not final yet; early `u16_00` values look
  like packed ids and need runtime access correlation.

## Modding Research

- The first modding layer is a loose-file overlay through `--mod_data_root`; `run_recomp.bat` points
  it at `mods\loose`.
- Current game data research is in `docs/modding_support.md`.
- The main content archive is `data\LINKDATA_BNS.IDX` + `data\LINKDATA_BNS.LNK`. The IDX uses
  `SM4L` plus 16-byte big-endian entries that point into the LNK in `0x800` byte sectors.
- Current scan found `G1TG` texture containers, `G1M_` model containers, `G1A_` animation
  containers, `LZP2` compressed blocks, and plain `[global ...]` render-config chunks.
- `LZP2` is now locally decoded and IDA-confirmed. It has a 16-byte header with little-endian
  decompressed size and compressed payload size, then a literal/back-reference/RLE stream ending in
  control byte `0`.
- Two useful inner containers are now split:
  `0x00033B59` tables for embedded texture bundles, and simple big-endian offset tables for
  stage/map bundles.
- Stage sample `2856-2858` now splits into usable files: entry `2856` contains `G1M_` models and
  `G1TG` textures, entry `2857` contains unknown metadata-style chunks, and entry `2858` contains
  299 non-empty `G1TG` texture containers plus 312 empty offset-table slots.
- IDA confirms a live three-entry stage path. `0x8227AAB0` loads the first two entries of a
  stride-3 set with `3 * stage_id + 2856` and `3 * stage_id + 2857`, or alternate entries
  `3 * stage_id + 2937` and `3 * stage_id + 2938`. `0x82286D10` loads the third bundle with
  `3 * stage_id + 2858`, or alternate entry `3 * stage_id + 2939`.
- `tools\scan_stage_bundle.ps1` now emits CSV manifests for decompressed offset-table bundles. The
  current baselines are `stage_2856_bundle_scan.csv`, `stage_2858_bundle_scan.csv`, and the
  runtime-confirmed `runtime_stage_2943_bundle_scan.csv`, `runtime_stage_2944_bundle_scan.csv`, and
  `runtime_stage_2945_bundle_scan.csv` scans.
- `tools\scan_g1m.ps1` now expands `G1MG` geometry chunks into Project-G1M-aligned section labels:
  materials, vertex buffers, vertex attributes, joint palettes, index buffers, submeshes, and mesh
  groups. Stage entries `2856` and `2943` both report `DX9` G1MG headers and 9 geometry sections.
- The first parsed SW2E stage vertex declaration is consistent across `2856` and `2943`: position
  `Float_x3` at stream offset `0`, joint index `UByte_x4` at `12`, normal `Float_x3` at `16`,
  color `NormUByte_x4` at `28`, and UV `Float_x2` at `32`. The scanner also emits index-buffer and
  submesh vertex/index/material ranges.
- `tools\export_g1m_obj.py` now exports the currently decoded SW2E `G1M_0030` / `G1MG0040` subset
  to OBJ for Blender inspection. It supports positions, normals, UVs, triangle strips, and strip
  restart indices. Validated exports with zero warnings: 3 normal stage `2856` models, 3 runtime
  stage `2943` models, 8 character samples from `1566-1573`, and all 23 embedded `G1M_` models from
  weapon/prop bundle entry `2410`.
- `tools\patch_g1m_from_obj.py` now provides the first conservative edit path back into `G1M_`:
  patch existing position/normal/UV float streams from matching exported OBJ files while preserving
  topology, indices, materials, skeleton/joint data, unknown fields, and original file size.
  Validation: no-edit stage `2856` roundtrip patched 3 files and re-exported with 3055 vertices,
  4456 faces, and 0 warnings; a controlled first-vertex move survived OBJ -> G1M -> OBJ.
- `tools\build_linkdata_entry_patch.ps1` now connects patched G1M subfiles to the archive-entry
  pipeline. The controlled entry `2856` edit was overlaid onto the split stage container, repacked,
  wrapped as literal `LZP2`, dry-run through full `LINKDATA_BNS` rebuild with 1 patched entry, then
  decompressed/split/exported back to OBJ. The moved first vertex survived the full path:
  OBJ -> G1M -> offset-table stage container -> LZP2 -> split -> OBJ.
- Full `LINKDATA_BNS.IDX/LNK` rebuild was generated at `out\modded_linkdata_bns_preview` with 3549
  entries and 1 patched entry. Extracting entry `2856` from that rebuilt archive, decompressing,
  splitting, and exporting back to OBJ produced 3055 vertices, 4456 faces, 0 warnings, and the moved
  first vertex `v 1 37.6050391 -750`. The rebuilt archive's patched `sub_0001` hash matches the
  controlled patched G1M hash.
- Patched archive runtime smoke passed when mounted through a full hard-linked `data` mirror:
  `runtime.patched-archive-full-data-smoke.log` and
  `runtime.patched-archive-full-data-pulse-smoke.log` both mounted the patched overlay and opened
  `LINKDATA_BNS.LNK/IDX` with no fatal/assert/crash/dirty-disc lines. A partial overlay containing
  only `LINKDATA_BNS` is not safe: it caused `LINKDATA_DMY.LNK` lookup failure and dirty-disc UI.
- `tools\pulse_recomp_input.ps1` was added for unattended smoke tests past later menu prompts.
  The current built-in synthetic input only covers startup/storage prompts; later menus still need
  manual input or a foreground key pulse.
- `tools\build_linkdata_asset_manifest.ps1` now builds range-level archive manifests directly from
  `LINKDATA_BNS`. It can decode LZP2 in memory, identify inner offset tables and `0x00033B59`
  texture bundles, and count embedded models, textures, animations, CAP metadata, texture images,
  empty slots, unknown chunks, and G1MG geometry chunks.
- Runtime-confirmed table/resource entries are now tracked in `docs\modding_support.md`. High-signal
  targets include weapon tables `1578/1579`, Japan-map variants `112-117`, startup/setup tables
  `118/120/121`, edit-state entry `128`, relay-camera entry `77`, and save thumbnail entry `3548`.
- New manifest outputs:
  `manifest_character_officer_1566_1945.csv`, `manifest_weapon_prop_2062_2411.csv`, and
  `manifest_stage_representative_2856_2870.csv`. Current high-signal reads: the character/officer
  band has 83 top-level model entries, 88 texture entries, 41 animation entries, and 74 CAP metadata
  entries; the weapon/prop band has 222 direct model entries, 127 texture entries, and entry `2410`
  as a 23-model embedded bundle; the first five normal stage groups show the same three-entry
  model/metadata/texture pattern.
- Repackers now exist for `0x00033B59`, offset-table containers, literal LZP2 streams, and full
  `LINKDATA_BNS.IDX/LNK` rebuilds. No-patch BNS dry-run preserves the original archive size.
- `__imp__NtCreateFile` and `__imp__NtOpenFile` now log paths and returned handles. Use those logs
  with `NtReadFile` offsets to correlate menus/battles with file and archive access.
- `0x8210E528` is now hooked read-only. A 40-second muted smoke run stayed responsive and logged
  real archive ids and output magics, including `KSHL`, `G1TG`, and decompressed `0x00033B59`
  texture bundles.
- `0x82347750` is now hooked read-only to log high-level archive asset requests for the first calls
  and modding-relevant ranges, including stage, character, texture, and table banks.
- `runtime.asset-hook-smoke.log` is the current validation run. The game stayed alive for the full
  45-second smoke window, no fatal/assert/crash lines were found, and stage-path entries `2943`,
  `2944`, and `2945` were captured. Entries `2943` and `2944` came from the first-two stage loader
  path, and entry `2945` came from `0x82286D10` at LR `0x82286E20`, matching the alternate
  `3 * stage_id + 2939` formula with `stage_id = 2`.

## Next Hook Candidates

- Keep the storage/profile and draw-loop logging hooks active while mapping deeper gameplay.
- Map the GPU resolve callers around the `0 <= x < 1280` over `surface pitch 640` warnings and the
  occasional `k_1_REVERSE` destination-format warning. The game is playable, but these are the next
  renderer accuracy targets.
- Use the new `sw2e_archive_asset_load_wrapper` (`0x82347750`) logs to correlate title/menu/battle
  actions with archive entry id, stage id, mode/override state, and caller LR, then fall back to
  `0x8210E528` when sector-level detail is needed.
- Build a friendly mod manifest on top of the repackers so edited `G1M_`, `G1TG`, and table chunks
  can be injected without manually copying catalog relative paths.
- Identify the unknown decoded table chunks that likely control economy, officer, weapon, campaign,
  collision, spawn, and placement data.
- Add a controllable input hook/toggle so boot automation can be turned off explicitly after the save
  prompt instead of only by call-count windows.
- Once the XEX IDB is loaded in IDA, run `tools/apply_ida_labels.ps1 -Apply` to push the current
  labels and comments into the database.
