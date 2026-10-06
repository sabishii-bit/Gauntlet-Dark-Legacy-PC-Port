# Gauntlet Dark Legacy

A PC port of the GameCube version of *Gauntlet Dark Legacy* for Windows and
Linux. Currently in alpha.

Requires your own USA GameCube disc image (`GUNE5D`). Game assets are not included.

## Install and play

1. Download the installer for your system from [Releases](https://github.com/sabishii-bit/Gauntlet-Dark-Legacy-PC-Port/releases).
2. Run it, select or drop in your `.iso` or `.ciso`, and choose an install folder.
3. Launch `gauntlet.exe` on Windows or `gauntlet` on Linux.

Supported systems: Windows 10/11 x64 or Ubuntu 24.04+ x86-64, with a Vulkan 1.3
graphics driver. Extract the Linux `.tar.gz` before running its installer.

To upgrade, install into a new folder and copy `saves/` and `config/` from
your previous installation.

## Build from source

Requires Python 3.9+ and Git.

```sh
git clone https://github.com/sabishii-bit/Gauntlet-Dark-Legacy-PC-Port.git
cd Gauntlet-Dark-Legacy-PC-Port
python scripts/setup.py
```

Follow the [asset setup instructions](assets/README.md), then launch:

```sh
python scripts/build.py --run
```

## Tests and scenarios

```sh
python scripts/build.py --test
python -m unittest discover -s tests/scripts
python scripts/scenario.py --list
python scripts/scenario.py genie --build
```

## Report a bug

Open an [issue](https://github.com/sabishii-bit/Gauntlet-Dark-Legacy-PC-Port/issues)
with your build version, OS, and steps to reproduce it. Please do not upload
game assets.

## License

Not yet decided. The code is original; the game data belongs to its owners.
