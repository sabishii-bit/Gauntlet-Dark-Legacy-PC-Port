"""Build asset-free alpha installers on the target OS, or verify a release tag.

Maintainer workflow (from a clean, committed checkout):
  python scripts/setup.py --yes --no-build
  python -m venv build/installer-venv
  <venv-python> -m pip install -r scripts/installer/requirements.txt
  <venv-python> scripts/release.py build

Bump VERSION, commit, and push v<VERSION> to publish a GitHub prerelease. A manual
Release workflow run builds downloadable artifacts without publishing a release.
No disc image, native assets, extracted media, saves or personal settings are shipped.
Linux targets x86-64 Ubuntu 24.04+ desktops; Vulkan 1.3 drivers remain prerequisites.
Windows targets x64 Windows 10/11. Installers are unsigned until signing is configured.
The wizard installs into a fresh directory; upgrading means a new install and copying
the old saves/ and config/ folders afterwards. It never deletes an older installation.
"""

import argparse
import hashlib
import importlib.metadata
import json
import os
from pathlib import Path
import platform
import re
import shutil
import subprocess
import sys
import tarfile
import urllib.request
import zipfile

import devenv
import package
from installer.install import payload_inventory, temporary_directory

ROOT = devenv.ROOT
SEMVER = re.compile(r"(?:0|[1-9][0-9]*)\.(?:0|[1-9][0-9]*)\.(?:0|[1-9][0-9]*)-alpha\.[1-9][0-9]*")


def version(root=ROOT):
    value = (root / "VERSION").read_text(encoding="utf-8").strip()
    if not SEMVER.fullmatch(value):
        raise ValueError("VERSION must be an alpha semantic version, e.g. 0.1.0-alpha.1")
    return value


def validate_tag(tag, root=ROOT):
    expected = "v" + version(root)
    if tag != expected:
        raise ValueError(f"Tag {tag!r} must equal {expected!r}")
    return expected


def digest(path):
    result = hashlib.sha256()
    with path.open("rb") as stream:
        while block := stream.read(1024 * 1024):
            result.update(block)
    return result.hexdigest()


def freezer_environment():
    """Do not harvest unrelated Qt/ICU/SSL DLLs from a developer's tool PATH.

    Qt on Windows uses the OS ICU API; a Poppler/Conda icuuc.dll with versioned
    exports has the same filename but cannot satisfy that ABI. PyInstaller's
    dependency search must see the OS libraries, not those unrelated tool kits.
    """
    environment = dict(os.environ)
    for key in ("PYTHONPATH", "PYTHONHOME", "QT_PLUGIN_PATH", "QML2_IMPORT_PATH"):
        environment.pop(key, None)
    environment["PYTHONNOUSERSITE"] = "1"
    paths = [str(Path(sys.executable).parent), sys.base_prefix]
    if devenv.WINDOWS:
        windows = Path(os.environ["SystemRoot"])
        paths.extend((str(windows / "System32"), str(windows)))
    else:
        paths.extend(("/usr/bin", "/bin"))
        # setup-python's interpreter itself can depend on libpython in this folder.
        environment["LD_LIBRARY_PATH"] = str(Path(sys.base_prefix) / "lib")
    environment["PATH"] = os.pathsep.join(paths)
    return environment


def make_payload(output, files, release_version, commit, system, executable):
    """Archive only explicit runtime files, then validate with the installer's own policy."""
    names = [name for _, name in files]
    if len(set(name.casefold() for name in names)) != len(names):
        raise ValueError("Duplicate release destination")
    if not SEMVER.fullmatch(release_version):
        raise ValueError("Invalid release version")
    hashes = {name: digest(source) for source, name in files}
    metadata = {"version": release_version, "commit": commit, "platform": system,
                "executable": executable, "files": hashes}
    with zipfile.ZipFile(output, "x", compression=zipfile.ZIP_DEFLATED, compresslevel=6) as archive:
        for source, name in sorted(files, key=lambda entry: entry[1]):
            archive.write(source, name)
        archive.writestr("build-info.json", json.dumps(metadata, indent=2) + "\n")
    payload_inventory(output)
    return metadata


def upstream_license(staging, project, release_version, name):
    """Obtain a verbatim notice from the dependency's exact source release."""
    repository = {"Qt": "qt/qtbase", "QtForPython": "pyside/pyside-setup"}[project]
    url = f"https://raw.githubusercontent.com/{repository}/v{release_version}/LICENSES/{name}"
    with urllib.request.urlopen(url, timeout=60) as response:
        content = response.read(128 * 1024)
    if len(content) < 80 or len(content) == 128 * 1024 or b"<html" in content.lower():
        raise ValueError(f"Invalid license download: {url}")
    target = staging / f"{project}-{name}"
    target.write_bytes(content)
    return target, f"licenses/installer/{project}/{name}"


def installer_notices(staging):
    files = []
    for name in ("PySide6-Essentials", "shiboken6", "PyInstaller"):
        distribution = importlib.metadata.distribution(name)
        found = [path for path in distribution.files or ()
                 if any(word in str(path).lower() for word in ("license", "copying"))
                 and distribution.locate_file(path).is_file()]
        # Qt for Python's Linux wheels omit these records altogether. Its exact
        # source-release notices below are mandatory on every platform instead.
        if not found and name == "PyInstaller":
            raise ValueError(f"Missing license files for {name}")
        for index, path in enumerate(found):
            files.append((Path(distribution.locate_file(path)),
                          f"licenses/installer/{name}/{index}-{path.name}"))
    qt_version = importlib.metadata.version("PySide6-Essentials")
    if importlib.metadata.version("shiboken6") != qt_version:
        raise ValueError("PySide and Shiboken versions differ")
    for name in ("Apache-2.0.txt", "BSD-3-Clause.txt", "GFDL-1.3-no-invariants-only.txt",
                 "GPL-2.0-only.txt", "GPL-3.0-only.txt", "LGPL-3.0-only.txt",
                 "LicenseRef-Qt-Commercial.txt", "Qt-GPL-exception-1.0.txt"):
        files.append(upstream_license(staging, "QtForPython", qt_version, name))
    for name in ("LGPL-3.0-only.txt", "GPL-3.0-only.txt"):
        files.append(upstream_license(staging, "Qt", qt_version, name))
    return files


def licenses(binary, staging):
    """Carry the installed dependency copyright/license notices, not generated asset data."""
    files = []
    if (ROOT / "LICENSE").is_file():
        files.append((ROOT / "LICENSE", "licenses/project.txt"))
    else:
        # Preserve the repository's undecided licensing; do not invent an open-source grant.
        project_notice = staging / "PROJECT.txt"
        project_notice.write_text("Project code license: not yet decided (see repository README).\n"
                                  "Game data belongs to its respective owners and is not distributed.\n",
                                  encoding="utf-8")
        files.append((project_notice, "licenses/project.txt"))
    triplet = "x64-windows" if devenv.WINDOWS else "x64-linux"
    share = binary / "vcpkg_installed" / triplet / "share"
    copyrights = sorted(share.glob("*/copyright"))
    if not copyrights:
        raise ValueError(f"Missing vcpkg license inventory: {share}")
    files.extend((path, f"licenses/vcpkg/{path.parent.name}.txt") for path in copyrights)
    files.extend(installer_notices(staging))
    python_notices = [Path(sys.base_prefix) / "LICENSE.txt",
                      Path(f"/usr/share/doc/python{sys.version_info.major}.{sys.version_info.minor}/copyright")]
    python_notice = next((path for path in python_notices if path.is_file()), None)
    if python_notice is None:
        # setup-python's Linux distributions keep the license under the standard library.
        import sysconfig
        python_notice = Path(sysconfig.get_path("stdlib")) / "LICENSE.txt"
    if not python_notice.is_file():
        raise ValueError("Missing Python runtime license")
    files.append((python_notice, "licenses/installer/Python.txt"))
    notice = staging / "THIRD-PARTY.txt"
    notice.write_text(
        "Installer: Qt/PySide6 and Shiboken are used under LGPLv3; PyInstaller uses its "
        "GPL exception for frozen applications. Corresponding notices accompany this build.\n"
        "The installer source and build recipe are scripts/installer, scripts/install_game.py "
        "and scripts/release.py in this release's source archive. They permit rebuilding "
        "with modified Qt/PySide libraries; no signature or activation check restricts this.\n"
        "Qt sources: https://download.qt.io/official_releases/qt/\n"
        "PySide/Shiboken sources: https://download.qt.io/official_releases/QtForPython/\n"
        "PyInstaller sources: https://github.com/pyinstaller/pyinstaller\n"
        "Game dependencies: see licenses/vcpkg. Windows includes the Microsoft Visual C++ "
        "redistributable CRT, not a system DLL harvest. No Microsoft development tools are included.\n"
        "Linux includes libstdc++/libgcc under GPLv3 plus the GCC Runtime Library Exception; "
        "see licenses/gcc. System graphics, audio and windowing libraries are not bundled.\n"
        "Game assets are supplied solely by the player and remain unchanged.\n", encoding="utf-8")
    files.append((notice, "licenses/THIRD-PARTY.txt"))
    return files


def build_release(args):
    if platform.machine().lower() not in ("amd64", "x86_64") or sys.platform not in ("win32", "linux"):
        raise ValueError("Build on x86-64 Windows or Linux; cross-compilation is not supported")
    release_version = version()
    commit = subprocess.check_output(["git", "rev-parse", "HEAD"], cwd=ROOT, text=True).strip()
    dirty = subprocess.check_output(["git", "status", "--porcelain"], cwd=ROOT, text=True).strip()
    if dirty and not args.allow_dirty:
        raise ValueError("Release builds require a clean checkout (local testing: --allow-dirty)")
    if dirty:
        commit += "-dirty"
    system = "windows-x64" if devenv.WINDOWS else "linux-x64"
    name = f"GauntletDarkLegacy-{release_version}-{system}-setup"
    output = args.output.resolve()
    output.mkdir(parents=True, exist_ok=True)
    final_name = name + (".exe" if devenv.WINDOWS else ".tar.gz")
    if (output / final_name).exists():
        raise ValueError(f"Release artifact already exists: {output / final_name}")
    # Separate build cache: no disc icon, retail inputs or developer-specific fallback paths.
    binary = ROOT / "build" / f"release-{system}"
    configure = ["cmake", "--preset", devenv.release_preset(), "-B", str(binary),
                 "-DGDL_EMBED_DISC_ICON=OFF", "-DGDL_ENABLE_VULKAN_VALIDATION=OFF",
                 "-DGDL_ASSET_DIR=", "-DGDL_UNPACKED_DIR=", f"-DGDL_DATA_DIR={ROOT / 'data'}",
                 "-DGDL_TEST_DISCOVERY_FILTER=~[gpu]~[assets]~[unpacked]"]
    if not devenv.WINDOWS:
        configure.append("-DCMAKE_BUILD_RPATH_USE_ORIGIN=ON")
    devenv.run(configure)
    devenv.run(["cmake", "--build", str(binary), "--target", "gauntlet", "tests"])
    # Unit tests deliberately exclude the private asset tier; no retail data is on the runner.
    devenv.run(["ctest", "--test-dir", str(binary), "-LE", "gpu|assets|unpacked",
                "--output-on-failure"])
    executable = binary / "bin" / ("gauntlet.exe" if devenv.WINDOWS else "gauntlet")
    reported = subprocess.check_output([str(executable), "--version"], text=True).strip()
    if reported != release_version:
        raise ValueError(f"Executable version {reported!r} differs from VERSION")
    with temporary_directory(ROOT / "build", "gdl-release-") as temporary:
        staging = Path(temporary)
        runtimes = (package.msvc_runtime_directory(),) if devenv.WINDOWS else ()
        plan = package.make_plan(executable, None, ROOT / "data", staging / "unused", runtimes)
        # Public packages use the checked-in defaults only, never local debug/test data.
        tracked_data = set(subprocess.check_output(
            ["git", "ls-files", "data"], cwd=ROOT, text=True).splitlines())
        files = [(source, relative.as_posix()) for source, relative in plan.files
                 if relative.parts[0] != "data" or relative.as_posix() in tracked_data]
        # The data tree is configuration/text, not an alternative path for game media.
        if any(relative.startswith("data/") and not relative.endswith(".json") for _, relative in files):
            raise ValueError("Unexpected public data input; review the release allowlist")
        files.extend(licenses(binary, staging))
        if not devenv.WINDOWS:
            for library in ("libstdc++.so.6", "libgcc_s.so.1"):
                compiler = devenv.environment().get("CXX", "g++")
                path = Path(subprocess.check_output([compiler, f"-print-file-name={library}"], text=True).strip())
                if not path.is_absolute() or not path.is_file():
                    raise ValueError(f"Missing compiler runtime: {library}")
                files.append((path.resolve(), f"lib/{library}"))
            notices = sorted(Path("/usr/share/doc").glob("gcc-*-base/copyright"))
            if not notices:
                raise ValueError("Missing GCC runtime license notices")
            files.extend((path, f"licenses/gcc/{path.parent.name}.txt") for path in notices)
        portable = staging / "portable.flag"
        portable.write_text("Use config/ and saves/ beside the game executable.\n", encoding="utf-8")
        files.extend(((ROOT / "VERSION", "VERSION"), (portable, "portable.flag")))
        payload = staging / "runtime.zip"
        make_payload(payload, files, release_version, commit, system, executable.name)
        freezer = [sys.executable, "-m", "PyInstaller", "--noconfirm", "--clean", "--onefile", "--noupx",
                   "--windowed", "--name", name, "--distpath", str(staging / "dist"),
                   "--workpath", str(staging / "work"), "--specpath", str(staging),
                   "--paths", str(ROOT / "scripts"),
                   "--add-data", f"{payload}{os.pathsep}.",
                   "--add-data", f"{ROOT / 'scripts/installer/strings.json'}{os.pathsep}installer",
                   str(ROOT / "scripts/install_game.py")]
        subprocess.run(freezer, cwd=ROOT, env=freezer_environment(), check=True)
        frozen = staging / "dist" / (name + (".exe" if devenv.WINDOWS else ""))
        for check in ("--check-payload", "--check-wizard"):
            diagnostic = staging / f"{check[2:]}.log"
            try:
                subprocess.run([str(frozen), check, "--diagnostic-log", str(diagnostic)],
                               env=freezer_environment(), check=True, timeout=120)
            except subprocess.CalledProcessError as error:
                detail = diagnostic.read_text(encoding="utf-8") if diagnostic.exists() else "No Python traceback"
                raise RuntimeError(f"Frozen installer failed {check}:\n{detail}") from error
        if devenv.WINDOWS:
            shutil.copyfile(frozen, output / final_name)
        else:
            with tarfile.open(output / final_name, "w:gz") as archive:
                archive.add(frozen, arcname=name)
        # Notices can also be read before installing (and without extracting the executable).
        with zipfile.ZipFile(output / f"{name}-licenses.zip", "x", zipfile.ZIP_DEFLATED) as archive:
            for source, relative in files:
                if relative.startswith("licenses/"):
                    archive.write(source, relative)
        artifacts = (output / final_name, output / f"{name}-licenses.zip")
        (output / f"{name}.sha256").write_text(
            "".join(f"{digest(path)}  {path.name}\n" for path in artifacts), encoding="ascii")
    print(f"Ready: {output / final_name}")


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    commands = parser.add_subparsers(dest="command", required=True)
    validate = commands.add_parser("check-tag")
    validate.add_argument("tag")
    commands.add_parser("check-installer-tools", help="validate dependency notices before compiling the game")
    build = commands.add_parser("build")
    build.add_argument("--allow-dirty", action="store_true", help="local QA only; marks receipt dirty")
    build.add_argument("--output", type=Path, default=ROOT / "out/releases")
    args = parser.parse_args()
    if args.command == "check-tag":
        print(validate_tag(args.tag))
    elif args.command == "check-installer-tools":
        (ROOT / "build").mkdir(exist_ok=True)
        with temporary_directory(ROOT / "build", "installer-tools-") as staging:
            print(f"Verified {len(installer_notices(staging))} installer dependency notices")
    else:
        build_release(args)


if __name__ == "__main__":
    main()
