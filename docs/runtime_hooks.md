# Runtime Hooks And Function Maps

This project now has a small non-generated hook layer under `src/hooks`.

Generated ReXGlue functions are emitted as weak `sub_XXXXXXXX` aliases that point at strong
`__imp__sub_XXXXXXXX` implementations. To hook a game function, define a strong
`sub_XXXXXXXX` in `src/hooks/runtime_hooks.cpp`, then call `__imp__sub_XXXXXXXX(ctx, base)`
when the original behavior should still run.

Current active hooks and runtime changes:

| Address | Symbol | Current label | Why it matters |
| --- | --- | --- | --- |
| `0x8210F1F8` | `sub_8210F1F8` | storage prompt caller | Calls the storage selector when no device is selected. |
| `0x82110570` | `sub_82110570` | storage/profile flow | Alternate storage selector caller on the profile/save path. |
| `0x8210E528` | `sw2e_archive_read_entry_sectors` | archive entry sector read | Logs packed archive selector, entry index, sector count, output pointer, and output magic. |
| `0x823523D0` | `sub_823523D0` | storage selector wrapper candidate | Tiny wrapper around `XamShowDeviceSelectorUI`. |
| `0x82353A98` | `sub_82353A98` | xam message-box wrapper | Builds an event and calls `XamShowMessageBoxUIEx`. |
| `0x82353BF8` | `sub_82353BF8` | video-mode warning message flow | Builds localized message-box strings after checking AV/config state. |
| `0x8235C820` | `sub_8235C820` | draw packet builder | Skips stale draw cursors before they write into read-only guest pages. |
| `0x8235CC48` | `sub_8235CC48` | transient draw packet builder | Same stale-cursor guard on a second title/movie packet path. |
| `0x8235D660` | `sub_8235D660` | movie/draw state submitter | Guards a larger packet write during title/menu transitions. |
| `0x82361B70` | `sub_82361B70` | draw state/quad command builder | Guards title/menu draw command writes. |
| `0x82363DA8` | `sub_82363DA8` | draw packet helper | Guards a small draw packet write. |
| `0x82363FE8` | `sub_82363FE8` | draw packet helper | Guards a small draw packet write. |
| `0x82364248` | `sub_82364248` | command stream drain caller | Guards command stream cleanup packet writes. |
| `0x82369820` | `sub_82369820` | variable packet writer | Main high-frequency draw packet writer; used as the render heartbeat. |
| `0x82347750` | `sw2e_archive_asset_load_wrapper` | archive asset load wrapper | Logs higher-level archive asset requests, resolved selectors/entry ids, caller LR, returned pointer, and output magic. |
| `0x8236BBF8` | `sub_8236BBF8` | renderer debug command parser | Logs the command byte and response buffer from a surviving single-letter diagnostic parser. |
| `0x8236BE60` | `sub_8236BE60` | PIX performance marker callback | Tracks PIX-style render event bits and timing counters after registration by the graphics diagnostic path. |
| `0x8236C020` | `sub_8236C020` | PIX diagnostic callback registration | Registers the renderer command parser and PIX performance callback with the graphics diagnostics interface. |
| `0x8236D978` | `sub_8236D978` | guest debug callback logger | Pass-through logs the graphics diagnostic logger wrapper. |
| `0x8236DCE0` | `sub_8236DCE0` | graphics device diagnostic report builder | Pass-through logs the graphics/device report path. |
| `0x8236DF60` | `sub_8236DF60` | graphics init diagnostic trap path | Pass-through logs the init diagnostics and missing-device trap path. |
| `0x8236E780` | `sub_8236E780` | graphics config probe and init path | Pass-through logs the init path that probes config/device state. |
| `0x8249E854` | `__imp__XamInputGetState` | xam controller state import | Merges real input with optional Start/A boot pulses when `--sw2e_auto_boot_input=true`. |
| `0x8249E7D4` | `__imp__XamContentCreateEx` | xam content create import | Logs save-container creation/opening. |
| `0x8249EBE4` | `__imp__DbgPrint` | xbox debug print import | Mirrors surviving guest developer diagnostics into the PC runtime log. |
| `0x8249E974` | `__imp__NtCreateFile` | kernel file create/open import | Logs guest file paths and returned handles for runtime file mapping. |
| `0x8249E9D4` | `__imp__NtOpenFile` | kernel file open import | Logs guest file paths and returned handles for runtime file mapping. |
| `0x8249E924` | `__imp__NtWaitForSingleObjectEx` | kernel wait import | Logs suspicious waits on the save/profile path. |

The storage/profile hooks seed dummy HDD device id `1` before the game asks the ReXGlue headless
storage selector. Synthetic Start/A input is now off for normal play. Launch with
`--sw2e_auto_boot_input=true` only for unattended smoke tests that need to advance past startup
prompts without touching the window.

Mouse/keyboard support is enabled through ReXGlue MnK controller emulation, not generated recomp
code. `run_recomp.bat` and `launch.vs.json` now use the gameplay layout below:

| PC input | Xbox input | Current use |
| --- | --- | --- |
| `W/A/S/D` | left stick | Movement/menu steering. |
| Mouse movement | right stick | Camera/look when the mouse is captured. |
| `LMB` | `X` | Normal attack. |
| `RMB` | `Y` | Charge/strong attack. |
| `Space` | `A` | Confirm/jump. |
| `Shift` | `B` | Back/cancel/Musou-style action depending on game state. |
| `Q` / `F` | left/right shoulder | Shoulder actions. |
| `Z` / `Control` | left/right trigger | Trigger actions moved off mouse buttons. |
| `Tab` / `Escape` | Back/Start | Back and pause/menu. |

The default launcher uses `--mnk_capture_mouse=true`, which hides and recenters the cursor so mouse
movement works like a right stick without hitting the window edge. `run_recomp_visible_cursor.bat`
uses the same input mapping with `--mnk_capture_mouse=false`; that keeps the host cursor visible for
menu/editor experiments, but mouse-look can clamp at the window boundary because the pointer is no
longer recentered.

The draw packet hooks do not replace renderer logic. They only skip stale guest command cursors that
point at non-writable pages. The ReXGlue SDK source also has Samurai Warriors 2 Empires compatibility
fallbacks in `src/graphics/util/draw.cpp`: unsupported resolve commands, bad resolve vertex buffers,
unsupported MSAA, and non-color destination formats now log and drop the bad resolve instead of
asserting the runtime. Out-of-pitch resolve rectangles still log, but the SDK path clamps them and
continues. This is what made the current build playable.

The archive read hook is read-only. It calls the original `0x8210E528` implementation, then logs the
decoded packed id and the first four output bytes. A 40-second smoke run after adding it stayed
responsive and logged entries including early `KSHL`, direct `G1TG`, decompressed `0x00033B59`
texture bundles, character-range entries, and equipment/prop-range entries.

The asset-load wrapper hook is also read-only. It calls the original `0x82347750` implementation and
logs only the early calls plus modding-relevant ranges. This is the better hook for correlating
runtime behavior with archive ids because its LR points at the system that requested the entry, such
as the first-two stage bundle loader at `0x8227AAB0` or the third bundle loader at `0x82286D10`.

The guest debug hooks capture surviving developer diagnostics. The `DbgPrint` import receives text
after the game's own `__vsnprintf` formatting, so the project hook mirrors that final string into
the PC log. The `0x8236BBF8` hook watches a suspicious renderer diagnostic command parser that
branches on single-letter commands `a/c/d/f/g/m/p/t/x` and writes responses with `sprintf`.
IDA confirms `0x8236C020` registers that parser as diagnostic command `28` and registers
`0x8236BE60` as callback id `47`. Strings around this path include `PIX!Gpu`, `PIX!Trace`,
`PIX!OK`, `PIX!NO`, and capture-ended markers, so treat it as PIX/render diagnostics rather than a
confirmed gameplay debug menu.

`runtime.debug-hooks-smoke.log` is the first validation run for these hooks. The rebuilt executable
linked successfully, stayed alive for a 25-second muted smoke run, and logged `0x8236E780` from the
graphics init path with no fatal/assert/crash/dirty-disc matches. The `DbgPrint` and command-parser
hooks did not emit during that short boot window; they remain targeted probes for longer menu,
edit-mode, map, and battle sessions.

`runtime.asset-hook-smoke.log` is the current validation log for this hook. The game stayed running
for the full 45-second smoke window, no fatal/assert/crash lines were found, and the hook captured
stage-path requests for entries `2943`, `2944`, and `2945`. Entries `2943` and `2944` came from the
first-two loader path, and entry `2945` came directly from `0x82286D10` before the archive worker
read it asynchronously.

Manifest-only runtime additions:

| Address | Symbol | Why it matters |
| --- | --- | --- |
| `0x821EDA68` | `sub_821EDA68` | Added after a runtime fatal reported an invalid/unregistered function during menu transition. |
| `0x8234E688` | `sub_8234E688` | Runtime-discovered start transition helper. |
| `0x823887B8` | `sub_823887B8` | Runtime-discovered helper. |
| `0x82391A10` | `sub_82391A10` | Runtime-discovered helper. |

The curated map is in `src/hooks/function_map.cpp`. The generated full map can be refreshed with:

```powershell
powershell -ExecutionPolicy Bypass -File .\tools\export_function_map.ps1
```

IDA labels can be applied from the curated map with:

```powershell
powershell -ExecutionPolicy Bypass -File .\tools\apply_ida_labels.ps1 -Apply
```

That script looks up every address before writing. If IDA is currently showing the native PC
executable instead of the Xbox XEX database, it skips the labels instead of renaming the wrong
functions.

Use the curated map for high-confidence labels and the generated CSV for quick address lookup.

IDA-confirmed archive/file landmarks:

| Address | IDA name | Why it matters |
| --- | --- | --- |
| `0x8210EF50` | `sw2e_archive_filesystem_bootstrap` | Initializes archive/file state. |
| `0x8210E0D8` | `sw2e_init_linkdata_archive_tables` | Opens link archives and expands IDX records into runtime tables. |
| `0x8210E528` | `sw2e_archive_read_entry_sectors` | Best runtime hook target for archive entry id logging and future mod overrides. |
| `0x8210D9F8` | `sw2e_archive_lzp2_postprocess` | Detects LZP2 after archive reads. |
| `0x82116600` | `sw2e_lzp2_decode` | Game's LZP2 decoder. |
| `0x82110D40` | `sw2e_save_game_data_write` | Writes `save:\SW2Empires.dat` and applies save thumbnail entry `0xDDC`. |
| `0x82116898` | `sw2e_wmv_movie_playback_flow` | Formats `d:\data\movie\MEV%02u.wmv` and drives movie playback. |
| `0x8215D4C0` | `sw2e_weapon_tables_init` | Loads weapon table candidates `0x62A` and `0x62B`; `0x62B` is counted as `0x30`-byte records. |
| `0x82347750` | `sw2e_archive_asset_load_wrapper` | High-level archive asset loader used by stage, character, UI, and table systems. |
| `0x82200AA8` | `sw2e_free_mode_setup_tables_load` | Loads Free Mode setup entries `0x78` and `0x79`. |
| `0x8220B210` | `sw2e_edit_state_assets_init` | Loads edit/new-officer state entry `0x80`. |
| `0x82224730` | `sw2e_startup_tables_load` | Loads startup entries `0x76`, `0x79`, and `0x78`. |
| `0x82257938` | `sw2e_scenario_control_table_loader` | Source path `sc_ctrl.cpp`; loads scenario-control archive entry `0x44` and stores a count/pointer pair. |
| `0x82260B98` | `sw2e_scenario_message_section_blob_loader` | Loads entry `0x41`, a seven-section scenario/battle message blob. |
| `0x82262678` | `sw2e_scenario_auxiliary_table_loader` | Loads entry `0x42`, a counted `595`-row auxiliary scenario table. |
| `0x8226A970` | `sw2e_relay_camera_resource_load` | Loads relay-camera resource entry `0x4D`. |
| `0x8227AAB0` | `sw2e_stage_set_first_two_bundle_loader` | Loads the first two entries of a stride-3 stage set. |
| `0x82286D10` | `sw2e_stage_bundle_loader` | Selects the current stage id and loads the third, large stride-3 stage bundle entry. |
| `0x82286B30` | `sw2e_stage_bundle_subfile_instantiator` | Walks loaded stage bundle offsets and creates runtime stage subasset objects. |
| `0x8227AE00` | `sw2e_battle_stage_scene_bootstrap` | Initializes battle-stage state and loads the three-entry stage set. |
| `0x822A85C0` | `sw2e_japan_map_resource_selector` | Selects Japan-map UI resources from the `0x82054C58` archive-id table. |
| `0x8236BBF8` | `sw2e_renderer_debug_command_parser` | Surviving renderer diagnostic parser for single-letter commands `a/c/d/f/g/m/p/t/x`. |
| `0x8236BE60` | `sw2e_pix_performance_marker_callback` | PIX-style render event/timing callback registered by the graphics diagnostics path. |
| `0x8236C020` | `sw2e_pix_diagnostic_callback_registration` | Registers the PIX command parser and performance callback during graphics initialization. |
| `0x8236D8F0` | `sw2e_guest_debug_print_formatter` | Formats diagnostic text and calls the Xbox `DbgPrint` import. |
| `0x8236D978` | `sw2e_guest_debug_callback_logger` | Formats diagnostic text and dispatches through callback or `DbgPrint`. |
| `0x8236DCE0` | `sw2e_graphics_device_diagnostic_report` | Builds graphics/device diagnostic output. |
| `0x8236DF60` | `sw2e_graphics_init_diagnostic_trap` | Graphics init diagnostic path that can trap on missing device state. |
| `0x8236E780` | `sw2e_graphics_config_probe_init` | Probes Xbox config/render state during graphics initialization. |
| `0x82351F78` | `sw2e_file_create_open` | Game file-open wrapper around `NtCreateFile`. |
| `0x82351CE8` | `sw2e_file_read` | Game file-read wrapper around `NtReadFile`. |
| `0x82352160` | `sw2e_file_write` | Game file-write wrapper around `NtWriteFile`. |
| `0x82352288` | `sw2e_file_seek` | Game seek wrapper. |

Modding/file-format notes are tracked in `docs/modding_support.md`. The file-open hooks are the
current bridge between runtime behavior and the packed `LINKDATA_BNS` archive research.
