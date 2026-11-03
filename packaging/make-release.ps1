<#
    make-release.ps1 - package a PlugNspectr release zip.

    Builds are NOT run here. Build both plugins Release|x64 first, then:

        powershell -File packaging/build-release.ps1
        powershell -File packaging/make-release.ps1 -Version 1.0.2

    Refuses to package if either bundle's moduleinfo.json or file version
    differs from -Version.

    Produces <OutDir>/PlugNspectr-<Version>-Windows-x64.zip containing both
    VST3 bundles plus INSTALL.txt, rendered from packaging/INSTALL.txt.

    The licence wording lives in that tracked template - never inline here and
    never hand-typed at release time - so it cannot silently drift out of sync
    with LICENSE (which is exactly how the zips shipped a stale "GPLv3" line).

    Kept ASCII-only on purpose: Windows PowerShell 5.1 reads BOM-less scripts
    as ANSI, so non-ASCII punctuation here becomes a parse error.
#>
param(
    [Parameter(Mandatory = $true)][string] $Version,
    [string] $OutDir = $env:TEMP
)

$ErrorActionPreference = 'Stop'

$repoRoot = (Resolve-Path (Join-Path $PSScriptRoot '..')).Path
$name     = "PlugNspectr-$Version-Windows-x64"
$stageDir = Join-Path ([System.IO.Path]::GetTempPath()) "pns-pkg-$Version"
$staging  = Join-Path $stageDir $name
$zipPath  = Join-Path $OutDir "$name.zip"

# ---- Stage the built VST3 bundles ---------------------------------------
if (Test-Path $stageDir) { Remove-Item $stageDir -Recurse -Force }
New-Item -ItemType Directory -Force $staging | Out-Null

foreach ($p in @('PlugNspectrPre', 'PlugNspectrPost')) {
    $bundle = Join-Path $repoRoot "$p\Builds\VisualStudio2026\x64\Release\VST3\$p.vst3"
    if (-not (Test-Path $bundle)) {
        throw "Missing $p.vst3 - build $p as Release|x64 first (packaging/build-release.ps1). Looked in: $bundle"
    }

    # Refuse to ship a bundle that reports a different version than the one
    # being released: hosts read moduleinfo.json, Windows reads the binary's
    # file version. Both drifted unnoticed to 1.0.0 in every release up to 1.0.3.
    $info = Join-Path $bundle 'Contents\Resources\moduleinfo.json'
    if (-not (Test-Path $info)) {
        throw "$p.vst3 has no moduleinfo.json - build with packaging/build-release.ps1 (64-bit MSBuild)"
    }
    $infoVer = ((Get-Content $info -Raw) | Select-String '"Version":\s*"([^"]+)"').Matches[0].Groups[1].Value
    $binVer  = (Get-Item (Join-Path $bundle "Contents\x86_64-win\$p.vst3")).VersionInfo.FileVersion
    if ($infoVer -ne $Version -or $binVer -ne $Version) {
        throw "$p reports moduleinfo $infoVer / file version $binVer, not $Version - bump the version and rebuild"
    }

    Copy-Item $bundle (Join-Path $staging "$p.vst3") -Recurse
}

# ---- Render INSTALL.txt from the tracked template -----------------------
$template = Join-Path $repoRoot 'packaging\INSTALL.txt'
if (-not (Test-Path $template)) { throw "Missing template: $template" }
(Get-Content $template -Raw).Replace('{VERSION}', $Version) |
    Set-Content (Join-Path $staging 'INSTALL.txt') -Encoding utf8

# ---- Zip ----------------------------------------------------------------
if (Test-Path $zipPath) { Remove-Item $zipPath -Force }
Compress-Archive -Path $staging -DestinationPath $zipPath
Remove-Item $stageDir -Recurse -Force

$mb = [math]::Round((Get-Item $zipPath).Length / 1MB, 2)
Write-Host "Created $zipPath ($mb MB)"
