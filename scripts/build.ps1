# Builds (and optionally tests or runs) the project with a CMake build preset.
#
#   .\scripts\build.ps1                                   # windows-ninja-debug
#   .\scripts\build.ps1 -Test                             # build, then run the unit tests
#   .\scripts\build.ps1 -Run -AppArgs '--no-vsync'        # build, then launch gauntlet.exe
#
# Build presets are listed in CMakePresets.json.

[CmdletBinding()]
param(
    [string]$Preset = 'windows-ninja-debug',
    [switch]$Test,
    [switch]$Run,
    [string[]]$AppArgs = @()
)

$ErrorActionPreference = 'Stop'
. (Join-Path $PSScriptRoot 'Enter-DevShell.ps1')

$root = Join-Path $PSScriptRoot '..'
Push-Location $root
try {
    $presets = Get-Content (Join-Path $root 'CMakePresets.json') -Raw | ConvertFrom-Json
    $buildPreset = $presets.buildPresets | Where-Object { $_.name -eq $Preset }
    if (-not $buildPreset) { throw "Unknown build preset '$Preset'." }
    $binaryDir = Join-Path $root "build\$($buildPreset.configurePreset)"

    if (-not (Test-Path (Join-Path $binaryDir 'CMakeCache.txt'))) {
        cmake --preset $buildPreset.configurePreset
        if ($LASTEXITCODE -ne 0) { throw "cmake configure failed with exit code $LASTEXITCODE" }
    }

    cmake --build --preset $Preset
    if ($LASTEXITCODE -ne 0) { throw "build failed with exit code $LASTEXITCODE" }

    if ($Test) {
        ctest --preset $Preset -LE gpu
        if ($LASTEXITCODE -ne 0) { throw "tests failed with exit code $LASTEXITCODE" }
    }

    if ($Run) {
        $exe = Join-Path $binaryDir 'bin\gauntlet.exe'
        & $exe @AppArgs
    }
} finally {
    Pop-Location
}
