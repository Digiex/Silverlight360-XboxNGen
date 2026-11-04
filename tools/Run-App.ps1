<#
.SYNOPSIS
Runs a package in xenia-edge, checking every 5 seconds that xenia is still running, and stops as soon as the app
reports it is done (a DbgPrint line matching -DonePattern), xenia exits or crashes, the log stops growing for
-IdleSeconds (a hang, or an app idle on an error, or a UI app that has finished loading), or the timeout passes.

.DESCRIPTION
xenia runs --unattended: its window shows, its dialogs (the host's "Error: 4004" box among them) are logged and
dismissed with their default button, and it exits when the app does (code 0) or crashes (code 1). The screenshot
(-Screenshot) is the app's own when it saved one (the SDK's samples\Common\Screenshot.cs logs "screenshot
game:\<name>.argb <w>x<h>"; xenia runs with --allow_game_relative_writes so the app's folder is writable), made a PNG
by argb2png.py; otherwise, before stopping a xenia still running, a capture of its window, which PrintWindow
sometimes gives as xenia's stale splash frame. Prints the app's DbgPrint lines, the
dialogs it showed and, if xenia crashed, its exit code, the Windows Error Reporting record since the start (exception
code and offset in xenia_edge.exe) and the end of the log.
#>
param(
    [Parameter(Mandatory)] [string]$Xex,
    [string]$Log = (Join-Path (Split-Path $Xex -Parent) 'xenia.log'),
    [string]$Screenshot = (Join-Path (Split-Path $Xex -Parent) 'xenia.png'),
    [string]$DonePattern = '',
    [int]$Seconds = 120,
    [int]$IdleSeconds = 20,
    # xenia-edge's guest scheduler sometimes stalls right after startup (a thread in a system module spinning,
    # before the app's code runs; the log says "no guest frame presented in ... watchdog ticks"): the run is
    # killed and started again, this many times at most.
    [int]$Restarts = 3,
    [string]$Xenia = (& (Join-Path $PSScriptRoot 'Config.ps1') Xenia),
    [string[]]$ExtraArgs = @()
)
$ErrorActionPreference = 'Stop'
Add-Type -AssemblyName System.Drawing
if (-not ('XboxNGen.Window' -as [type])) {
    Add-Type @"
using System;
using System.Runtime.InteropServices;
namespace XboxNGen {
public static class Window {
  [StructLayout(LayoutKind.Sequential)] public struct RECT { public int Left, Top, Right, Bottom; }
  [DllImport("user32.dll")] public static extern bool GetWindowRect(IntPtr h, out RECT r);
  [DllImport("user32.dll")] public static extern bool PrintWindow(IntPtr h, IntPtr hdc, uint flags);
}
}
"@
}

# PrintWindow of xenia's D3D12 window sometimes gives its splash screen in a stale, smaller frame (white at the right
# and bottom) long after the app has drawn, for any of the samples at random: the capture is retried every 3 seconds,
# for up to 30, while the pixel near the bottom right corner is white.
function Save-Screenshot([System.Diagnostics.Process]$p, [string]$file) {
    for ($try = 0; $try -lt 10; $try++) {
        $p.Refresh()
        $window = $p.MainWindowHandle
        if ($p.HasExited -or $window -eq [IntPtr]::Zero) { return }
        $r = New-Object XboxNGen.Window+RECT
        [void][XboxNGen.Window]::GetWindowRect($window, [ref]$r)
        $bmp = New-Object System.Drawing.Bitmap ($r.Right - $r.Left), ($r.Bottom - $r.Top)
        $g = [System.Drawing.Graphics]::FromImage($bmp)
        $hdc = $g.GetHdc(); [void][XboxNGen.Window]::PrintWindow($window, $hdc, 2); $g.ReleaseHdc($hdc)
        $corner = $bmp.GetPixel([Math]::Max(0, $bmp.Width - 40), [Math]::Max(0, $bmp.Height - 30))
        $stale = $corner.R -gt 240 -and $corner.G -gt 240 -and $corner.B -gt 240
        if (-not $stale -or $try -eq 9) {
            $bmp.Save($file, [System.Drawing.Imaging.ImageFormat]::Png)
            $g.Dispose(); $bmp.Dispose()
            Write-Host "screenshot: $file$(if ($stale) { ' (still the stale splash frame)' })"
            return
        }
        $g.Dispose(); $bmp.Dispose()
        Start-Sleep -Seconds 3
    }
}

if ($Screenshot) { $Screenshot = $ExecutionContext.SessionState.Path.GetUnresolvedProviderPathFromPSPath($Screenshot) }
$arguments = @('--unattended=true', "--log_file=`"$Log`"", '--log_level=2', '--flush_log=true',
    '--allow_game_relative_writes=true') + $ExtraArgs + @("`"$((Resolve-Path $Xex).Path)`"")
$stallPattern = 'GuestScheduler: no guest frame presented in [0-9]+ watchdog ticks'
$attempt = 0
while ($true) {
    if (Test-Path $Log) { Remove-Item $Log -Force }
    $started = Get-Date
    # stdout's copy of the log goes nowhere; -Log has it.
    $process = Start-Process -FilePath $Xenia -ArgumentList $arguments -RedirectStandardOutput 'NUL' -PassThru
    # Opening the handle now keeps ExitCode readable once the process is gone.
    $null = $process.Handle
    $state = 'timeout'
    $lastSize = -1
    $quietSince = Get-Date
    while ((Get-Date) -lt $started.AddSeconds($Seconds)) {
        Start-Sleep -Seconds 5
        if ($process.HasExited) { $state = 'exited'; break }
        if ($DonePattern -and (Test-Path $Log) -and (Select-String -Path $Log -Pattern $DonePattern -Quiet)) { $state = 'done'; break }
        if ((Test-Path $Log) -and (Select-String -Path $Log -Pattern $stallPattern -Quiet)) { $state = 'stalled'; break }
        # A log that stops growing means the app hung, is idle on an error, or has finished loading.
        $size = if (Test-Path $Log) { (Get-Item $Log).Length } else { 0 }
        if ($size -ne $lastSize) { $lastSize = $size; $quietSince = Get-Date }
        elseif ((Get-Date) -gt $quietSince.AddSeconds($IdleSeconds)) { $state = "idle for $IdleSeconds s"; break }
        Write-Host ("  {0,3}s: running" -f [int]((Get-Date) - $started).TotalSeconds)
    }
    if ($state -ne 'stalled' -or $attempt -ge $Restarts) { break }
    $attempt++
    Write-Host "xenia: scheduler stall after $([int]((Get-Date) - $started).TotalSeconds)s, restarting ($attempt of $Restarts)"
    # The stalled run's log is kept beside the log, for a look at what the fibers were waiting on.
    if (Test-Path $Log) { Copy-Item $Log ($Log -replace '\.log$', '.stall.log') -Force }
    if (-not $process.HasExited) { Stop-Process -Id $process.Id -Force; Start-Sleep -Seconds 2 }
}
# The app's own screenshot, if it logged one in this run.
$own = if ($Screenshot -and (Test-Path $Log)) {
    Select-String -Path $Log -Pattern '\(DbgPrint\) screenshot game:\\(\S+) ([0-9]+)x([0-9]+)' | Select-Object -Last 1
}
if ($own) {
    $file = Join-Path (Split-Path $Xex -Parent) $own.Matches[0].Groups[1].Value
    & python (Join-Path $PSScriptRoot 'argb2png.py') $file $own.Matches[0].Groups[2].Value $own.Matches[0].Groups[3].Value $Screenshot
    if ($LASTEXITCODE -eq 0) { Write-Host "screenshot: $Screenshot (the app's own)" } else { $own = $null }
}
if (-not $process.HasExited) {
    if ($Screenshot -and -not $own) {
        # A few seconds for the frames after the done line (Save-Screenshot retries a stale capture).
        if ($state -eq 'done') { Start-Sleep -Seconds 5 }
        Save-Screenshot $process $Screenshot
    }
    Stop-Process -Id $process.Id -Force
    Start-Sleep -Milliseconds 500
}
# The title's main returned with the host's HRESULT (an unhandled exception's, 0x8013150A for one), and xenia exited.
if ($state -eq 'exited' -and (Test-Path $Log) -and
    ($m = Select-String -Path $Log -Pattern '\[XAPI RETURN VALUE\] (-?[0-9]+)' | Select-Object -First 1)) {
    $state = 'app exited (hr 0x{0:X8})' -f [int]$m.Matches[0].Groups[1].Value
}
# The dialogs xenia dismissed: an error box among them means the app failed.
$dialogs = if (Test-Path $Log) { @(Select-String -Path $Log -Pattern 'UI (dialog|message box): ' -Context 0, 4) } else { @() }
if ($dialogs | Where-Object { $_.Line -match 'UI (dialog|message box): .*Error' }) {
    $state = "app error dialog ($state)"
    # Keep this run's log: the next run overwrites it.
    Copy-Item $Log ($Log -replace '\.log$', '.error.log') -Force
}

if (Test-Path $Log) {
    Select-String -Path $Log -Pattern '\(DbgPrint\) (.*)' | ForEach-Object { $_.Matches[0].Groups[1].Value } |
        Where-Object { $_ -notmatch '^(log|open): ' }
}
foreach ($d in $dialogs) {
    # The dialog's text runs to the next log line (they start with a level and a frame number).
    Write-Host ($d.Line -replace '^.*? UI ', 'UI ')
    foreach ($line in $d.Context.PostContext) {
        if ($line -match '^.> f:') { break }
        Write-Host "  $line"
    }
}
Write-Host "xenia: $state after $([int]((Get-Date) - $started).TotalSeconds)s"
if ($state -eq 'exited' -and $process.ExitCode) {
    Write-Host "exit code: $($process.ExitCode)"
    Get-WinEvent -FilterHashtable @{ LogName = 'Application'; Id = 1000; StartTime = $started } -ErrorAction SilentlyContinue |
        Where-Object { $_.Message -match 'xenia_edge' } | Select-Object -First 1 |
        ForEach-Object { ($_.Message -split "`n" | Select-Object -First 4) -join ' | ' }
    if (Test-Path $Log) { Get-Content $Log -Tail 15 | Where-Object { $_ -notmatch 'Invalid instruction' } }
}
