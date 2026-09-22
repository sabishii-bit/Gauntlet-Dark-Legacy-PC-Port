#!/usr/bin/env python3
"""Get this machine ready to build: find the tools, install what is missing, then build.

    python scripts/setup.py             # check, install what is missing (asking first), build
    python scripts/setup.py --check     # only report what is present and what is missing
    python scripts/setup.py --yes       # install without asking
    python scripts/setup.py --test      # ... and run the unit tests afterwards
    python scripts/setup.py --tooling   # also want the LLVM tooling (clangd, clang-tidy, clang-format)
    python scripts/setup.py --no-build  # stop once the tools are in place

Windows: Visual Studio 2022 Build Tools with the C++ workload (which brings CMake and Ninja)
through winget, vcpkg into C:\\vcpkg. Linux: the distribution's packages through apt, dnf or
pacman (with sudo), a newer CMake or Ninja from pip when the distribution's is too old, vcpkg
into ~/vcpkg. Python 3.9+ and git are all it needs to begin with. A Vulkan 1.3 driver comes
with the graphics driver and is only checked for.
"""

import argparse
import ctypes.util
import os
import pathlib
import re
import shutil
import subprocess
import sys
from dataclasses import dataclass, field
from typing import Callable, Optional

import devenv

ROOT = devenv.ROOT
WINDOWS = devenv.WINDOWS
VCPKG_URL = "https://github.com/microsoft/vcpkg"
MIN_CMAKE = (3, 30)
MIN_GCC = 14
MIN_CLANG = 17


@dataclass
class Requirement:
    name: str
    found: Optional[str]                      # where or which, None when missing
    fix: Optional[Callable[[], None]] = None  # installs it; None when only the user can
    advice: str = ""                          # what to do when there is no fix
    optional: bool = False


@dataclass
class Report:
    items: list = field(default_factory=list)

    def add(self, item: Requirement) -> None:
        self.items.append(item)

    def missing(self) -> list:
        return [item for item in self.items if item.found is None]

    def show(self) -> None:
        width = max(len(item.name) for item in self.items)
        for item in self.items:
            state = item.found if item.found else ("missing (optional)" if item.optional else "MISSING")
            print(f"  {item.name:<{width}}  {state}")


def run(command: list, **kwargs) -> subprocess.CompletedProcess:
    print("+", " ".join(str(part) for part in command))
    return subprocess.run(command, check=True, **kwargs)


def output(command: list) -> str:
    try:
        return subprocess.run(command, capture_output=True, text=True, check=False).stdout
    except OSError:
        return ""


def version_of(command: list, pattern: str = r"(\d+)\.(\d+)(?:\.(\d+))?") -> Optional[tuple]:
    match = re.search(pattern, output(command))
    if not match:
        return None
    return tuple(int(part) for part in match.groups() if part is not None)


def program(name: str, *preferred_dirs: pathlib.Path) -> Optional[str]:
    """The program in one of `preferred_dirs` (the copies the build scripts pick first), else
    the one on PATH."""
    for directory in preferred_dirs:
        for candidate in (directory / name, directory / f"{name}.exe"):
            if candidate.exists():
                return str(candidate)
    return shutil.which(name)


# --- vcpkg, the same on every platform -----------------------------------------------------

def vcpkg_root() -> pathlib.Path:
    configured = os.environ.get("VCPKG_ROOT")
    if configured:
        return pathlib.Path(configured)
    return pathlib.Path(r"C:\vcpkg") if WINDOWS else pathlib.Path.home() / "vcpkg"


def vcpkg_baseline() -> str:
    text = (ROOT / "vcpkg.json").read_text(encoding="utf-8")
    match = re.search(r'"builtin-baseline"\s*:\s*"([0-9a-f]+)"', text)
    return match.group(1) if match else ""


def vcpkg_has_baseline(root: pathlib.Path) -> bool:
    baseline = vcpkg_baseline()
    if not baseline:
        return True
    completed = subprocess.run(["git", "-C", str(root), "cat-file", "-e", f"{baseline}^{{commit}}"],
                               capture_output=True, check=False)
    return completed.returncode == 0


def install_vcpkg() -> None:
    root = vcpkg_root()
    if not (root / ".git").exists():
        run(["git", "clone", VCPKG_URL, str(root)])
    if not vcpkg_has_baseline(root):
        run(["git", "-C", str(root), "fetch", "origin"])
    bootstrap = root / ("bootstrap-vcpkg.bat" if WINDOWS else "bootstrap-vcpkg.sh")
    run([str(bootstrap), "-disableMetrics"], cwd=root, shell=WINDOWS)


def check_vcpkg(report: Report) -> None:
    root = vcpkg_root()
    tool = root / ("vcpkg.exe" if WINDOWS else "vcpkg")
    if not tool.exists():
        report.add(Requirement("vcpkg", None, install_vcpkg))
        return
    if not vcpkg_has_baseline(root):
        report.add(Requirement("vcpkg", None, install_vcpkg,
                               advice=f"{root} predates the baseline in vcpkg.json; git fetch it"))
        return
    report.add(Requirement("vcpkg", str(root)))


def check_common(report: Report) -> None:
    version = sys.version_info
    report.add(Requirement("Python", f"{version.major}.{version.minor}" if version >= (3, 9) else None,
                           advice="Python 3.9 or newer is needed to run the scripts"))
    report.add(Requirement("git", program("git"), advice="install git and put it on PATH"))


# --- Windows ---------------------------------------------------------------------------------

def vs_installer() -> pathlib.Path:
    program_files = pathlib.Path(os.environ.get("ProgramFiles(x86)", r"C:\Program Files (x86)"))
    return program_files / "Microsoft Visual Studio" / "Installer"


def vs_query(*requires: str) -> str:
    vswhere = vs_installer() / "vswhere.exe"
    if not vswhere.exists():
        return ""
    command = [str(vswhere), "-latest", "-products", "*", "-property", "installationPath"]
    for component in requires:
        command += ["-requires", component]
    return output(command).strip()


def winget() -> Optional[str]:
    return program("winget")


def install_build_tools() -> None:
    existing = vs_query()
    workload = ["--add", "Microsoft.VisualStudio.Workload.VCTools", "--includeRecommended"]
    if existing:
        # Visual Studio is there without the C++ workload: add it in place.
        setup = vs_installer() / "setup.exe"
        run([str(setup), "modify", "--installPath", existing, "--quiet", "--wait", "--norestart",
             *workload])
        return
    if winget() is None:
        sys.exit("winget is not available; install Visual Studio 2022 Build Tools with the "
                 "'Desktop development with C++' workload from visualstudio.microsoft.com")
    override = " ".join(["--quiet", "--wait", "--norestart", *workload])
    run(["winget", "install", "--id", "Microsoft.VisualStudio.2022.BuildTools", "--exact",
         "--accept-source-agreements", "--accept-package-agreements", "--override", override])


def winget_install(package: str) -> Callable[[], None]:
    def install() -> None:
        if winget() is None:
            sys.exit(f"winget is not available; install {package} by hand")
        run(["winget", "install", "--id", package, "--exact", "--accept-source-agreements",
             "--accept-package-agreements"])
    return install


def check_windows(report: Report, tooling: bool) -> None:
    vs_path = vs_query("Microsoft.VisualStudio.Component.VC.Tools.x86.x64")
    report.add(Requirement("MSVC (VS 2022, C++ workload)", vs_path or None, install_build_tools))
    bundled = pathlib.Path(vs_path) / "Common7" / "IDE" / "CommonExtensions" / "Microsoft" / "CMake" \
        if vs_path else pathlib.Path("nowhere")
    cmake = program("cmake", bundled / "CMake" / "bin")
    cmake_version = version_of([cmake, "--version"]) if cmake else None
    report.add(Requirement(f"CMake {MIN_CMAKE[0]}.{MIN_CMAKE[1]}+",
                           cmake if cmake_version and cmake_version >= MIN_CMAKE else None,
                           winget_install("Kitware.CMake")))
    ninja = program("ninja", bundled / "Ninja",
                    pathlib.Path(os.environ.get("ProgramFiles", r"C:\Program Files")) / "Ninja")
    report.add(Requirement("Ninja", ninja if ninja and ninja.lower().endswith(".exe") else None,
                           winget_install("Ninja-build.Ninja")))
    check_vcpkg(report)
    system32 = pathlib.Path(os.environ.get("SystemRoot", r"C:\Windows")) / "System32"
    loader = system32 / "vulkan-1.dll"
    report.add(Requirement("Vulkan runtime", str(loader) if loader.exists() else None,
                           advice="install your GPU vendor's current graphics driver"))
    if tooling:
        llvm = pathlib.Path(os.environ.get("ProgramFiles", r"C:\Program Files")) / "LLVM" / "bin"
        report.add(Requirement("LLVM tooling", program("clang-tidy", llvm), winget_install("LLVM.LLVM"),
                               optional=True))


# --- Linux -----------------------------------------------------------------------------------

PACKAGES = {
    "apt-get": {
        "build": ["build-essential", "g++-14", "cmake", "ninja-build", "pkg-config", "curl", "zip",
                  "unzip", "tar", "libx11-dev", "libxrandr-dev", "libxinerama-dev", "libxcursor-dev",
                  "libxi-dev", "libxkbcommon-dev", "libwayland-dev", "libgl-dev", "libvulkan-dev",
                  "libvulkan1", "mesa-vulkan-drivers", "autoconf", "autoconf-archive",
                  "automake", "libtool"],
        "tooling": ["clangd", "clang-format", "clang-tidy"],
        "install": ["sudo", "apt-get", "install", "-y"],
        "refresh": ["sudo", "apt-get", "update"],
    },
    "dnf": {
        "build": ["gcc-c++", "cmake", "ninja-build", "pkgconf-pkg-config", "curl", "zip", "unzip",
                  "tar", "libX11-devel", "libXrandr-devel", "libXinerama-devel", "libXcursor-devel",
                  "libXi-devel", "libxkbcommon-devel", "wayland-devel", "mesa-libGL-devel",
                  "vulkan-loader", "vulkan-loader-devel", "mesa-vulkan-drivers", "autoconf",
                  "autoconf-archive", "automake", "libtool"],
        "tooling": ["clang", "clang-tools-extra"],
        "install": ["sudo", "dnf", "install", "-y"],
        "refresh": [],
    },
    "pacman": {
        "build": ["base-devel", "gcc", "cmake", "ninja", "pkgconf", "curl", "zip", "unzip", "tar",
                  "libx11", "libxrandr", "libxinerama", "libxcursor", "libxi", "libxkbcommon",
                  "wayland", "mesa", "vulkan-icd-loader", "vulkan-headers", "autoconf",
                  "autoconf-archive", "automake", "libtool"],
        "tooling": ["clang"],
        "install": ["sudo", "pacman", "-S", "--needed", "--noconfirm"],
        "refresh": ["sudo", "pacman", "-Sy"],
    },
}


def package_manager() -> Optional[str]:
    for name in PACKAGES:
        if shutil.which(name):
            return name
    return None


def package_install(names: list) -> Callable[[], None]:
    def install() -> None:
        manager = package_manager()
        if manager is None:
            sys.exit(f"No apt, dnf or pacman found; install by hand: {' '.join(names)}")
        if PACKAGES[manager]["refresh"]:
            run(PACKAGES[manager]["refresh"])
        run([*PACKAGES[manager]["install"], *names])
    return install


def missing_packages(manager: str, names: list) -> list:
    """Check development packages even when the compiler and runtime are installed."""
    missing = []
    for name in names:
        if manager == "apt-get":
            installed = output(["dpkg-query", "-W", "-f=${Status}", name]).strip() \
                == "install ok installed"
        else:
            query = ["rpm", "-q", name] if manager == "dnf" else ["pacman", "-Q", name]
            installed = subprocess.run(query, capture_output=True, check=False).returncode == 0
        if not installed:
            missing.append(name)
    return missing


def pip_install(package: str) -> Callable[[], None]:
    def install() -> None:
        run([sys.executable, "-m", "pip", "install", "--user", package])
        user_bin = pathlib.Path.home() / ".local" / "bin"
        os.environ["PATH"] = f"{user_bin}{os.pathsep}{os.environ.get('PATH', '')}"
    return install


def newest_compiler() -> Optional[str]:
    """The newest usable C++ compiler on PATH: `g++`, `g++-N` or `clang++`, or None."""
    candidates = ["g++", "clang++"] + [f"g++-{n}" for n in range(20, MIN_GCC - 1, -1)] \
        + [f"clang++-{n}" for n in range(24, MIN_CLANG - 1, -1)]
    for name in candidates:
        path = shutil.which(name)
        if path is None:
            continue
        version = version_of([path, "--version"], r"(\d+)\.(\d+)\.(\d+)")
        if version is None:
            continue
        floor = MIN_CLANG if "clang" in name else MIN_GCC
        if version[0] >= floor:
            return path
    return None


def check_linux(report: Report, tooling: bool) -> None:
    manager = package_manager()
    build_packages = PACKAGES[manager]["build"] if manager else []
    if manager:
        # A pre-provisioned runner can have Vulkan and a compiler but still lack
        # the headers or Autotools that vcpkg's window-system ports build with.
        missing = missing_packages(manager, build_packages)
        report.add(Requirement("Linux build packages", None if missing else "installed",
                               package_install(build_packages),
                               advice="missing packages: " + ", ".join(missing)))
    compiler = newest_compiler()
    report.add(Requirement(f"C++ compiler (GCC {MIN_GCC}+ or Clang {MIN_CLANG}+)", compiler,
                           package_install(build_packages) if manager else None,
                           advice="install GCC 14 or newer, or Clang 17 or newer"))
    cmake = shutil.which("cmake")
    cmake_version = version_of([cmake, "--version"]) if cmake else None
    report.add(Requirement(f"CMake {MIN_CMAKE[0]}.{MIN_CMAKE[1]}+",
                           cmake if cmake_version and cmake_version >= MIN_CMAKE else None,
                           pip_install("cmake")))
    report.add(Requirement("Ninja", shutil.which("ninja"), pip_install("ninja")))
    report.add(Requirement("pkg-config", shutil.which("pkg-config") or shutil.which("pkgconf"),
                           package_install(build_packages) if manager else None))
    for name in ("curl", "zip", "unzip", "tar"):
        report.add(Requirement(name, shutil.which(name),
                               package_install(build_packages) if manager else None))
    check_vcpkg(report)
    vulkan = ctypes.util.find_library("vulkan")
    report.add(Requirement("Vulkan loader", vulkan,
                           package_install(build_packages) if manager else None,
                           advice="install the Vulkan loader and a Vulkan 1.3 driver (Mesa 22+)"))
    if tooling:
        report.add(Requirement("LLVM tooling", shutil.which("clang-tidy"),
                               package_install(PACKAGES[manager]["tooling"]) if manager else None,
                               optional=True))


# --- putting it together -----------------------------------------------------------------------

def gather(tooling: bool) -> Report:
    report = Report()
    check_common(report)
    if WINDOWS:
        check_windows(report, tooling)
    else:
        check_linux(report, tooling)
    return report


def confirm(question: str, yes: bool) -> bool:
    if yes:
        return True
    answer = input(f"{question} [y/N] ").strip().lower()
    return answer in ("y", "yes")


def fix_key(fix: Callable[[], None]) -> tuple:
    """Identifies a fix by what it does, so one apt run covers every package it installs."""
    cells = getattr(fix, "__closure__", None) or ()
    return (getattr(fix, "__qualname__", repr(fix)),
            tuple(str(cell.cell_contents) for cell in cells))


def install_missing(report: Report, yes: bool) -> bool:
    """Runs each missing item's fix (once per distinct fix); false when something still lacks."""
    done = set()
    for item in report.missing():
        if item.fix is None:
            print(f"  {item.name}: {item.advice or 'no automatic install'}")
            continue
        key = fix_key(item.fix)
        if key in done:
            continue
        if not confirm(f"Install {item.name}?", yes):
            continue
        item.fix()
        done.add(key)
    after = gather(tooling=any(item.optional for item in report.items))
    print("\nAfter installing:")
    after.show()
    return not [item for item in after.missing() if not item.optional]


def build(test: bool) -> None:
    """Configures and builds the platform's Debug preset, with the newest compiler on Linux."""
    if not WINDOWS:
        compiler = newest_compiler()
        if compiler and "CXX" not in os.environ:
            os.environ["CXX"] = compiler
    preset = devenv.default_preset()
    devenv.run(["cmake", "--preset", preset])
    devenv.run(["cmake", "--build", "--preset", preset])
    if test:
        devenv.run(["ctest", "--preset", preset, "-LE", "gpu"])


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    parser.add_argument("--check", action="store_true", help="report only; install nothing")
    parser.add_argument("--yes", action="store_true", help="install without asking")
    parser.add_argument("--tooling", action="store_true", help="also want the LLVM editor tooling")
    parser.add_argument("--no-build", action="store_true", help="stop once the tools are in place")
    parser.add_argument("--test", action="store_true", help="run the unit tests after building")
    args = parser.parse_args()

    report = gather(args.tooling)
    print(f"Tools for building under {ROOT} ({'Windows' if WINDOWS else 'Linux'}):")
    report.show()
    required_missing = [item for item in report.missing() if not item.optional]
    if args.check:
        return 1 if required_missing else 0
    if report.missing():
        print()
        if not install_missing(report, args.yes):
            print("\nSomething is still missing; see the advice above, then run this again.")
            return 1
    if args.no_build:
        return 0
    print("\nConfiguring and building:")
    build(args.test)
    exe = ROOT / "build" / devenv.default_preset() / "bin" / ("gauntlet.exe" if WINDOWS else "gauntlet")
    print(f"\nBuilt {exe}\nNext: put the game data under assets/ (see assets/README.md), then\n"
          "  python scripts/build.py --unpack --levels\n  python scripts/build.py --run")
    return 0


if __name__ == "__main__":
    sys.exit(main())
