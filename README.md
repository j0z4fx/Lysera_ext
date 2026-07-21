# Lysera_ext

Distribution repository for the Lysera external client and its self-updating loader.

## Use

1. Download `LyseraLoader.exe`.
2. Run it from any folder.
3. The loader installs under `%LOCALAPPDATA%\Lysera_data`, checks `release/version.txt`, and launches `bin/Lysera.exe` as administrator.

Runtime configuration and relationship data remain under `%LOCALAPPDATA%\Lysera_data\data\config` and `data\cache`; updates do not overwrite them. The first run of this loader also copies existing config/cache data found beside the loader.

## Publishing an update

Replace the files under `release/` (the application lives at `release/bin/Lysera.exe`) and change `release/version.txt`. Existing loader installations will update on their next launch.

The loader source and MSVC project are under `loader/`.
