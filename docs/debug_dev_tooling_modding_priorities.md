# Debug And Modding Priorities

This note is a compact research pass over the current docs, hook source, function map, and small
extracted manifests. It avoids runtime logs, raw binaries, generated recomp code, and broad asset
dumps.

## Current Runtime Hooks

The project already has enough runtime surface to map game behavior without editing generated code.
Use these before adding new probes:

| Area | Hook or cvar | Current use |
| --- | --- | --- |
| Loose files | `--mod_data_root` | Mounts a loose replacement tree; `run_recomp.bat` uses `mods\loose` when present. |
| Startup automation | `sw2e_auto_boot_input`, `sw2e_auto_probe_input` | False by default; only for bounded smoke/probe input through `XamInputGetState`/keystrokes. |
| Input | `__imp__XamInputGetState`, `__imp__XamInputGetKeystrokeEx` | Merges real input with optional staged pulses. Keep this hook conservative because bad pulses looked like freezes. |
| Save/content | `__imp__XamContentCreateEx`, `XamContentClose`, `XamContentSetThumbnail` | Brackets save-container lifecycle and thumbnail writes. |
| File I/O | `NtCreateFile`, `NtOpenFile`, `NtReadFile`, `NtWriteFile`, `NtFlushBuffersFile`, `NtClose` | Logs guest paths, handles, lengths, offsets, and status for archive/save correlation. |
| Archive reads | `0x8210E528` / `sw2e_archive_read_entry_sectors` | Logs packed archive selector, entry id, sectors, output pointer, LR, and output magic. |
| Asset requests | `0x82347750` / `sw2e_archive_asset_load_wrapper` | Best hook for "who requested entry X" because LR identifies the owning loader. |
| LZP2 decode | `0x8210D9F8`, `0x82116600` | Best next hook point when mod overrides need post-decompression identity. |
| Guest debug text | `__imp__DbgPrint` | Mirrors surviving developer diagnostics into runtime output. |
| Renderer diagnostics | `0x8236BBF8`, `0x8236BE60`, `0x8236C020`, `0x8236B8B8` | PIX/debug command parser, callback registration, performance marker, and trace/capture state-machine leads. |
| Native render sidecar | `sw2e_native_renderer*`, `native_render_events*` | Bounded draw/swap classification, guest-memory hashes, samples, shader dumps, and experimental D3D11 replay. |

Prefer bounded runs:

- Keep `native_render_events=false` for gameplay probes unless a capped JSONL event stream is
  intentionally needed.
- Use `sw2e_native_renderer_dump_gap_samples_only=true` or priority-only sample dumping for battle
  renderer work.
- Keep synthetic input off in normal play.

## Debug And Dev Tooling Leads

No active gameplay debug menu is confirmed yet. Existing searches for debug menu, free camera, stage
select, sound test, movie test, cheat, and similar strings mostly found stale strings, dialogue, or
binary false positives.

The real surviving dev-tool lead is the graphics diagnostic cluster:

| Address | Current label | Why it matters |
| --- | --- | --- |
| `0x8236BBF8` | `sw2e_renderer_debug_command_parser` | Parses single-letter commands `a/c/d/f/g/m/p/t/x` from `input+4` and writes a response with `sprintf`. |
| `0x8236C020` | `sw2e_pix_diagnostic_callback_registration` | Registers the parser as diagnostic command `28` and the marker callback as id `47`. |
| `0x8236BE60` | `sw2e_pix_performance_marker_callback` | Tracks PIX-style render event bits and timing counters. |
| `0x8236B8B8` | `sw2e_pix_trace_capture_state_machine` | Reached from `PIX!Trace`, `PIX!Gpu`, `crashdump.pix2`, and capture begin/end strings. |
| `0x823717B0`, `0x823727C0` | PIX capture-ended callbacks | Xrefs to `PIX!{CaptureEnded}` and `PIX!{CaptureFileCreationEnded}`; useful for naming the rest of the diagnostics flow. |
| `0x8236D8F0` | `sw2e_guest_debug_print_formatter` | Formats a stack message with `__vsnprintf` and sends it to `DbgPrint`. |
| `0x8236D978` | `sw2e_guest_debug_callback_logger` | Dispatches diagnostic text through a callback or falls back to `DbgPrint`. |
| `0x8236DCE0`, `0x8236DF60`, `0x8236E780` | graphics/device diagnostic cluster | Probes config/device state and can report graphics init diagnostics. |

Treat these as renderer diagnostic infrastructure until a real caller path proves otherwise. The best
runtime validation is to enter menus, edit mode, map screens, and battles, then search for
`SM2 guest DbgPrint`, `SM2 debug command parser`, or `PIX` in the current runtime output.

Stale but useful string leads:

- `debug/iwata/MOVIE_TELOP_OPENING.bin` and related movie-caption paths: no confirmed code xrefs yet.
- `/_sm4emp/ViewerCharaXB2.g1s` and `/_sm4emp/ViewerCharaClothXB2.g1s`: possible viewer/model
  research strings, but no confirmed enabled viewer entry point yet.
- Source-path anchors worth using for function names:
  `Weapon.cpp`, `ModelManager.cpp`, `MotionManager.cpp`, `RelayCamera.cpp`, `Stage.cpp`,
  `StgTexture.cpp`, `MiniMap.cpp`, `emp_free_setup.cpp`, `StateEdit.cpp`, `StateStartUp.cpp`,
  `JapanMap.cpp`, `sc_ctrl.cpp`, `emp_war_fix.cpp`, and `XBSaveLoad.cpp`.

## Format And Asset Leads

The highest-value modding path is already archive-first:

| Format/layer | Status |
| --- | --- |
| `LINKDATA_BNS.IDX/LNK` | Main content archive. IDX is `SM4L`, 3549 entries, 16-byte big-endian records, `0x800` byte sectors. |
| `LZP2` | Decoder mapped to `0x82116600`; local decode and literal encode exist. |
| `0x00033B59` | Inner texture-bundle container split/repack path exists. |
| Offset-table containers | Stage/map bundles split/repack path exists. |
| `G1M_` / `G1MG` | Scanner/exporter recognizes the SW2E Xbox 360 `DX9` geometry subset; OBJ export and conservative OBJ reimport exist. |
| `G1TG` | Texture containers are identified; runtime native texture decode is proven for linear Xenos format `20` / BC3. Material texture binding is still a priority. |
| `G1A_` | Animation containers identified in character/officer ranges; parsing is still open. |
| `00_20CAP` | Character/officer attachment/capsule metadata candidate; parsing is still open. |
| `[global ...]` | Render config text candidates around entries `2762-2855`; useful for lighting/material/native render research. |

High-signal entry ranges and tables:

| Entries | Current read | Priority |
| --- | --- | --- |
| `65-68` | Scenario cluster. `65` is a seven-section message blob, `66` is `595` rows of `0x2C`, `67` is unnamed, `68` is `86` rows of `0x64`. | Campaign/script/map-state data. |
| `77` / `0x4D` | Relay-camera resource from `RelayCamera.cpp`, raw 1008-byte blob. | Camera/editor preview metadata. |
| `112-117` | Japan map/UI variants selected through table `0x82054C58`. | Map UI and campaign navigation. |
| `118`, `120`, `121`, `128` | Startup, Free Mode setup, and edit/new-officer state tables. | Mode setup, officer creation, editor UX. |
| `1566-1945` | Character/officer run: 83 models, 88 texture containers, 41 animations, 74 CAP metadata entries, 367 texture images. | Characters, costumes, animations, attachments. |
| `1578`, `1579` | Weapon tables from `Weapon.cpp`; `1578` looks like 141 `0x20` records, `1579` like 147 `0x30` records. | Weapon stats/types/unlocks. |
| `2062-2411` | Weapon/prop/equipment run: 222 direct models, 127 texture containers, plus entry `2410` with 23 embedded models. | Weapon/prop model editing. |
| `2505-2681` | Texture-heavy bank. | UI, portraits, icons, shared battle textures. |
| `2856-2990` | Stage/map groups, mostly three-entry sets. | Blender/map editor foundation. |
| `2943-2945` | Runtime-confirmed alternate stage group for a battle path. | Use as a live correlation target. |
| `3548` / `0xDDC` | Save thumbnail PNG loaded by save flow. | Save/profile marker, not gameplay-critical. |

Stage groups are the cleanest Blender path right now. A normal stage group is:

```text
first entry:  3 G1M_ models + 3 G1TG texture containers + 1 unknown chunk
second entry: 5 unknown metadata chunks
third entry:  large offset-table texture bundle
```

For example, entry `2856` splits into 7 chunks and exports 3 stage models. The runtime-confirmed
alternate group `2943-2945` follows the same shape, with `2944` currently the best unknown metadata
target. The unknown chunks are likely where collision, pathing, placement, weather, spawn, or
scenario-specific stage metadata will surface.

## Highest-Value IDA Naming Targets

1. Stage loader internals: finish naming around `0x8227AE00`, `0x8227AAB0`, `0x82286D10`,
   `0x82286B30`, `0x82286950`, `0x822869D8`, and `0x82286A70`. Add comments for the normal
   `2856/2857/2858` and alternate `2937/2938/2939` formulas.
2. Stage subasset constructors: from `0x82286B30`, identify the branch/table that instantiates
   `G1M_`, `G1TG`, unknown metadata chunks, collision/pathing data, and any placement objects.
3. Asset wrapper callers: use LR from `0x82347750` to name caller systems as entries are loaded in
   real menus/battles. This gives better names than broad archive scans.
4. Weapon table accessors: start at `0x8215D4C0`, then name xrefs to its initialized globals and
   decode the `0x20` and `0x30` record fields in entries `1578/1579`.
5. Scenario-control globals: expand around `0x82257938` and globals `0x827A6B70/0x827A6B74`;
   cross-reference records in entry `68` with actual Empire Mode/menu behavior.
6. Scenario auxiliary/message readers: name accessors under `0x82260B98` and `0x82262678` so
   entries `65/66/67` get field names instead of only row sizes.
7. Character/edit-mode loaders: follow `0x8220B210` into new-officer state, character selection,
   `G1A_` animation loads, and `00_20CAP` attachment metadata.
8. Texture/material binding: name `G1M_` material texture-index consumers and the neighboring
   `G1TG` lookup path, because this directly improves Blender exports/imports.
9. PIX/debug cluster: keep `0x8236BBF8`, `0x8236C020`, `0x8236BE60`, `0x8236B8B8`,
   `0x823717B0`, `0x823727C0`, `0x8236D8F0`, and `0x8236D978` labeled, but treat them as render
   diagnostics unless a reachable gameplay-facing caller appears.
10. Native-render draw families: keep naming shader/draw-family evidence from bounded probes,
    especially the promoted D5, 1C9E, 1B2E, A395, 45C4, 6B72, ED8D, 6E10, 83BD, and B21C
    projection paths plus the current `3094A52CE2571823 / 969CA710A35A4251` non-indexed stride-8
    transform blocker, the `5A550226A224F581 / 7703E4142DFBD4D4` stride-7 layout blocker,
    stride-8/9/10 layouts, and tiled/render target texture fetches. This supports native rendering
    and also helps correlate runtime meshes back to archive `G1M_` data.

## Recommended Next Work

- Do not run broad JSON or binary dumps. Keep using capped native samples, small CSV manifests, and
  targeted IDA xrefs.
- Add bounded runtime logs around the stage-loader chain to print selected stage id, entry triplet,
  loaded pointer, subfile count, and LR.
- Keep using `0x82347750` caller LR plus archive entry ids as the main naming pass for loaders; it
  is the cleanest way to connect menus, battles, weapons, characters, and stage assets to
  `LINKDATA_BNS` entries.
- For the Blender/map-editor path, decode runtime-confirmed stage metadata entry `2944` first. For
  campaign/table editing, decode entry `68` next; for equipment editing, decode `1578/1579`.
- Build a friendly mod manifest format that can say "replace stage 2856 sub_0001" or "replace
  weapon table 1579" and drive the existing repackers.
- For Blender/map editing, prioritize material texture binding and unknown stage metadata before
  adding more OBJ geometry features.
- Keep graphics work side-by-side with modding: native rendering needs the same asset identity map,
  and modding benefits from runtime draw-to-asset correlation.
