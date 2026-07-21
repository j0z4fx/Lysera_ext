# Lysera_ext

Distribution repository for the Lysera external client and its self-updating loader.

## Use

1. Download `LyseraLoader.exe`.
2. Keep it in its own writable folder.
3. Run it. The loader checks `release/version.txt`, downloads changed runtime files, and launches `Lysera.exe` as administrator.

Runtime configuration and relationship data remain local under `data/config` and `data/cache`; updates do not overwrite them.

## Publishing an update

Replace the files under `release/` and change `release/version.txt`. Existing loader installations will update on their next launch.

The loader source and MSVC project are under `loader/`.
