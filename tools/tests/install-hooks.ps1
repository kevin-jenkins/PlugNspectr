# Point git at the repo's tracked hooks so the unit tests run before every push.
# Run once per clone:  powershell -File tools/tests/install-hooks.ps1
$root = (git rev-parse --show-toplevel)
if (-not $root) { Write-Host "Not inside a git repo." -ForegroundColor Red; exit 1 }
git -C $root config core.hooksPath .githooks
Write-Host "Installed: core.hooksPath -> .githooks" -ForegroundColor Green
Write-Host "The pre-push hook now runs tools/tests/run-tests.ps1 before each push."
Write-Host "Bypass a single push with: git push --no-verify"
