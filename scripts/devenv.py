#!/usr/bin/env python3
"""The environment the build tools run in, the same on every platform.

On Windows this is an x64 MSVC developer environment (what an "x64 Native Tools
Command Prompt" gives you) with the Visual Studio copies of CMake and Ninja first
on PATH; on Linux it is the current environment. On both, VCPKG_ROOT is filled in
from the usual checkout location when it is not set.

    python scripts/devenv.py            # print the resolved tool locations
    python scripts/devenv.py --shell    # open an interactive shell in that environment
    python scripts/devenv.py -- cmake --version   # run one command in it

Other scripts import this module and call environment().
"""

import argparse
import os
import pathlib
import shutil
import subprocess
import sys

ROOT = pathlib.Path(__file__).resolve().parents[1]
WINDOWS = sys.platform == "win32"


def default_preset() -> str:
    """The Debug Ninja preset for this platform (configure, build and test presets share it)."""
    return "windows-ninja-debug" if WINDOWS else "linux-ninja-debug"


def _vswhere() -> pathlib.Path:
    program_files = os.environ.get("ProgramFiles(x86)", r"C:\Program Files (x86)")
    vswhere = pathlib.Path(program_files) / "Microsoft Visual Studio" / "Installer" / "vswhere.exe"
    if not vswhere.exists():
        sys.exit("vswhere.exe not found. Install Visual Studio 2022 (or Build Tools) with the "
                 "'Desktop development with C++' workload.")
    return vswhere


def _visual_studio_path(vswhere: pathlib.Path) -> pathlib.Path:
    completed = subprocess.run(
        [str(vswhere), "-latest", "-products", "*", "-requires",
         "Microsoft.VisualStudio.Component.VC.Tools.x86.x64", "-property", "installationPath"],
        capture_output=True, text=True, check=False)
    path = completed.stdout.strip()
    if not path:
        sys.exit("No Visual Studio 2022 installation with the MSVC x64 toolset was found.")
    return pathlib.Path(path)


def _developer_environment(vs_path: pathlib.Path, vswhere: pathlib.Path) -> dict[str, str]:
    """Runs VsDevCmd.bat and captures the variables it exports."""
    vsdevcmd = vs_path / "Common7" / "Tools" / "VsDevCmd.bat"
    if not vsdevcmd.exists():
        sys.exit(f"VsDevCmd.bat not found under {vs_path}")
    # VsDevCmd's extension scripts expect vswhere on PATH.
    seed = dict(os.environ)
    seed["PATH"] = f"{vswhere.parent}{os.pathsep}{seed.get('PATH', '')}"
    marker = "__GDL_ENV__"
    # One string so Windows gets the command line verbatim; /s strips the outer quotes.
    command = f'cmd.exe /d /s /c ""{vsdevcmd}" -arch=x64 -host_arch=x64 -no_logo && echo {marker} && set"'
    completed = subprocess.run(command, capture_output=True, text=True, env=seed, check=False)
    output = completed.stdout
    if completed.returncode != 0 or marker not in output:
        sys.exit(f"VsDevCmd.bat failed:\n{output}\n{completed.stderr}")
    env: dict[str, str] = {}
    for line in output.split(marker, 1)[1].splitlines():
        key, separator, value = line.partition("=")
        if separator and key:
            env[key] = value
    return env


def _prepend_path(env: dict[str, str], directory: pathlib.Path) -> None:
    if directory.is_dir():
        env["PATH"] = f"{directory}{os.pathsep}{env.get('PATH', '')}"


def _windows_environment() -> dict[str, str]:
    # The developer shell replaces VCPKG_ROOT with the copy bundled in Visual Studio,
    # whose tool can lag behind the baseline in vcpkg.json; the caller's value wins.
    user_vcpkg_root = os.environ.get("VCPKG_ROOT")
    vswhere = _vswhere()
    vs_path = _visual_studio_path(vswhere)
    env = _developer_environment(vs_path, vswhere)

    # Prefer the CMake and Ninja that ship with Visual Studio over an MSYS2/Cygwin cmake that
    # cannot drive MSVC, or a ninja.bat shim that CMake cannot launch as a make program.
    cmake_tools = vs_path / "Common7" / "IDE" / "CommonExtensions" / "Microsoft" / "CMake"
    _prepend_path(env, cmake_tools / "CMake" / "bin")
    _prepend_path(env, cmake_tools / "Ninja")
    ninja = shutil.which("ninja", path=env.get("PATH"))
    if ninja is None or not ninja.lower().endswith(".exe"):
        fallback = pathlib.Path(os.environ.get("ProgramFiles", r"C:\Program Files")) / "Ninja"
        if (fallback / "ninja.exe").exists():
            _prepend_path(env, fallback)
        else:
            sys.exit("No usable ninja.exe found. Install the 'C++ CMake tools for Windows' VS "
                     "component or Ninja itself.")

    if user_vcpkg_root:
        env["VCPKG_ROOT"] = user_vcpkg_root
    elif pathlib.Path(r"C:\vcpkg\vcpkg.exe").exists():
        env["VCPKG_ROOT"] = r"C:\vcpkg"
    elif not env.get("VCPKG_ROOT"):
        sys.exit("VCPKG_ROOT is not set. Point it at your vcpkg checkout "
                 "(https://github.com/microsoft/vcpkg).")
    return env


def _posix_environment() -> dict[str, str]:
    env = dict(os.environ)
    if not env.get("VCPKG_ROOT"):
        home_vcpkg = pathlib.Path.home() / "vcpkg"
        if (home_vcpkg / "vcpkg").exists():
            env["VCPKG_ROOT"] = str(home_vcpkg)
        else:
            sys.exit("VCPKG_ROOT is not set. Point it at your vcpkg checkout "
                     "(https://github.com/microsoft/vcpkg).")
    return env


_cached = None


def environment() -> dict[str, str]:
    """The environment to run cmake, ctest and the build tools in (computed once)."""
    global _cached  # noqa: PLW0603 - one process-wide cache
    if _cached is None:
        _cached = _windows_environment() if WINDOWS else _posix_environment()
    return _cached


def resolve(command: list[str]) -> list[str]:
    """Replaces the program name with its full path in the developer environment.

    Process creation searches the parent's PATH, not the child environment's, so without this
    a different cmake or ctest on the caller's PATH would run with the developer settings.
    """
    executable = shutil.which(command[0], path=environment().get("PATH"))
    if executable is None:
        sys.exit(f"{command[0]} not found in the developer environment")
    return [executable, *command[1:]]


def run(command: list[str], **kwargs) -> subprocess.CompletedProcess:
    """Runs a command in the developer environment from the repository root; exits on failure."""
    completed = subprocess.run(resolve(command), cwd=kwargs.pop("cwd", ROOT), env=environment(),
                               check=False, **kwargs)
    if completed.returncode != 0:
        sys.exit(f"{command[0]} failed with exit code {completed.returncode}")
    return completed


def describe() -> str:
    env = environment()
    path = env.get("PATH")
    lines = [f"cmake:      {shutil.which('cmake', path=path)}",
             f"ninja:      {shutil.which('ninja', path=path)}",
             f"VCPKG_ROOT: {env.get('VCPKG_ROOT')}"]
    if WINDOWS:
        lines.insert(0, f"MSVC:       {shutil.which('cl', path=path)}")
    return "\n".join(lines)


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    parser.add_argument("--shell", action="store_true",
                        help="open an interactive shell in the developer environment")
    parser.add_argument("command", nargs="*", help="a command to run in the environment")
    args = parser.parse_args()

    if args.shell:
        print(describe())
        shell = [os.environ.get("COMSPEC", "cmd.exe")] if WINDOWS else [os.environ.get("SHELL", "/bin/sh")]
        return subprocess.run(resolve(shell), cwd=ROOT, env=environment(), check=False).returncode
    if args.command:
        return subprocess.run(resolve(args.command), cwd=ROOT, env=environment(),
                              check=False).returncode
    print(describe())
    return 0


if __name__ == "__main__":
    sys.exit(main())
