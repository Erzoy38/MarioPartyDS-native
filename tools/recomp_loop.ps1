# Runs the translated game. When it reaches code that was not translated, records the address,
# translates again and retries. Stops on any other result and prints it.
# Usage: powershell -ExecutionPolicy Bypass -File tools\recomp_loop.ps1 [-Rounds 40] [-MaxBlocks 400000000]
param([int]$Rounds = 40, [string]$MaxBlocks = "400000000")
$rom = "Mario Party DS (Europe) (En,Fr,De,Es,It) (Rev 1).nds"
$file = "tools\recomp_entries.txt"
for ($round = 1; $round -le $Rounds; $round++) {
    python tools/recomp.py | Out-Null
    if ($LASTEXITCODE -ne 0) { "recompiler failed"; exit 1 }
    powershell -NoProfile -ExecutionPolicy Bypass -File native\build.ps1 | Select-String -Pattern "error|FAILED|failed" | Select-Object -First 8
    if ($LASTEXITCODE -ne 0) { "build failed"; exit 1 }
    $out = & native\build\MarioPartyDS-headless.exe $rom --stop 0xFFFFFFFC --max-blocks $MaxBlocks
    $code = $LASTEXITCODE
    $missing = $out | Select-String -Pattern '^MISSING ([0-9a-f]+ \d( ov\d+)?)' | Select-Object -First 1
    if ($missing) {
        $line = $missing.Matches[0].Groups[1].Value
        if ((Test-Path $file) -and ((Get-Content $file) -contains $line)) {
            "round ${round}: $line is missing again although it was added (it may lie outside mapped code, e.g. an overlay)"
            $out
            exit 2
        }
        Add-Content -Path $file -Value $line -Encoding ascii
        "round ${round}: added $line"
        continue
    }
    "round ${round}: exit $code"
    $out
    exit 0
}
"round limit reached"
