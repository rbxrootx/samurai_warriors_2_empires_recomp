# Modding Support Notes

## Current Mod Path

- Loose-file overrides are supported with `--mod_data_root`.
- `run_recomp.bat` uses `mods\loose` automatically when that folder exists.
- A mod file should use the same path it would have under `L:\SM2\game_files`.
- Missing mod files fall back to the original extracted game files.
- Generated recomp files should stay untouched. Modding and runtime probes should live in
  `src/hooks`, docs/tools, the manifest, or the local ReXGlue SDK source.

Example:

```text
mods\loose\data\movie\MEV00.WMV
```

## Disc Data Layout

The extracted game data is small at the filesystem level. Most real content is packed into Koei
link archives.

| Path | Current read | Notes |
| --- | --- | --- |
| `data\LINKDATA_BNS.IDX` | Main archive index | Magic `SM4L`, 3549 entries. Header is 16 bytes. Entries are 16-byte big-endian records. |
| `data\LINKDATA_BNS.LNK` | Main archive payload | 0x663AC800 bytes. Indexed in 0x800-byte sectors by `LINKDATA_BNS.IDX`. |
| `data\LINKDATA_ANS.IDX` | Audio/voice-style index | 7793 entries. Header appears to be two big-endian u32 values followed by 20-byte records. |
| `data\LINK_BGM.BDX` | Music bank | Starts with `XBOX360 BGM DATA`. |
| `data\LINK_VODAT.BDX` | Voice/audio bank | Starts with a `RIFF WAVE` header. |
| `data\LINK_SEBANK.HDX` | Sound effect bank header | Starts with `SDsdVers`, `SDsdHead`, `SDsdProg`. |
| `data\LINK_SEBANK.BDX` | Sound effect bank payload | Raw paired data for `LINK_SEBANK.HDX`. |
| `data\movie\MEVxx.WMV` | Movies | WMV files. `MEV14`, `MEV15`, and `MEV17` are currently stashed for the boot workaround. |

`LINKDATA_BNS.IDX` entry format, current best read:

```text
u32be sector_offset
u32be allocated_sector_count
u32be byte_size
u32be flags_or_reserved
```

The sector size is `0x800`. The IDX header's total sector count is `0x000CC759`, which matches the
current LNK byte size exactly when multiplied by `0x800`.

Use this scanner to inspect entries:

```powershell
powershell -ExecutionPolicy Bypass -File .\tools\scan_linkdata_bns.ps1 |
  Export-Csv .\docs\linkdata_bns.scan.csv -NoTypeInformation
```

Use this extractor to build a catalog and optionally dump entries:

```powershell
powershell -ExecutionPolicy Bypass -File .\tools\extract_linkdata_bns.ps1 `
  -OutputRoot .\extracted\linkdata_bns_catalog -CatalogOnly

powershell -ExecutionPolicy Bypass -File .\tools\extract_linkdata_bns.ps1 `
  -OutputRoot .\extracted\linkdata_bns_samples\character_models_1566_1573 `
  -Start 1566 -Count 8 -Extract
```

## Main Archive Signatures

Current scan of `LINKDATA_BNS.LNK`:

| Signature | Count | Likely meaning | Why it matters |
| --- | ---: | --- | --- |
| `G1TG` | 715 | Koei texture container | Texture, UI, portrait, map texture, character texture modding. |
| `G1M_` | 306 | Koei model container | Character, weapon, prop, stage model modding. |
| `G1A_` | 41 | Koei animation container | Character/action animation path. |
| `LZP2` | 350 | Compressed data block | Likely tables, map chunks, and packed assets. Decoding this is the next big unlock. |
| `KSHL` | 16 | Small linked sub-archives/tables | First archive entries. Likely high-level tables or shared packages. |
| `[global` | 94 | Plain render configuration | Lighting/material/render config. Useful for graphics experiments. |
| `00_2` | 921 | Tiny `00_20CAP...` metadata blocks | Repeated beside character assets; likely capsule/cap/attachment metadata. |

## Candidate Ranges

These are not final labels yet. They are the best targets found from entry order, signatures, and
payload sizes.

| Entry range | Current read | Notes |
| --- | --- | --- |
| `0-15` | `KSHL0042` small packs | First-level tables or package descriptors. |
| `16-145` | Textures and early `LZP2` blocks | Likely shared UI/system/title assets. |
| `146-351` | Large `LZP2` run | Strong candidate for packed gameplay tables, text, or shared data. |
| `1566-1945` | Character/officer asset run | Manifest-confirmed run of `G1TG`, `G1M_`, `G1A_`, and `00_20CAP` entries. Start here for characters, costumes, officer animations, and attachment metadata. |
| `2062-2411` | Small model/texture alternation | Manifest-confirmed weapon/prop/equipment candidate: mostly direct `G1M_` models and `G1TG` textures, plus one embedded model bundle at `2410`. |
| `2505-2681` | Texture-heavy bank | Candidate UI/portraits/icons or shared battle textures. |
| `2762-2855` | `[global ...]` render config | Plain text-ish lighting/render setup. One entry starts with parallel light and ambient light fields. |
| `2856-2990` | Stage/map-sized groups | Repeating groups of two `LZP2` blocks plus one very large blob. Strongest current map/stage candidate. |
| `2991-3124` and `3506-3547` | Texture runs | Stage/UI texture candidates. |

## Runtime-Confirmed Table And Resource Entries

IDA MCP is now reachable at `http://127.0.0.1:13337` and was used to cross-check archive loads
against decompiled XEX functions. These entries are stronger than range guesses because the game
code directly loads them through `sw2e_archive_asset_load_wrapper`.

| Entry | Hex | Confirmed loader | Current read |
| ---: | ---: | --- | --- |
| `77` | `0x4D` | `sw2e_relay_camera_resource_load` (`0x8226A970`) | Relay camera resource loaded from `RelayCamera.cpp`; raw 1008-byte unknown blob. |
| `112-117` | `0x70-0x75` | `sw2e_japan_map_resource_selector` (`0x822A85C0`) | Japan map/UI variants selected from table `0x82054C58`; all are compressed `LZP2` entries. |
| `118` | `0x76` | `sw2e_startup_tables_load` (`0x82224730`) | Startup table/resource, compressed `LZP2`. |
| `120` | `0x78` | `sw2e_free_mode_setup_tables_load`, `sw2e_startup_tables_load` | Free Mode/startup setup table, compressed `LZP2`. |
| `121` | `0x79` | `sw2e_free_mode_setup_tables_load`, `sw2e_startup_tables_load` | Free Mode/startup setup table, raw `0x00033B59`-style unknown container. |
| `128` | `0x80` | `sw2e_edit_state_assets_init` (`0x8220B210`) | Edit/new-officer state asset table; compressed `LZP2`. |
| `65` | `0x41` | `sw2e_scenario_message_section_blob_loader` (`0x82260B98`) | Seven-section scenario/battle message blob. Section 2 has thousands of officer/dialogue callout strings. |
| `66` | `0x42` | `sw2e_scenario_auxiliary_table_loader` (`0x82262678`) | Counted auxiliary scenario table: `595` rows of `0x2C` bytes. |
| `68` | `0x44` | `sw2e_scenario_control_table_loader` (`0x82257938`) | Scenario-control table from source path `sc_ctrl.cpp`; loader stores a count/pointer pair and resets scenario globals. |
| `1578` | `0x62A` | `sw2e_weapon_tables_init` (`0x8215D4C0`) | Weapon table candidate; 4512 bytes = 141 records of `0x20` bytes. |
| `1579` | `0x62B` | `sw2e_weapon_tables_init` (`0x8215D4C0`) | Weapon table candidate; 7056 bytes = 147 records of `0x30` bytes. The game explicitly divides this entry size by `0x30`. |
| `3548` | `0xDDC` | `sw2e_save_game_data_write` (`0x82110D40`) | PNG thumbnail loaded before `XamContentSetThumbnail`; size 3723 bytes. |

The Japan-map variant table read from `0x82054C58` begins:

```text
112, 113, 114, 115, 116, 117, 111, 111,
111, 111, 111, 238, 240, 179, 296, 490
```

The next high-value table work is to decode entries `1578/1579` for weapons and entries
`112-128` for campaign/map/edit UI state. These are better first targets than scanning the full
archive blindly.

Entry `68` / `0x44` is now extracted at
`extracted\linkdata_bns_scenario_entry_68\unknown_bin\entry_0068____V_____size_0000219C.bin`.
It is not compressed; the first big-endian word is `86`, and the remaining `8600` bytes divide
exactly into `86` fixed `0x64`-byte records. `tools\scan_scenario_control_table.py` writes a
conservative CSV without assigning final field names yet:

```powershell
python .\tools\scan_scenario_control_table.py `
  .\extracted\linkdata_bns_scenario_entry_68\unknown_bin\entry_0068____V_____size_0000219C.bin `
  --csv .\extracted\linkdata_bns_scenario_entry_68\scenario_control_entry_68.csv
```

The current CSV is `extracted\linkdata_bns_scenario_entry_68\scenario_control_entry_68.csv`. Early
rows have `u16_00` values such as `15`, `11`, `16`, `270`, `271`, `512`, and `769`; treat these as
candidate packed scenario/control ids until runtime reads or caller logic confirms the meaning.

The adjacent scenario cluster entries are extracted under
`extracted\linkdata_bns_scenario_cluster_65_68`. Current structure:

| Entry | Loader | Current structure |
| ---: | --- | --- |
| `65` / `0x41` | `sw2e_scenario_message_section_blob_loader` | Seven `(offset,size)` sections. `tools\split_scenario_blob.py` writes them to `entry_0065_sections`; sections `0/1` contain battle event message templates, section `2` contains thousands of officer/dialogue callout strings, and sections `3-6` contain smaller event-dialogue groups. |
| `66` / `0x42` | `sw2e_scenario_auxiliary_table_loader` | Counted fixed table: first word `595`, then `595` records of `0x2C` bytes. Field meanings unknown. |
| `67` / `0x43` | Not yet named | Small counted table: first word `2`, then two large `0x236`-byte records if interpreted as fixed rows. Needs caller xrefs. |
| `68` / `0x44` | `sw2e_scenario_control_table_loader` | Counted fixed table: first word `86`, then `86` records of `0x64` bytes. |

Split entry `65` with:

```powershell
python .\tools\split_scenario_blob.py `
  .\extracted\linkdata_bns_scenario_cluster_65_68\unknown_bin\entry_0065__________size_0002FFDC.bin `
  --out-dir .\extracted\linkdata_bns_scenario_cluster_65_68\entry_0065_sections
```

## Range-Level Asset Manifests

`tools\build_linkdata_asset_manifest.ps1` reads `LINKDATA_BNS` directly and emits one CSV row per
archive entry. With `-DecodeLzp2`, it decodes LZP2 in memory, detects `0x00033B59` and offset-table
containers, and counts embedded `G1M_`, `G1TG`, `G1A_`, `00_20CAP`, empty slots, unknown chunks,
texture images, and G1MG geometry chunks.

Example:

```powershell
powershell -ExecutionPolicy Bypass -File .\tools\build_linkdata_asset_manifest.ps1 `
  -Start 2062 -Count 350 -DecodeLzp2 `
  -OutputPath .\extracted\linkdata_bns_manifests\manifest_weapon_prop_2062_2411.csv
```

Current manifest outputs:

| CSV | Current read |
| --- | --- |
| `extracted\linkdata_bns_manifests\manifest_character_officer_1566_1945.csv` | Top-level entries: 83 `G1M_` models, 88 `G1TG` texture containers, 41 `G1A_` animations, 74 `00_20CAP` metadata entries, 6 scanned unknowns, 87 zero-size placeholders, and one oversized unknown entry at `1575`. The texture entries contain 367 texture images total. |
| `extracted\linkdata_bns_manifests\manifest_character_officer_1566_1945_runs.csv` | Consecutive run summary for the character/officer band. Shows the repeating model/animation/cap/texture cadence from `1610` onward. |
| `extracted\linkdata_bns_manifests\manifest_weapon_prop_2062_2411.csv` | Top-level entries: 222 direct `G1M_` models, 127 direct `G1TG` texture containers, and entry `2410` as an offset-table bundle with 23 embedded `G1M_` models. Total model count across the range is 245. |
| `extracted\linkdata_bns_manifests\manifest_weapon_prop_2062_2411_runs.csv` | Consecutive run summary for weapon/prop candidates. The early stretch alternates texture/model pairs; later stretches include long model-only runs. |
| `extracted\linkdata_bns_manifests\manifest_stage_representative_2856_2870.csv` | Five representative normal stage groups. Each group follows the confirmed three-entry pattern: first entry has 3 models plus 3 texture containers and 1 unknown chunk; second entry has 5 unknown metadata chunks; third entry is a large texture offset table. Across these 15 entries: 15 models, 15 G1MG chunks, 2682 texture containers, 4675 texture images, 2778 empty slots, and 30 unknown metadata chunks. |

For stage work, prefer scanning one stage group at a time:

```powershell
powershell -ExecutionPolicy Bypass -File .\tools\build_linkdata_asset_manifest.ps1 `
  -Start 2856 -Count 3 -DecodeLzp2 `
  -OutputPath .\extracted\linkdata_bns_manifests\manifest_stage_2856_2858.csv
```

The full `2856-2990` pass is valid in theory but heavy in Windows PowerShell because many stage
third-bundle entries are large raw offset tables. Chunking by the runtime's three-entry stage group
keeps memory bounded and mirrors the game loader.

## Decoded Container Layers

### LZP2

`LZP2` is now decoded well enough to extract usable payloads.

Header, current read:

```text
0x00 char[4]  "LZP2"
0x04 u32le    constant 0x3F8147AE
0x08 u32le    decompressed_size
0x0C u32le    compressed_payload_size
0x10 bytes    compressed stream
```

The stream uses literal runs, back-references, and repeated-byte runs. The local decoder is:

```powershell
powershell -ExecutionPolicy Bypass -File .\tools\decompress_lzp2.ps1 `
  -InputPath .\extracted\linkdata_bns_samples\lzp2_probe\compressed_lzp2\entry_0019_LZP2_G___size_003D0890.lzp2 `
  -OutputDirectory .\extracted\linkdata_bns_samples\lzp2_decompressed -Force
```

IDA confirms the game decoder at `0x82116600` (`sw2e_lzp2_decode`). Its stream control byte layout:

```text
0x00                 end of compressed stream
0x01..0x3F           literal run length, followed by that many bytes
0x40..0x7F           repeated-byte run, next byte extends length, following byte is value
0x80..0xFF           back-reference, next byte extends offset
```

The encoder in `tools\compress_lzp2_literal.ps1` writes valid literal-only streams and includes the
required zero terminator for the real game decoder.

Verified samples:

| Entry | Compressed | Decompressed | Output read |
| ---: | ---: | ---: | --- |
| `19` | `0x003D0890` | `0x00E20AB0` | Direct `G1TG0040` texture container. |
| `20` | `0x001BF050` | `0x0083B520` | `0x00033B59` inner container with 78 embedded `G1TG` textures. |
| `21` | `0x00193480` | `0x002C66D0` | `0x00033B59` inner container with 177 embedded `G1TG` textures. |
| `69` | `0x0005FB70` | `0x00172520` | `0x00033B59` inner container with 78 embedded `G1TG` textures. |
| `2856` | `0x0031F900` | `0x003B87D0` | Offset-table stage container with `G1M_` and `G1TG` chunks. |
| `2857` | `0x000913D0` | `0x00184400` | Offset-table stage data container. |

### 0x00033B59 Texture Container

Some decompressed LZP2 payloads begin with `00 03 3B 59`. Current structure:

```text
0x00 u32be magic             0x00033B59
0x04 u32be entry_count
0x08 u32be table_offset      usually 0x10
0x0C u32be unknown_or_flags  currently 0
0x10 records[entry_count]:
     u32be offset_div_0x10
     u32be byte_size
```

The payload offset is `offset_div_0x10 * 0x10`. The splitter is:

```powershell
powershell -ExecutionPolicy Bypass -File .\tools\split_003b59_container.ps1 `
  -InputPath .\extracted\linkdata_bns_samples\lzp2_decompressed\entry_0021_LZP2_G___size_00193480.decompressed.bin `
  -OutputDirectory .\extracted\linkdata_bns_samples\entry_0021_split -Force
```

`entry_0021` split into 177 `G1TG0040` texture files.

### Offset-Table Container

Several stage/map candidates use a simpler big-endian offset table:

```text
0x00 u32be entry_count
0x04 u32be offsets[entry_count]
```

Each offset is absolute within the container. Size is `next_offset - current_offset`, with the final
entry ending at EOF. The splitter is:

```powershell
powershell -ExecutionPolicy Bypass -File .\tools\split_offset_table_container.ps1 `
  -InputPath .\extracted\linkdata_bns_samples\stage_2856_2858\unknown_bin\entry_2858____c_____size_00D0BA90.bin `
  -OutputDirectory .\extracted\linkdata_bns_samples\stage_2858_split -Force
```

For read-only classification without writing split files, use:

```powershell
powershell -ExecutionPolicy Bypass -File .\tools\scan_stage_bundle.ps1 `
  -InputPath .\extracted\linkdata_bns_samples\stage_2856_2858_decompressed\entry_2856_LZP2_G___size_0031F900.decompressed.bin `
  -OutputPath .\extracted\linkdata_bns_samples\stage_2856_bundle_scan.csv
```

Verified stage sample:

| Entry | Split result |
| ---: | --- |
| `2856` | 7 chunks: 3 `G1M_0030` models, 3 `G1TG0040` texture containers, 1 unknown chunk. |
| `2857` | 5 unknown chunks, likely stage metadata/collision/path or placement data. |
| `2858` | 611 offset-table slots: 299 non-empty `G1TG0040` texture containers and 312 empty alias slots. |

This means the `2856-2990` stage range is now editable at least at the extracted model/texture-file
level, pending repacking.

## Repacking And Injection Tools

The first safe injection path is now tool-complete for `LINKDATA_BNS` entries:

| Layer | Tool | Verification |
| --- | --- | --- |
| `0x00033B59` texture bundle | `tools\pack_003b59_container.ps1` | Entry `21` rebuilt byte-for-byte from 177 split `G1TG` files. |
| Offset-table stage bundle | `tools\pack_offset_table_container.ps1` | Entry `2858` rebuilt byte-for-byte from 611 split chunks. |
| `LZP2` compression | `tools\compress_lzp2_literal.ps1` | Literal stream decompresses back to exact original bytes and includes the game-required zero terminator. |
| `LINKDATA_BNS.IDX/LNK` | `tools\repack_linkdata_bns.ps1` | No-patch dry-run preserves original `3549` entries, `837465` sectors, and `1715128320` bytes. |

Example edit loop for a compressed texture bundle:

```powershell
powershell -ExecutionPolicy Bypass -File .\tools\pack_003b59_container.ps1 `
  -SplitDirectory .\extracted\linkdata_bns_samples\entry_0021_split `
  -OutputPath .\work\entry_0021_repacked.bin -Force

powershell -ExecutionPolicy Bypass -File .\tools\compress_lzp2_literal.ps1 `
  -InputPath .\work\entry_0021_repacked.bin `
  -OutputPath .\mods\linkdata_bns_patch\compressed_lzp2\entry_0021_LZP2_G___size_00193480.lzp2 -Force

powershell -ExecutionPolicy Bypass -File .\tools\repack_linkdata_bns.ps1 `
  -PatchRoot .\mods\linkdata_bns_patch `
  -OutputDirectory .\out\modded_linkdata_bns -DryRun
```

Drop replacement files under `mods\linkdata_bns_patch` using the same `RelativePath` shown in the
catalog CSV. Run without `-DryRun` to build a new `LINKDATA_BNS.IDX/LNK` pair. The literal LZP2
encoder is usually larger than the original compressed stream, so use the full archive repacker
rather than in-place replacement unless the new byte size fits the original allocation.

## IDA-Confirmed Archive Runtime

The archive read path is now named in IDA and mirrored in `src\hooks\function_map.cpp`.

| Address | IDA name | Current read |
| --- | --- | --- |
| `0x8210EF50` | `sw2e_archive_filesystem_bootstrap` | Initializes archive/file system state. |
| `0x8210E0D8` | `sw2e_init_linkdata_archive_tables` | Opens `LINKDATA_BNS.LNK/IDX`, `LINKDATA_DMY.LNK/IDX`, and `LINK_VODAT.BDX` plus `LINKDATA_ANS.IDX`; expands IDX records into runtime tables. |
| `0x8210E528` | `sw2e_archive_read_entry_sectors` | Core entry reader. Decodes packed archive id, seeks to sector offset, reads sector-aligned payload, then calls LZP2 postprocess. |
| `0x82347750` | `sw2e_archive_asset_load_wrapper` | High-level archive asset loader used by stage, character, UI, and table systems. It maps source `0/1/2` to archive selector bits and either allocates or uses a caller buffer before reading. |
| `0x8210D9F8` | `sw2e_archive_lzp2_postprocess` | Detects `LZP2` and dispatches to the decoder. |
| `0x82116600` | `sw2e_lzp2_decode` | In-place LZP2 decoder. |
| `0x8210DBD0` | `sw2e_archive_entry_size_bytes` | Returns byte size for a packed archive id. |
| `0x8210DC18` | `sw2e_archive_entry_alloc_sectors` | Returns allocated sector count for a packed archive id. |
| `0x8210E4A8` | `sw2e_archive_select_handle` | Chooses archive handle or special temporary per-file handle. |

Packed archive id, current read:

```text
bits  0..25  entry index
bits 28..28  archive table selector when bit 30 is clear
bit      30  force selector 2
```

Archive selector `0` is the main BNS archive path we care about most for maps, characters, weapons,
UI, and shared gameplay data.

## IDA-Confirmed Stage Runtime

The first real stage-loader path is now identified. IDA shows `0x8227AE00` bootstrapping battle
stage scene state and using a three-entry stage set. The first two entries are loaded by
`0x8227AAB0`; the third large bundle is loaded by `0x82286D10`.

```text
normal stage set:
  first bundle:  3 * stage_id + 2856
  second bundle: 3 * stage_id + 2857
  third bundle:  3 * stage_id + 2858

alternate/override stage set:
  first bundle:  3 * stage_id + 2937
  second bundle: 3 * stage_id + 2938
  third bundle:  3 * stage_id + 2939
```

The loaded entries are offset-table bundles. `0x82286950` stores the bundle subfile count and
allocates the runtime subasset table. `0x82286B30` walks the offsets inside the bundle, turns each
subfile into a runtime object, and grafts those objects into the stage system. `0x82286A70` is the
matching cleanup path.

Current named stage functions:

| Address | IDA name | Current read |
| --- | --- | --- |
| `0x8227AE00` | `sw2e_battle_stage_scene_bootstrap` | Initializes battle-stage globals, fixes offsets in a stage metadata table, loads the three-entry stage set, then wires terrain/placement systems. |
| `0x8227AD30` | `sw2e_battle_stage_scene_cleanup` | Frees battle-stage buffers and calls the stage subasset cleanup path. |
| `0x8227AAB0` | `sw2e_stage_set_first_two_bundle_loader` | Loads the first two entries of a stride-3 stage set. |
| `0x82286D10` | `sw2e_stage_bundle_loader` | Selects the current stage id and loads the third, large stride-3 stage bundle entry. |
| `0x82286B30` | `sw2e_stage_bundle_subfile_instantiator` | Walks bundle subfiles and creates runtime objects for each stage subasset. |
| `0x82286950` | `sw2e_stage_subasset_table_allocator` | Allocates the runtime stage subasset table from the loaded bundle's first word. |
| `0x822869D8` | `sw2e_stage_metadata_row_lookup` | Finds a 100-byte row in the global stage metadata table, with special rows for modes 2 and 3. |
| `0x82286A70` | `sw2e_stage_subasset_table_cleanup` | Releases stage subasset runtime objects and frees their table. |

Important nuance: entries `2856`, `2857`, and `2858` are the normal stride-3 stage set, while
entries `2937`, `2938`, and `2939` are the matching alternate/override set. That means entry `2856`
is no longer just a nearby stage/model candidate; it is the first normal stage-set bundle loaded by
the stage path.

Latest smoke evidence from `runtime.asset-hook-smoke.log`: the battle/stage path loaded entries
`2943`, `2944`, and `2945`. Entries `2943` and `2944` came from the first-two loader path at
`0x8227AAB0`; entry `2945` came from `0x82286D10` at LR `0x82286E20` with `immediate=0`, then the
archive worker read it asynchronously. These match the alternate formulas with `stage_id = 2`.

## Blender Map Editor Path

The practical path toward a Blender map editor is:

1. Use `tools\extract_linkdata_bns.ps1` to extract stage candidates, starting with entries
   `2856-2990`.
2. Decompress LZP2 entries with `tools\decompress_lzp2.ps1`.
3. Split stage containers with `tools\split_offset_table_container.ps1`.
4. Export `G1M_` chunks to OBJ with `tools\export_g1m_obj.py` for first-pass Blender inspection.
5. Treat the unknown stage chunks as the next reverse-engineering target for collision, navigation,
   spawn points, object placement, weather, and scenario metadata.
6. Add edited-geometry import/repack support, then rebuild with `pack_offset_table_container.ps1`,
   optionally recompress with `compress_lzp2_literal.ps1`, and rebuild `LINKDATA_BNS` with
   `repack_linkdata_bns.ps1`.

Current stage anchor:

| Entry | Editor relevance |
| ---: | --- |
| `2856` | Contains 3 stage `G1M_` models, 3 `G1TG` texture containers, 16 texture images total, and 1 unknown chunk. Best first Blender import target. |
| `2857` | 5 unknown stage metadata chunks. Best next target for collision/path/placement decoding. |
| `2858` | Large texture bundle: 299 `G1TG` texture containers, 447 texture images, and 312 empty offset slots. Useful for stage texture editing and chunk classification. |
| `2943` | Runtime-confirmed alternate first bundle for `stage_id = 2`; contains 3 `G1M_` models, 3 `G1TG` texture containers, and 1 unknown chunk. |
| `2944` | Runtime-confirmed alternate second bundle for `stage_id = 2`; currently 5 unknown metadata-style chunks. |
| `2945` | Runtime-confirmed alternate third bundle for `stage_id = 2`; contains 589 `G1TG` texture containers and 636 empty offset slots. |

### G1M Model Chunk Notes

`tools\scan_g1m.ps1` now catalogs top-level G1M chunks:

```powershell
powershell -ExecutionPolicy Bypass -File .\tools\scan_g1m.ps1 `
  -InputPath .\extracted\linkdata_bns_samples\entry_2856_LZP2_G___size_0031F900.decompressed_split `
  -OutputPath .\extracted\linkdata_bns_samples\stage_2856_g1m_chunks.csv
```

Current stage `2856` G1M read:

| Chunk | Version | Current read |
| --- | --- | --- |
| `G1M_` | `0030` | Top-level header. Offset `0x0C` points to first chunk at `0x18`; offset `0x14` is a chunk-count hint. |
| `G1MF` | `0020` | Fixed-size metadata block, `0x88` bytes in the stage and character samples. |
| `G1MS` | `0030` | Skeleton/scene-style block, `0x50` bytes in the current samples. |
| `G1MM` | `0020` | Material/model metadata block, `0x50` bytes in the current samples. |
| `G1MG` | `0030` | Large geometry payload. First Blender importer target. Now expanded by `tools\scan_g1m.ps1` into geometry subsections. |

Stage entry `2856` contains three G1M models. Their `G1MG` chunks begin at `0x140` and are
`46324`, `40308`, and `52400` bytes. Character samples `1566-1573` use the same top-level chunk
pattern, with top-header `unknown10=1` instead of `0` in the stage models.

Using Project-G1M's public headers as the naming reference, the local scanner now recognizes these
`G1MG` section ids:

| Section id | Scanner name | Current read |
| ---: | --- | --- |
| `0x00010001` | `G1MG.section1` | Per-mesh/group header data; still needs SW2E-specific field names. |
| `0x00010002` | `G1MG.materials` | Material records. |
| `0x00010003` | `G1MG.section3` | Small unknown section. |
| `0x00010004` | `G1MG.vertex_buffers` | Packed vertex data. |
| `0x00010005` | `G1MG.vertex_attributes` | Vertex declarations: position, normal, UV, color, joint data, etc. |
| `0x00010006` | `G1MG.joint_palettes` | Joint palette data. |
| `0x00010007` | `G1MG.index_buffers` | Triangle/index data. |
| `0x00010008` | `G1MG.submeshes` | Submesh records. |
| `0x00010009` | `G1MG.mesh_groups` | Mesh group and LOD records. |

Current SW2E stage G1MG headers report platform `0x44583900` (`DX9\0`) and 9 geometry sections.
Useful model counts from the regenerated CSVs:

| Source | Model | Materials | Submeshes | Vertex buffer bytes | Index buffer bytes |
| --- | --- | ---: | ---: | ---: | ---: |
| `2856` | `sub_0001` | 1 | 1 | 40104 | 5820 |
| `2856` | `sub_0002` | 3 | 3 | 36904 | 2764 |
| `2856` | `sub_0006` | 1 | 1 | 45264 | 6736 |
| `2943` | `sub_0001` | 1 | 1 | 30024 | 4280 |
| `2943` | `sub_0002` | 10 | 14 | 79544 | 5896 |
| `2943` | `sub_0006` | 1 | 1 | 34184 | 4976 |

The parsed stage vertex declarations are consistent across `2856` and `2943` so far:

| Semantic | Data type | Stream offset | Blender relevance |
| --- | --- | ---: | --- |
| `Position` | `Float_x3` | 0 | Vertex position. |
| `JointIndex` | `UByte_x4` | 12 | Skin/joint lookup; may be unused or simple palette data for stage pieces. |
| `Normal` | `Float_x3` | 16 | Vertex normals. |
| `Color` | `NormUByte_x4` | 28 | Vertex color layer. |
| `UV` | `Float_x2` | 32 | Texture coordinates. |

The scanner also emits `G1MG.index_buffer` rows with index counts/data type and `G1MG.submesh`
rows with material index, vertex range, and index range. The raw submesh type field is still
unlabeled for SW2E, but the vertex/index/material ranges are already usable importer data.

### OBJ Geometry Export

`tools\export_g1m_obj.py` is the first local one-way geometry exporter for Blender inspection. It
parses the SW2E big-endian `G1M_0030` / `G1MG0040` subset seen so far: vertex buffers, vertex
attributes, index buffers, submeshes, triangle lists, triangle strips, and strip restart indices.
It currently writes positions, normals, UVs, OBJ material slots, and simple `.mtl` files containing
the raw material texture indices/layers/types/tile modes. It does not yet decode texture images into
the material file, nor export skinning, skeletons, or animations. Matching edited OBJ geometry can be
patched back into `G1M_` with `tools\patch_g1m_from_obj.py`.

Example commands:

```powershell
python .\tools\export_g1m_obj.py `
  .\extracted\linkdata_bns_samples\entry_2856_LZP2_G___size_0031F900.decompressed_split `
  --out-dir .\extracted\g1m_obj_exports\stage_2856 `
  --scale 0.01 --flip-v

python .\tools\export_g1m_obj.py `
  .\extracted\linkdata_bns_samples\runtime_stage_2943_split `
  --out-dir .\extracted\g1m_obj_exports\runtime_stage_2943 `
  --scale 0.01 --flip-v

python .\tools\export_g1m_obj.py `
  .\extracted\linkdata_bns_samples\weapon_prop_2410_split `
  --out-dir .\extracted\g1m_obj_exports\weapon_prop_2410 `
  --flip-v
```

Current OBJ export validation:

| Source | OBJ files | Vertices | Faces | Warnings |
| --- | ---: | ---: | ---: | ---: |
| `stage_2856` | 3 | 3055 | 4456 | 0 |
| `runtime_stage_2943` | 3 | 3592 | 3912 | 0 |
| `character_models_1566_1573` | 8 | 2250 | 1607 | 0 |
| `weapon_prop_2410` | 23 | 5018 | 3624 | 0 |
| `material_slot_smoke` | 1 | 1002 | 1829 | 0 |

The generated `export_manifest.csv` files live beside the OBJ outputs:

| Output folder | Current read |
| --- | --- |
| `extracted\g1m_obj_exports\stage_2856` | Normal stage anchor models from entry `2856`. |
| `extracted\g1m_obj_exports\runtime_stage_2943` | Runtime-confirmed alternate stage models from entry `2943`. |
| `extracted\g1m_obj_exports\character_models_1566_1573` | Eight small character/officer-range model samples. |
| `extracted\g1m_obj_exports\weapon_prop_2410` | Entry `2410` split into 23 `G1M_` weapon/prop models, all exported. |
| `extracted\g1m_obj_exports\material_slot_smoke` | Single stage model smoke after adding `.mtl` material-slot export. |

### Runtime Gap OBJ Previews

`tools\export_native_gap_obj.py` is a separate diagnostic bridge for live runtime draw samples. It
does not parse archive `G1M_` files; instead, it reads a native-renderer `samples.jsonl` capture from
`run_recomp_native_gap_sample_probe.bat`, pairs sampled vertex/index buffers by frame and draw, and
writes simple OBJ previews for unsupported native layout/transform draws.

Example:

```powershell
.\run_recomp_native_gap_sample_probe.bat

python .\tools\export_native_gap_obj.py `
  .\extracted\native_render_samples\native_gap_probe_20260718-001120 `
  --max-draws 10
```

Validation on `native_gap_probe_20260718-001120` exported ten transform-gap OBJ previews and two
layout-gap OBJ previews from real gameplay draw data while keeping the large native event JSON stream
disabled. The newer `native_gap_probe_20260718-002329` sample adds compact vertex/pixel float
constant snapshots to every manifest row, and `tools\export_native_gap_obj.py` now writes a
`gap_obj_manifest.csv` with raw bounds, constant indices, and heuristic projection candidates beside
the OBJ files. Those projection fields are renderer research leads, not final camera math.

Non-indexed draw previews are clamped to the draw's submitted `index_count`, not the full sampled
vertex-buffer byte range. This matters for repeated strip families such as
`VS=0xD5CCD0C915DDCC0B`, where one draw submits a four-vertex strip even though the sampled backing
buffer is much larger.

This gives the future Blender/map-editor path two complementary sources: archive-side `G1M_` exports
for editable assets, and runtime-side gap OBJs for correlating what the game actually draws in
battle.

### OBJ Geometry Reimport

`tools\patch_g1m_from_obj.py` is the first conservative geometry edit path. It takes an original
`G1M_` plus a matching OBJ from `export_g1m_obj.py`, then patches the existing position, normal, and
UV float streams in-place. It preserves the original G1M size, chunk layout, indices, materials,
skeleton/joint data, and unknown fields.

Current limitations are intentional:

- The OBJ must keep the exported vertex order and vertex count.
- It does not add/remove vertices, faces, materials, bones, textures, or submeshes.
- Use the same `--scale` and `--flip-v` values that were used for export.

Example no-edit roundtrip for the normal stage anchor:

```powershell
python .\tools\patch_g1m_from_obj.py `
  .\extracted\linkdata_bns_samples\entry_2856_LZP2_G___size_0031F900.decompressed_split `
  .\extracted\g1m_obj_exports\stage_2856 `
  --out .\extracted\g1m_roundtrip\stage_2856 `
  --scale 0.01 --flip-v

python .\tools\export_g1m_obj.py `
  .\extracted\g1m_roundtrip\stage_2856 `
  --out-dir .\extracted\g1m_roundtrip\stage_2856_obj_check `
  --scale 0.01 --flip-v
```

Validation so far:

| Test | Result |
| --- | --- |
| Stage `2856` no-edit OBJ -> G1M -> OBJ | 3 files patched, 3055 vertices, 4456 faces, 0 warnings after re-export. |
| Controlled one-vertex edit | First exported vertex changed from `v 0 37.6050391 -750` to `v 1 37.6050391 -750`, then survived OBJ -> G1M -> OBJ. |

### LINKDATA Entry Patch Build

`tools\build_linkdata_entry_patch.ps1` wires patched subfiles into the existing archive pipeline.
It overlays matching replacement files onto a split offset-table entry, repacks the offset table,
optionally wraps the result in literal `LZP2`, and writes the output to a patch root with the same
relative path expected by `tools\repack_linkdata_bns.ps1`.

For compressed stage entries, pass `-CompressLzp2`. For uncompressed offset-table entries such as
weapon/prop bundle `2410`, omit it.

Preview command used for the controlled stage edit:

```powershell
powershell -ExecutionPolicy Bypass -File .\tools\build_linkdata_entry_patch.ps1 `
  -EntryIndex 2856 `
  -SplitDirectory .\extracted\linkdata_bns_samples\entry_2856_LZP2_G___size_0031F900.decompressed_split `
  -ReplacementDirectory .\extracted\g1m_roundtrip\stage_2856_control_edit `
  -PatchRoot .\extracted\mod_build\linkdata_bns_patch_preview `
  -WorkDirectory .\extracted\mod_build\entry_2856_control_edit `
  -CompressLzp2 -DryRunArchive -Force
```

Validation for that preview patch:

| Check | Result |
| --- | --- |
| Replacement count | 1 patched `G1M_` subfile in entry `2856`. |
| Repacked stage container | 3901328 bytes; splits back into the expected 7 subfiles. |
| Literal LZP2 patch | `extracted\mod_build\linkdata_bns_patch_preview\compressed_lzp2\entry_2856_LZP2_G___size_0031F900.lzp2`, magic `LZP2.G.?`, 3963271 bytes. |
| BNS repack dry-run | 3549 entries, 1 patched entry, output size 1715816448 bytes. |
| Full decode verification | Patch LZP2 decompresses, splits, exports to OBJ with 3055 vertices, 4456 faces, and 0 warnings. |
| Edited vertex evidence | First vertex remains `v 1 37.6050391 -750` after OBJ -> G1M -> stage container -> LZP2 -> split -> OBJ. |

Full archive rebuild verification:

| Check | Result |
| --- | --- |
| Output archive | `out\modded_linkdata_bns_preview\LINKDATA_BNS.IDX` plus `LINKDATA_BNS.LNK`. |
| Rebuild summary | 3549 entries, 1 patched entry, 837801 sectors, 1715816448 LNK bytes. |
| Extracted rebuilt entry `2856` | `Magic8=LZP2.G.?`, size 3963271 bytes, alloc sectors 1936. |
| Rebuilt entry decode | Decompresses to 3901328 bytes, then splits back into 7 subfiles. |
| Rebuilt OBJ verification | Exports 3 G1M models with 3055 vertices, 4456 faces, and 0 warnings. |
| Patched subfile hash | Rebuilt archive's `sub_0001` SHA256 matches the controlled patched G1M SHA256: `0797C73C779D53FAA09A0AE8F3080C6FAF79BFC1948312A79127F74471873C53`. |
| Edited vertex evidence | Extracted-from-rebuilt OBJ still starts with `v 1 37.6050391 -750`. |

Runtime smoke verification:

| Check | Result |
| --- | --- |
| Partial mod-root attempt | Mounting only `data\LINKDATA_BNS.IDX/LNK` made `LINKDATA_DMY.LNK` fail in the overlay and triggered the dirty-disc path. Do not use partial `data` directories for archive smoke tests. |
| Full hard-linked data mirror | `extracted\mod_build\smoke_modroot_patched_archive_full_data` hard-links the full `game_files\data` tree and overrides only `LINKDATA_BNS.IDX/LNK`. |
| 45-second smoke | `runtime.patched-archive-full-data-smoke.log`: patched overlay mounted, `LINKDATA_BNS.LNK/IDX` opened successfully, no fatal/assert/crash/dirty-disc lines. Known nonfatal GPU resolve warnings remain. |
| 65-second pulsed smoke | `runtime.patched-archive-full-data-pulse-smoke.log`: process stayed alive for full window, patched overlay mounted, `LINKDATA_BNS` opened successfully, no fatal/assert/crash/dirty-disc lines, 238 archive/hook matches. |
| Input automation note | Built-in synthetic boot input is opt-in via `--sw2e_auto_boot_input=true`. Later menus, such as difficulty selection, still require manual input or `tools\pulse_recomp_input.ps1`. |

`tools\pulse_recomp_input.ps1` can pulse the recomp window's mapped keyboard button during smoke
tests. It supports foreground `SendInput` pulses and a fallback `PostMessage` mode:

```powershell
powershell -ExecutionPolicy Bypass -File .\tools\pulse_recomp_input.ps1 `
  -InitialDelaySeconds 12 -DurationSeconds 45 -IntervalMs 900 -Key Space
```

`run_recomp.bat` now uses the same playable MnK layout as Visual Studio debugging: `W/A/S/D` on
movement, `LMB` as normal attack, `RMB` as charge/strong attack, `Space` as A/confirm, `Shift` as B,
and `Escape` as Start. `run_recomp_visible_cursor.bat` keeps the host cursor visible with the same
mapping for future menu/editor experiments, but captured mouse is still the better gameplay mode.
Neither normal launcher enables synthetic boot input; add `--sw2e_auto_boot_input=true` only when
running unattended tests.

`tools\scan_stage_bundle.ps1` now writes a Blender-facing manifest for a decompressed offset-table
bundle. Current outputs:

| CSV | Current read |
| --- | --- |
| `extracted\linkdata_bns_samples\stage_2856_bundle_scan.csv` | 7 subfiles: 1 unknown metadata chunk, 3 `G1M_0030` models, 3 `G1TG0040` texture containers. |
| `extracted\linkdata_bns_samples\stage_2858_bundle_scan.csv` | 611 offset-table slots: 299 non-empty `G1TG0040` texture containers and 312 empty slots. |
| `extracted\linkdata_bns_samples\runtime_stage_2943_bundle_scan.csv` | 7 subfiles: 1 unknown metadata chunk, 3 `G1M_0030` models, 3 `G1TG0040` texture containers. |
| `extracted\linkdata_bns_samples\runtime_stage_2944_bundle_scan.csv` | 5 unknown metadata-style chunks. |
| `extracted\linkdata_bns_samples\runtime_stage_2945_bundle_scan.csv` | 1225 offset-table slots: 589 non-empty `G1TG0040` texture containers and 636 empty slots. |
| `extracted\linkdata_bns_samples\stage_2856_g1m_chunks.csv` | Top-level G1M chunks plus expanded `G1MG` geometry sections for the normal stage-model bundle. |
| `extracted\linkdata_bns_samples\runtime_stage_2943_g1m_chunks.csv` | Expanded `G1MG` geometry sections for the runtime-confirmed alternate stage-model bundle. |
| `extracted\linkdata_bns_samples\character_models_1566_1573_g1m_chunks.csv` | Expanded `G1MG` geometry sections for eight character-range model samples. |

### External Tooling Leads

Existing public tools are useful for comparison and format sanity checks:

- Project-G1M is the main Joschuka reference repo for practical native C++ `G1M` handling:
  https://github.com/Joschuka/Project-G1M
- `gust_stuff` includes G1M mesh export/import scripts and Blender-adjacent workflows for Atelier
  games: https://github.com/eArmada8/gust_stuff

Our editor path is to build a SW2E-focused Blender importer/exporter from the extracted stage
bundles, IDA names, runtime hooks, and local format notes. External tools are references, not the
foundation of the workflow.

Project-G1M cross-check notes from the current source:

- It matches the `G1MG` section ids already in our scanner: `0x00010002` materials,
  `0x00010004` vertex buffers, `0x00010005` vertex attributes, `0x00010006` joint palettes,
  `0x00010007` index buffers, `0x00010008` submeshes, and `0x00010009` mesh groups.
- Its vertex semantics match our exporter assumptions: position `0`, joint weight `1`, joint index
  `2`, normal `3`, UV `5`, tangent `6`, binormal `7`, color `10`, plus a few less-used render
  semantics.
- Useful vertex data types to add beyond the current float-only OBJ bridge: half-float vec2/vec4
  (`0x0A`, `0x0B`), normalized ubyte vec4 (`0x0D`), ubyte vec4 (`0x05`), ushort vec4 (`0x07`), and
  tentative uint vec4 (`0x09`).
- Its 56-byte submesh layout lines up with our parser: attribute/vertex-buffer set at `+0x04`, bone
  palette at `+0x08`, material index at `+0x18`, index-buffer index at `+0x1C`, primitive type at
  `+0x24`, vertex range at `+0x28/+0x2C`, and index range at `+0x30/+0x34`.
- Material texture records are the next practical Blender target: each material has texture records
  containing texture index, UV layer, texture type, and tile modes. That should let us write OBJ
  `.mtl` or a native Blender importer that pairs stage `G1M_` chunks with neighboring `G1TG`
  texture containers.

## Runtime Debug Anchors

Useful project-layer hooks for mapping internals:

| Address | Hook | Use |
| --- | --- | --- |
| `0x8249E974` | `__imp__NtCreateFile` | Logs guest file paths and returned handles. |
| `0x8249E9D4` | `__imp__NtOpenFile` | Logs guest file paths and returned handles. |
| `0x8249E934` | `__imp__NtReadFile` | Logs reads by handle, length, and offset. |
| `0x8249E994` | `__imp__NtWriteFile` | Logs writes by handle, length, and offset. |
| `0x8249E7D4` | `__imp__XamContentCreateEx` | Save container creation/opening. |
| `0x8249EBE4` | `__imp__DbgPrint` | Mirrors surviving guest developer diagnostics into the PC runtime log. |
| `0x8210E528` | `sw2e_archive_read_entry_sectors` | Best hook candidate for logging archive entry ids at runtime. |
| `0x82347750` | `sw2e_archive_asset_load_wrapper` | Higher-level hook for logging who requested archive entries, including stage/character/texture loads. |
| `0x8210D9F8` | `sw2e_archive_lzp2_postprocess` | Best hook candidate for logging decompressed entry outputs. |
| `0x8236BBF8` | `sw2e_renderer_debug_command_parser` | Logs a surviving renderer diagnostic parser with commands `a/c/d/f/g/m/p/t/x`. |
| `0x8236BE60` | `sw2e_pix_performance_marker_callback` | PIX-style render event/timing callback; useful when probing leftover render diagnostics. |
| `0x8236C020` | `sw2e_pix_diagnostic_callback_registration` | Registers the parser as command `28` and the marker callback as id `47`. |
| `0x8236DCE0` | `sw2e_graphics_device_diagnostic_report` | Graphics/device diagnostic output path; useful for future renderer replacement work. |
| `0x82369820` | `sub_82369820` | High-frequency draw packet writer; useful render heartbeat. |

The new file-open logs are the practical way to connect runtime behavior to archive/file reads. Run
the game, enter a mode, then search `runtime.log` for `SM2 file import call`.

The new debug-output logs are the practical way to surface any dormant developer diagnostics. Search
`runtime.log` for `SM2 guest DbgPrint` and `SM2 debug command parser` while entering boot, menus,
edit mode, map screens, and battles.

## Next Modding Work

- Use the `0x82347750` asset-load logs while entering actual battles to correlate stage ids,
  scenario/mode state, and archive entries across more maps.
- Add explicit bounded runtime logs around stage loaders `0x8227AAB0`, `0x82286D10`, and
  `0x82286B30`: selected stage id, the three-entry archive group, returned object pointers, and LR.
  That should connect Blender-side stage edits to exact `LINKDATA_BNS` entries without broad scans.
- Decode archive entry `68` (`0x44`) and the globals around `0x827A6B70` to map scenario-control
  tables; this is likely a better campaign/script lead than the stale debug-menu strings.
- Use the full-data hard-link mirror pattern for future patched archive smoke tests; avoid partial
  `data` overlays unless the overlay fallback behavior is changed.
- Identify the unknown chunks in stage containers; these are likely where collision, pathing,
  placement, weather, or scenario-specific stage metadata lives.
- Extend the G1M material/texture binding map so Blender exports can pair submeshes with neighboring
  `G1TG` textures instead of carrying only raw material slot numbers.
- Use the decoded `LZP2` data to search for economy/officer/weapon tables. The economy is likely in
  table-like binary chunks rather than the direct model/texture files.
- Name the archive loader, model loader, texture loader, animation loader, and stage loader functions
  in IDA as runtime stacks identify them.

## Future Graphics API Direction

Replacing or bypassing the current ReXGlue/Xenia-derived render path is possible later, but the sane
path is staged:

1. Keep ReXGlue rendering stable while mapping `G1TG`, `G1M_`, `G1A_`, and `[global]` render config.
2. Add asset extraction/viewer tooling first, so custom rendering has real inputs.
3. Add runtime hooks around asset decode and draw submission.
4. Only then consider a new `IGraphicsSystem` backend or a higher-level renderer fed by decoded game
   assets.

For modding, archive entry overrides and data format decoding matter more right now than replacing
the renderer.
