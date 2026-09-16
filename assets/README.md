# Game assets

Game data is not part of this repository. Extract the contents of your own
*Gauntlet Dark Legacy* GameCube disc (`GUNE5D`) into this folder so that the
layout is:

```
assets/
  GUNE5D/
    Gauntlet/        <- the asset tree the engine reads (STATIC/, LEVELS/, PLAYERS/, ...)
    carddemo/
    opening.bnr
    sys/
```

Dolphin can do the extraction: right-click the game, *Properties*,
*Filesystem*, then *Extract Entire Disc*.

The build points the executable at `assets/GUNE5D/Gauntlet` by default
(`GDL_ASSET_DIR` in CMake); `--assets <dir>` overrides it at run time.
Everything under `assets/` except this file is ignored by git.
