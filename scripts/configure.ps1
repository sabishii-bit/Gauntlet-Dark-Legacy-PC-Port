# Configures the project with a CMake preset from inside a developer shell.
#
#   .\scripts\configure.ps1                      # windows-ninja-debug
#   .\scripts\configure.ps1 -Preset windows-vs2022
#
# The first run lets vcpkg build the dependencies (a few minutes).

[CmdletBinding()]
param(
    [string]$Preset = 'windows-ninja-debug'
)

$ErrorActionPreference = 'Stop'
. (Join-Path $PSScriptRoot 'Enter-DevShell.ps1')

Push-Location (Join-Path $PSScriptRoot '..')
try {
    cmake --preset $Preset
    if ($LASTEXITCODE -ne 0) { throw "cmake configure failed with exit code $LASTEXITCODE" }
} finally {
    Pop-Location
}
