# Dot-source this file to get an x64 MSVC developer environment in the current
# PowerShell session (the equivalent of a "x64 Native Tools Command Prompt").
#
#   . .\scripts\Enter-DevShell.ps1
#
# It also makes sure VCPKG_ROOT points at a usable vcpkg checkout, since
# CMakePresets.json reads it.

# Entering the Visual Studio shell replaces VCPKG_ROOT with the copy bundled in
# Visual Studio, whose tool can lag behind the baseline in vcpkg.json. A value
# set before entering the shell (machine, user or session) wins.
$userVcpkgRoot = $env:VCPKG_ROOT

$vswhere = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio\Installer\vswhere.exe'
if (-not (Test-Path $vswhere)) {
    throw "vswhere.exe not found. Install Visual Studio 2022 (or Build Tools) with the 'Desktop development with C++' workload."
}

$vsPath = & $vswhere -latest -products * `
    -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 `
    -property installationPath
if (-not $vsPath) {
    throw "No Visual Studio 2022 installation with the MSVC x64 toolset was found."
}

# vsdevcmd's extension scripts expect vswhere on PATH; without it they print a
# harmless "vswhere.exe is not recognized" line.
$env:Path = "$(Split-Path $vswhere);$env:Path"

Import-Module (Join-Path $vsPath 'Common7\Tools\Microsoft.VisualStudio.DevShell.dll')
Enter-VsDevShell -VsInstallPath $vsPath -SkipAutomaticLocation `
    -DevCmdArguments '-arch=x64 -host_arch=x64' | Out-Null

# Prefer the CMake and Ninja that ship with Visual Studio. This sidesteps two
# common PATH problems: an MSYS2/Cygwin cmake (e.g. devkitPro's) that cannot
# drive MSVC, and wrapper scripts such as pyenv-win's ninja.bat that CMake
# cannot launch as a make program.
$vsCMake = Join-Path $vsPath 'Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin'
$vsNinja = Join-Path $vsPath 'Common7\IDE\CommonExtensions\Microsoft\CMake\Ninja'
foreach ($dir in @($vsNinja, $vsCMake)) {
    if (Test-Path $dir) { $env:Path = "$dir;$env:Path" }
}
$ninja = Get-Command ninja -ErrorAction SilentlyContinue
if (-not $ninja -or $ninja.Source -notlike '*.exe') {
    $fallback = Join-Path $env:ProgramFiles 'Ninja'
    if (Test-Path (Join-Path $fallback 'ninja.exe')) {
        $env:Path = "$fallback;$env:Path"
    } else {
        throw "No usable ninja.exe found. Install the 'C++ CMake tools for Windows' VS component or Ninja itself."
    }
}

if ($userVcpkgRoot) {
    $env:VCPKG_ROOT = $userVcpkgRoot
} elseif (Test-Path 'C:\vcpkg\vcpkg.exe') {
    $env:VCPKG_ROOT = 'C:\vcpkg'
} elseif (-not $env:VCPKG_ROOT) {
    throw "VCPKG_ROOT is not set. Point it at your vcpkg checkout (https://github.com/microsoft/vcpkg)."
}

Write-Host "Developer shell ready: $vsPath"
Write-Host "  cmake: $((Get-Command cmake).Source)"
Write-Host "  ninja: $((Get-Command ninja).Source)"
Write-Host "  VCPKG_ROOT: $env:VCPKG_ROOT"
