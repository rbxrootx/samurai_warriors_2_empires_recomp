# External Tooling Notes

This project keeps its core modding path SW2E-specific, but public Koei/Omega Force tooling is
useful for naming, cross-checking, and format sanity checks.

## Format References

| Tool/reference | Useful for | Notes |
| --- | --- | --- |
| [ReXGlue SDK docs](https://rexglue-rexglue-sdk.mintlify.app/introduction) and [SDK repo](https://github.com/rexglue/rexglue-sdk) | Runtime architecture, hooks, build expectations, and SDK/source patch context | ReXGlue is still early and generates native C++ from Xbox 360 XEX code, with runtime graphics/input/filesystem layers. Keep project-side native-render work isolated from generated code and expect SDK APIs to move. |
| [Project-G1M](https://github.com/Joschuka/Project-G1M) | G1M/G1T/G1A/G2A reference behavior and Noesis-side validation | Native C++ Noesis plugin. Best treated as a reference for G1M/G1T layout and export behavior, not as the SW2E archive/modding foundation. |
| [fmt_g1m](https://github.com/Joschuka/fmt_g1m) | Historical Python reference | Deprecated by its author in favor of Project-G1M, but still useful for comparing older parsing assumptions. |
| [gust_tools](https://github.com/VitaSmith/gust_tools) and [gust_stuff](https://github.com/eArmada8/gust_stuff) | KT/Gust-family texture/container clues | Useful for G1T-style texture research, with game-specific differences expected. |
| [Cethleann](https://github.com/neptuwunium/Cethleann) | Koei Tecmo archive/model research | Broad KT reverse-engineering toolkit; useful for comparison, not a drop-in SW2E patcher. |
| [QuickBMS](https://aluigi.altervista.org/quickbms.htm) and [Xentax LINKDATA notes](https://wiki.xentax.spektr.name/index.php/Koei_Tecmo_LINKDATA_BIN_IDX) | LINKDATA family context | LINKDATA variants differ by game; use these for structure comparison only. |
| [G1T format notes](https://amicitia.miraheze.org/wiki/G1T) | Texture format orientation | Public notes describe G1T as a Koei Tecmo texture format related to DDS-style texture payloads. |

## Current Local Advantage

The local tools already cover SW2E-specific work that generic KT tools usually do not:

- `LINKDATA_BNS.IDX/LNK` cataloging and full rebuild.
- `LZP2` decode and literal encode.
- Stage offset-table split/repack.
- `G1M_0030` / `G1MG0040` scan and OBJ export.
- Conservative OBJ-to-G1M patching while preserving topology and original chunk layout.
- Runtime archive and native-render hooks for correlating files with real menus/battles.

## Public Repo Rules

Do not commit extracted game data, disc images, XEX binaries, movie files, giant runtime logs,
native event JSON dumps, or generated recomp edits. Keep small source notes, CSV summaries, scripts,
and original project artwork.

Third-party tools and plugins are not bundled here. Link them as references, check their licenses
before reusing code, and keep SW2E-specific code in this repository independently auditable.

## Next Cross-Checks

1. Compare Project-G1M output against local exports for stage `2856`, runtime stage `2943`,
   character samples `1566-1573`, and weapon/prop bundle `2410`.
2. Validate SW2E Xbox 360 `G1TG` texture extraction/repacking and connect material texture indices
   to neighboring `G1TG` containers.
3. Decode unknown stage metadata chunks in `2857` and `2944`; likely candidates are collision,
   placement, pathing, scenario, weather, or lighting data.
4. Use runtime `sw2e_archive_asset_load_wrapper` logs across multiple battles to map stage IDs to
   exact archive groups.
5. Continue PIX/DbgPrint/source-path xref work, but treat it as renderer diagnostics unless a real
   gameplay-facing debug menu becomes reachable.
6. Keep ReXGlue native-render work project-specific for now: SW2E can advance by mapping the small
   number of shader/layout families it actually uses, then feed generalized fixes back into the SDK
   only after the game-side behavior is proven.
