param([string[]]$Only)
$root = (Resolve-Path (Join-Path $PSScriptRoot "..\..\..")).Path
$ccc = Join-Path $root "bin\ccc.exe"
$env:CCC_ROOT = $root
Push-Location $root
$failed = 0
foreach ($script in Get-ChildItem $PSScriptRoot -Filter *.txt) {
    if ($Only -and $Only -notcontains $script.BaseName) { continue }
    $watch = [Diagnostics.Stopwatch]::StartNew()
    $out = & $ccc --headless $script.FullName 2>&1 | Out-String
    $code = $LASTEXITCODE
    $ok = $code -eq 0
    if (-not $ok) { $failed++ }
    "{0,-8} {1,-6} {2,6:n1} s{3}" -f $script.BaseName, ($(if ($ok) { "pass" } else { "FAIL" })), $watch.Elapsed.TotalSeconds, ($(if ($ok) { "" } else { "  exit $code" }))
    if (-not $ok) { ($out -split "`n" | Select-String -Pattern "TIMEOUT" -Context 0, 40) | Out-String }
}
Pop-Location
exit $failed
