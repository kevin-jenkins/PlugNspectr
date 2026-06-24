# Configure, build, and run the PlugNspectr unit tests (doctest).
# Usage:  powershell -File tools/tests/run-tests.ps1   (run from anywhere)

$cmake = (Get-Command cmake -ErrorAction SilentlyContinue).Source
if (-not $cmake) { $cmake = "C:\Program Files\Microsoft Visual Studio\18\Community\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe" }

$src = Join-Path $PSScriptRoot "..\render-harness"
$bld = Join-Path $src "build"

& $cmake -S $src -B $bld | Out-Null
if ($LASTEXITCODE -ne 0) { Write-Host "CMake configure failed" -ForegroundColor Red; exit 1 }

& $cmake --build $bld --config Release --target PnsTests PnsTestsPre | Out-Null
if ($LASTEXITCODE -ne 0) { Write-Host "Build failed" -ForegroundColor Red; exit 1 }

$fail = 0
foreach ($t in @("PnsTests", "PnsTestsPre")) {
    $exe = Get-ChildItem $bld -Recurse -Filter "$t.exe" | Select-Object -First 1
    Write-Host "`n=== $t ===" -ForegroundColor Cyan
    & $exe.FullName
    if ($LASTEXITCODE -ne 0) { $fail = 1 }
}
exit $fail
