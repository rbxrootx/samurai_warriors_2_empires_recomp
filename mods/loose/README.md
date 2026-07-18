# Loose Mod Overlay

Place files here using the same paths they have under `game_files`.

Example:

```text
mods\loose\data\...
```

When `run_recomp.bat` sees this folder, it launches with `--mod_data_root` so files here override
the extracted game files. Anything missing here falls back to `L:\SM2\game_files`.

Most gameplay assets are currently packed inside `data\LINKDATA_BNS.LNK`, so replacing individual
characters, weapons, maps, and tables still needs archive-entry extraction/repacking. See
`docs\modding_support.md` for the current file-format map.
