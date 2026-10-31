[CmdletBinding()]
param(
    [string]$BuildDirectory = 'build',
    [string]$OutputDirectory = 'dist'
)
$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest
$projectRoot = Split-Path $PSScriptRoot -Parent
function Resolve-ProjectPath([string]$Path) {
    if ([IO.Path]::IsPathRooted($Path)) { return [IO.Path]::GetFullPath($Path) }
    return [IO.Path]::GetFullPath((Join-Path $projectRoot $Path))
}
$buildPath = Resolve-ProjectPath $BuildDirectory
$outputPath = Resolve-ProjectPath $OutputDirectory
$cmake = (Get-Command cmake -CommandType Application -ErrorAction Stop).Source
$ctest = Join-Path (Split-Path $cmake -Parent) 'ctest.exe'
$vswhere = Join-Path ([Environment]::GetFolderPath('ProgramFilesX86')) 'Microsoft Visual Studio\Installer\vswhere.exe'
if (-not (Test-Path -LiteralPath $vswhere)) { throw 'Visual Studio Installer / vswhere was not found.' }
$vsPath = & $vswhere -latest -version '[17.0,18.0)' -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
if (-not $vsPath) { throw 'Install Visual Studio 2022 C++ desktop tools and a Windows SDK.' }
& $cmake -S $projectRoot -B $buildPath -G 'Visual Studio 17 2022' -A x64 "-DCMAKE_GENERATOR_INSTANCE=$vsPath" "-DPBC_OUTPUT_DIR=$outputPath" "-DPBC_TEST_DATA_DIR=$buildPath/ui-regression/data"
if ($LASTEXITCODE -ne 0) { throw 'CMake configuration failed.' }
& $cmake --build $buildPath --config Release
if ($LASTEXITCODE -ne 0) { throw 'Build failed.' }
& $ctest --test-dir $buildPath -C Release --output-on-failure
if ($LASTEXITCODE -ne 0) { throw 'Tests failed.' }
Write-Host "Executable: $(Join-Path $outputPath 'PeripheralBattery.exe')"
