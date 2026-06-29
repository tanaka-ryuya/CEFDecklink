# ==============================================================
# monitor.ps1 - DeckLink Blackout Monitor
# Compatible with PowerShell 5.x
# ==============================================================

param(
    [string]$LogDir = "$PSScriptRoot\..\build\Release\logs",
    [switch]$Tail
)

# Resolve path (PS5 compatible)
if (Test-Path $LogDir) {
    $LogDir = (Resolve-Path $LogDir).Path
} else {
    Write-Host "[ERROR] Log directory not found: $LogDir" -ForegroundColor Red
    Write-Host "  -> Start the application first with start.bat" -ForegroundColor Yellow
    exit 1
}

# Find latest log file
$latestLog = Get-ChildItem -Path $LogDir -Filter "app_*.log" |
             Sort-Object LastWriteTime -Descending |
             Select-Object -First 1

if (-not $latestLog) {
    Write-Host "[ERROR] No log files found in: $LogDir" -ForegroundColor Red
    exit 1
}

$logPath = $latestLog.FullName
Write-Host "==================================================" -ForegroundColor Cyan
Write-Host " DeckLink Blackout Monitor" -ForegroundColor Cyan
Write-Host "==================================================" -ForegroundColor Cyan
Write-Host " Log file : $logPath"
Write-Host " Log size : $([math]::Round($latestLog.Length / 1KB, 1)) KB"
Write-Host " Modified : $($latestLog.LastWriteTime)"
Write-Host ""

# Check if app is still alive (log updated in last 90 seconds)
$ageSeconds = [math]::Round(((Get-Date) - $latestLog.LastWriteTime).TotalSeconds)
if ($ageSeconds -gt 90) {
    Write-Host "[DEAD?] Log not updated for $ageSeconds seconds - app may have crashed!" -ForegroundColor Red
} else {
    Write-Host "[ALIVE] Log updated $ageSeconds seconds ago" -ForegroundColor Green
}
Write-Host ""

# Read all lines
$lines = Get-Content $logPath

# --- Summary counters ---
$blackouts    = @($lines | Where-Object { $_ -match '\[BLACKOUT\]' })
$blackoutEnds = @($lines | Where-Object { $_ -match '\[BLACKOUT_END\]' })
$warns        = @($lines | Where-Object { $_ -match '\[WARN\]' })
$errors       = @($lines | Where-Object { $_ -match '\[ERROR\]' })
$heartbeats   = @($lines | Where-Object { $_ -match '\[HEARTBEAT\]' })
$statusLines  = @($lines | Where-Object { $_ -match '\[STATUS\]' })

Write-Host "--- Summary ---" -ForegroundColor Yellow
if ($blackouts.Count -gt 0) {
    Write-Host "  Blackout events   : $($blackouts.Count)" -ForegroundColor Red
} else {
    Write-Host "  Blackout events   : $($blackouts.Count)" -ForegroundColor Green
}
Write-Host "  Blackout recovers : $($blackoutEnds.Count)" -ForegroundColor Cyan
if ($warns.Count -gt 0) {
    Write-Host "  WARN messages     : $($warns.Count)" -ForegroundColor Yellow
} else {
    Write-Host "  WARN messages     : $($warns.Count)" -ForegroundColor Green
}
if ($errors.Count -gt 0) {
    Write-Host "  ERROR messages    : $($errors.Count)" -ForegroundColor Red
} else {
    Write-Host "  ERROR messages    : $($errors.Count)" -ForegroundColor Green
}
Write-Host "  Heartbeats logged : $($heartbeats.Count)"
Write-Host ""

# --- Show latest STATUS ---
if ($statusLines.Count -gt 0) {
    Write-Host "--- Latest STATUS ---" -ForegroundColor Yellow
    $statusLines | Select-Object -Last 3 | ForEach-Object { Write-Host "  $_" }
    Write-Host ""
}

# --- Show BLACKOUT details ---
if ($blackouts.Count -gt 0) {
    Write-Host "--- BLACKOUT Events ---" -ForegroundColor Red
    $blackouts | ForEach-Object { Write-Host "  $_" -ForegroundColor Red }
    Write-Host ""

    if ($blackouts.Count -ge 2) {
        try {
            $firstTime = [datetime]::ParseExact(($blackouts[0] -replace ' \[BLACKOUT\].*', '').Trim(), 'yyyy-MM-dd HH:mm:ss', $null)
            $lastTime  = [datetime]::ParseExact(($blackouts[-1] -replace ' \[BLACKOUT\].*', '').Trim(), 'yyyy-MM-dd HH:mm:ss', $null)
            $totalSpanMin   = ($lastTime - $firstTime).TotalMinutes
            $avgIntervalMin = if ($blackouts.Count -gt 1) { $totalSpanMin / ($blackouts.Count - 1) } else { 0 }
            Write-Host "  First blackout : $firstTime"
            Write-Host "  Last blackout  : $lastTime"
            Write-Host ("  Avg interval   : {0:N1} minutes" -f $avgIntervalMin) -ForegroundColor Yellow
            Write-Host ""
        } catch {}
    }
}

# --- Show BLACKOUT_END ---
if ($blackoutEnds.Count -gt 0) {
    Write-Host "--- BLACKOUT_END (recovery durations) ---" -ForegroundColor Cyan
    $blackoutEnds | ForEach-Object { Write-Host "  $_" -ForegroundColor Cyan }
    Write-Host ""
}

# --- Show WARNs ---
if ($warns.Count -gt 0) {
    Write-Host "--- WARN Messages ---" -ForegroundColor Yellow
    $warns | Select-Object -Last 10 | ForEach-Object { Write-Host "  $_" -ForegroundColor Yellow }
    Write-Host ""
}

# --- Show ERRORs ---
if ($errors.Count -gt 0) {
    Write-Host "--- ERROR Messages ---" -ForegroundColor Red
    $errors | Select-Object -Last 10 | ForEach-Object { Write-Host "  $_" -ForegroundColor Red }
    Write-Host ""
}

# --- Show latest HEARTBEAT ---
if ($heartbeats.Count -gt 0) {
    Write-Host "--- Latest HEARTBEAT ---" -ForegroundColor Cyan
    $heartbeats | Select-Object -Last 1 | ForEach-Object { Write-Host "  $_" }
    Write-Host ""
}

# --- Save report ---
$reportPath = Join-Path $LogDir "monitor_report.txt"
$report = @"
=== Monitor Report: $(Get-Date) ===
Log file    : $logPath
App alive   : $(if ($ageSeconds -le 90) { "YES (${ageSeconds}s ago)" } else { "UNKNOWN (${ageSeconds}s ago)" })
Blackouts   : $($blackouts.Count)
Recoveries  : $($blackoutEnds.Count)
Warnings    : $($warns.Count)
Errors      : $($errors.Count)
Heartbeats  : $($heartbeats.Count)

--- BLACKOUT events ---
$($blackouts -join "`n")

--- BLACKOUT_END events ---
$($blackoutEnds -join "`n")

--- WARN events ---
$($warns -join "`n")

--- ERROR events ---
$($errors -join "`n")
"@
$report | Set-Content $reportPath -Encoding UTF8

Write-Host "==================================================" -ForegroundColor Cyan
Write-Host " Report saved: $reportPath" -ForegroundColor Green
Write-Host "==================================================" -ForegroundColor Cyan

# --- Tail mode ---
if ($Tail) {
    Write-Host ""
    Write-Host "[Tail mode] Watching log... (Ctrl+C to stop)" -ForegroundColor Magenta
    Get-Content $logPath -Wait | Where-Object {
        $_ -match '\[BLACKOUT\]|\[BLACKOUT_END\]|\[WARN\]|\[ERROR\]|\[HEARTBEAT\]|\[STATUS\]'
    } | ForEach-Object {
        $color = 'White'
        if ($_ -match '\[BLACKOUT\]')     { $color = 'Red' }
        elseif ($_ -match '\[BLACKOUT_END\]') { $color = 'Cyan' }
        elseif ($_ -match '\[WARN\]')     { $color = 'Yellow' }
        elseif ($_ -match '\[ERROR\]')    { $color = 'Red' }
        elseif ($_ -match '\[HEARTBEAT\]'){ $color = 'Cyan' }
        Write-Host $_ -ForegroundColor $color
    }
}
