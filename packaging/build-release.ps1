<#
    build-release.ps1 - build both plugins Release|x64 for packaging.

        powershell -File packaging/build-release.ps1

    Then package with make-release.ps1.

    Uses the 64-bit MSBuild on purpose. Projucer's VST3 post-build step only
    generates Contents/Resources/moduleinfo.json when PROCESSOR_ARCHITECTURE is
    AMD64 or ARM64; under the default (32-bit) MSBuild it reads "x86" and the
    step silently skips the manifest. The Visual Studio IDE is 64-bit, so IDE
    builds were fine - command-line builds were not. Fixing the generated
    .vcxproj instead would be undone by the next Projucer save.

    Fails if either bundle comes out without a manifest.

    Kept ASCII-only on purpose: Windows PowerShell 5.1 reads BOM-less scripts
    as ANSI, so non-ASCII punctuation here becomes a parse error.
#>
param(
    [string] $Configuration = 'Release'
)

$ErrorActionPreference = 'Stop'
$repoRoot = (Resolve-Path (Join-Path $PSScriptRoot '..')).Path

$vswhere = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio\Installer\vswhere.exe'
if (-not (Test-Path $vswhere)) { throw "vswhere.exe not found - is Visual Studio installed?" }
$msbuild = & $vswhere -latest -requires Microsoft.Component.MSBuild -find 'MSBuild\**\Bin\amd64\MSBuild.exe' |
    Select-Object -First 1
if (-not $msbuild) { throw "64-bit MSBuild not found via vswhere" }
Write-Host "Using $msbuild"

foreach ($p in @('PlugNspectrPre', 'PlugNspectrPost')) {
    $sln = Join-Path $repoRoot "$p\Builds\VisualStudio2026\$p.sln"
    & $msbuild $sln "-p:Configuration=$Configuration" '-p:Platform=x64' '-m' '-v:minimal' '-nologo'
    if ($LASTEXITCODE -ne 0) { throw "$p build failed" }

    $info = Join-Path $repoRoot "$p\Builds\VisualStudio2026\x64\$Configuration\VST3\$p.vst3\Contents\Resources\moduleinfo.json"
    if (-not (Test-Path $info)) { throw "$p built but has no moduleinfo.json - the VST3 manifest step did not run" }
    $ver = ((Get-Content $info -Raw) | Select-String '"Version":\s*"([^"]+)"').Matches[0].Groups[1].Value
    Write-Host "$p OK - moduleinfo.json Version $ver"
}
