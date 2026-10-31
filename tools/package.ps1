[CmdletBinding()]
param(
    [string]$Destination = 'releases',
    [string]$ExpectedTag = ''
)
$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest
$projectRoot = Split-Path $PSScriptRoot -Parent
$cmakeText = [IO.File]::ReadAllText((Join-Path $projectRoot 'CMakeLists.txt'))
$versionMatch = [regex]::Match($cmakeText, 'project\(PeripheralBattery VERSION ([0-9]+\.[0-9]+\.[0-9]+)')
if (-not $versionMatch.Success) { throw 'Project version was not found in CMakeLists.txt.' }
$version = $versionMatch.Groups[1].Value
if ($ExpectedTag -and $ExpectedTag -ne ('v' + $version)) { throw "Tag $ExpectedTag does not match version v$version." }
$changelog = [IO.File]::ReadAllText((Join-Path $projectRoot 'CHANGELOG.md'))
$sectionPattern = '(?ms)^## \[' + [regex]::Escape($version) + '\][^\r\n]*\r?\n(.*?)(?=^## |\z)'
$section = [regex]::Match($changelog, $sectionPattern)
if (-not $section.Success) { throw "CHANGELOG.md has no entry for $version." }

# A separate output avoids touching a running portable app or its data directory.
$buildPath = Join-Path $projectRoot 'build/package'
$binaryPath = Join-Path $buildPath 'bin'
& (Join-Path $PSScriptRoot 'build.ps1') -BuildDirectory $buildPath -OutputDirectory $binaryPath
$exe = Join-Path $binaryPath 'PeripheralBattery.exe'
$fileVersion = [Diagnostics.FileVersionInfo]::GetVersionInfo($exe).ProductVersion
if ($fileVersion -ne $version) { throw "EXE version $fileVersion does not match project version $version. Update assets/app.rc." }

if ([IO.Path]::IsPathRooted($Destination)) { $destinationPath = [IO.Path]::GetFullPath($Destination) }
else { $destinationPath = [IO.Path]::GetFullPath((Join-Path $projectRoot $Destination)) }
[IO.Directory]::CreateDirectory($destinationPath) | Out-Null
$zipName = 'PeripheralBattery-{0}-windows-x64.zip' -f $version
$zipPath = Join-Path $destinationPath $zipName

# An explicit three-file list excludes settings, probes, compiler files and PDBs.
$files = [ordered]@{
    'PeripheralBattery.exe' = $exe
    'README.txt' = (Join-Path $projectRoot 'docs/portable-readme.txt')
    'LICENSE.txt' = (Join-Path $projectRoot 'LICENSE')
}
Add-Type -AssemblyName System.IO.Compression
Add-Type -AssemblyName System.IO.Compression.FileSystem
$stream = [IO.File]::Open($zipPath, [IO.FileMode]::Create, [IO.FileAccess]::Write)
$archive = [IO.Compression.ZipArchive]::new($stream, [IO.Compression.ZipArchiveMode]::Create, $false)
try {
    foreach ($entry in $files.GetEnumerator()) {
        [IO.Compression.ZipFileExtensions]::CreateEntryFromFile($archive, $entry.Value, $entry.Key, [IO.Compression.CompressionLevel]::Optimal) | Out-Null
    }
} finally { $archive.Dispose() }
$check = [IO.Compression.ZipFile]::OpenRead($zipPath)
try {
    $actual = @($check.Entries | ForEach-Object { $_.FullName } | Sort-Object)
    $expected = @($files.Keys | Sort-Object)
    if (@(Compare-Object $expected $actual).Count -ne 0) { throw 'Unexpected ZIP contents.' }
} finally { $check.Dispose() }
$hash = (Get-FileHash -LiteralPath $zipPath -Algorithm SHA256).Hash.ToLowerInvariant()
[IO.File]::WriteAllText(($zipPath + '.sha256'), ($hash + '  ' + $zipName + [Environment]::NewLine), [Text.Encoding]::ASCII)
$utf8 = [Text.UTF8Encoding]::new($false)
[IO.File]::WriteAllText((Join-Path $destinationPath 'release-notes.md'), ($section.Groups[1].Value.Trim() + [Environment]::NewLine), $utf8)
Write-Host "Portable ZIP: $zipPath"
Write-Host "SHA256: $hash"
