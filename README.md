# Gauntlet Dark Legacy

A C++26 / Vulkan PC port of the GameCube version of *Gauntlet Dark Legacy*.
Windows and Linux are supported. This is an alpha; bugs and missing behavior
are still being worked on.

You need your own USA GameCube disc image (`GUNE5D`). No game assets are
included or downloaded.

## Install and play

1. Download the installer for your system from [Releases](https://github.com/sabishii-bit/Gauntlet-Dark-Legacy-PC-Port/releases).
2. Run it, select or drop in your `.iso` or `.ciso`, and choose an install folder.
3. Launch `gauntlet.exe` on Windows or `gauntlet` on Linux.

The installers target Windows 10/11 x64 and Ubuntu 24.04+ x86-64. A Vulkan 1.3
graphics driver is required. On Linux, extract the `.tar.gz` before running
the installer. Python and build tools are not needed to play a release.

Installed builds keep saves in `saves/` and settings in `config/settings.json`
beside the game. To upgrade, install into a new folder and copy those two
folders from your previous installation.

## Build from source

Start with Python 3.9+, Git, and a Vulkan 1.3 graphics driver. Use `python3`
instead of `python` on Linux if needed.

```sh
git clone https://github.com/sabishii-bit/Gauntlet-Dark-Legacy-PC-Port.git
cd Gauntlet-Dark-Legacy-PC-Port
python scripts/setup.py
```

The setup script checks dependencies, asks before installing missing tools,
and builds the project. Use `--check` to inspect your setup without changing it.

Extract your disc to `assets/GUNE5D` using the [asset setup instructions](assets/README.md),
then build and launch:

```sh
python scripts/build.py --run
```

The game reads the native assets directly. No conversion to JSON, PNG or WAV
is required. `--run` uses a Release build; other build commands default to
Debug. You can also name a preset, such as `windows-ninja-release` or
`linux-ninja-release`. Builds are placed under `build/<preset>/bin/`.

## Development

Run these from the repository root:

```sh
python scripts/build.py --test                 # build and run non-GPU C++ tests
python -m unittest discover -s tests/scripts   # test the Python helpers
python scripts/scenario.py --list             # list playtest scenarios
python scripts/scenario.py genie --build      # build and launch a scenario
```

Tests that need game assets skip when the files are missing. Scenarios set up
interactive playtests; they do not test player actions automatically.

For changed C++ files, run `python scripts/lint.py <path>` and
`python scripts/clangd-check.py <path>` after building. Each script supports
`--help` for more options.

## Report a bug

Open an [issue](https://github.com/sabishii-bit/Gauntlet-Dark-Legacy-PC-Port/issues)
with your build version, OS, level, character, and steps to reproduce it.
Screenshots or a short recording help. Please do not upload game assets.

## License

Not yet decided. The code is original; the game data belongs to its owners.
