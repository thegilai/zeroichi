# Zero Ichi: downloads the packages listed in download-list.txt onto this SD card.
$ErrorActionPreference = 'Stop'
$ProgressPreference = 'SilentlyContinue'      # makes downloads much faster
[Net.ServicePointManager]::SecurityProtocol = [Net.SecurityProtocolType]::Tls12

$root = Split-Path -Parent $MyInvocation.MyCommand.Path
$list = Join-Path $root 'download-list.txt'
Write-Host ''
Write-Host '  Zero Ichi package downloader' -ForegroundColor Yellow
Write-Host '  -------------------------'
if (-not (Test-Path $list)) {
    Write-Host '  Nothing to download - the Pi has not asked for anything.' -ForegroundColor Green
    Write-Host '  Put the card back in the Pi and power it on.'
    exit 0
}

$entries = @()
foreach ($line in Get-Content $list) {
    if ($line -match '^\s*#' -or $line.Trim() -eq '') { continue }
    $parts = $line -split "`t"
    if ($parts.Count -ge 3) {
        $entries += [pscustomobject]@{ Kind = $parts[0].Trim(); Url = $parts[1].Trim(); Rel = $parts[2].Trim() }
    }
}

$ok = 0; $skipped = 0; $failed = 0; $n = 0
foreach ($e in $entries) {
    $n++
    $target = Join-Path $root ($e.Rel -replace '/', '\')
    $name = Split-Path -Leaf $target
    $dir = Split-Path -Parent $target
    if (-not (Test-Path $dir)) { New-Item -ItemType Directory -Force -Path $dir | Out-Null }
    if ((Test-Path $target) -and ((Get-Item $target).Length -gt 0) -and $e.Rel -notmatch 'InRelease$|Packages\.(xz|gz)$') {
        $skipped++; continue
    }
    Write-Host ("  [{0}/{1}] {2}" -f $n, $entries.Count, $name) -NoNewline
    $tmp = "$target.part"
    try {
        Invoke-WebRequest -Uri $e.Url -OutFile $tmp -UseBasicParsing
        Move-Item -Force $tmp $target
        $ok++
        Write-Host '  OK' -ForegroundColor Green
    } catch {
        if (Test-Path $tmp) { Remove-Item -Force $tmp }
        if ($e.Kind -eq 'OPT') {
            Write-Host '  (not on server - that is fine)' -ForegroundColor DarkGray
        } else {
            $failed++
            Write-Host "  FAILED: $($_.Exception.Message)" -ForegroundColor Red
        }
    }
}

Write-Host ''
if ($failed -gt 0) {
    Write-Host "  $failed required download(s) failed. Check your internet connection and run this again." -ForegroundColor Red
} else {
    Write-Host "  Done! ($ok downloaded, $skipped already there)" -ForegroundColor Green
    Write-Host '  Now eject the SD card safely, put it back in the Pi and power it on.'
}
