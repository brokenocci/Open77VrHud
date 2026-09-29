$ErrorActionPreference = "Stop"
$here = Split-Path -Parent $MyInvocation.MyCommand.Path
$zig = Get-Command zig -ErrorAction SilentlyContinue
if (-not $zig) {
    $candidate = Join-Path $env:LOCALAPPDATA "zig-0.14.1\zig-x86_64-windows-0.14.1\zig.exe"
    if (Test-Path $candidate) { $zig = $candidate } else { throw "zig was not found on PATH" }
} else {
    $zig = $zig.Source
}
& $zig cc -shared -O2 -Wall -Wno-unused-function -target x86_64-windows-gnu -o (Join-Path $here "Open77VrHud.dll") (Join-Path $here "Open77VrHud.c") -ld3d12 -ld3d11 -ldxgi -lole32 -luuid -luser32 -lpsapi
if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }
Write-Output "Open77VrHud.dll written to $here"
