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
  manifest beside the binary samples with draw, shader, fetch, texture, and vertex-layout metadata.
  `sw2e_native_renderer_dump_priority_samples_only=false` can be enabled to keep the sample budget for
  indexed, `triangle_strip`, stride-8+ model-layout, or multi-texture draws instead of title/menu
  quads.
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
