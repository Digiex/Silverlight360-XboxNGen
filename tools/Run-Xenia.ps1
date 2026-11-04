<#
.SYNOPSIS
Runs a staged package's default.xex in xenia-edge until the title exits or a timeout, then prints its DbgPrint lines.

.DESCRIPTION
The package folder is mounted writable (--allow_game_relative_writes), which the zapper needs to write native
images beside the assemblies it compiles. xenia runs --headless: no window, dialogs are logged and dismissed, and the
process exits when the title does (code 0) or crashes (code 1). Its stdout copy of the log is discarded; -Log has it.
#>
param(
    [Parameter(Mandatory)] [string]$Stage,
    [int]$Seconds = 120,
    [string]$Log = (Join-Path $Stage 'xenia.log'),
    [string]$Xenia = (& (Join-Path $PSScriptRoot 'Config.ps1') Xenia),
    [string[]]$ExtraArgs = @(),
    # Stops when the log hasn't grown for this long: a hung zapper (its main thread spinning) writes nothing more.
    [int]$IdleSeconds = 20
)
$ErrorActionPreference = 'Stop'
if (Test-Path $Log) { Remove-Item $Log -Force }
$xex = Join-Path (Resolve-Path $Stage) 'default.xex'
$arguments = @('--headless=true', "--log_file=`"$Log`"", '--log_level=2', '--flush_log=true', '--allow_game_relative_writes=true') + $ExtraArgs + @("`"$xex`"")
$process = Start-Process -FilePath $Xenia -ArgumentList $arguments -RedirectStandardOutput 'NUL' -PassThru
# Opening the handle now keeps ExitCode readable once the process is gone.
$null = $process.Handle
$deadline = (Get-Date).AddSeconds($Seconds)
$lastSize = -1
$lastGrowth = Get-Date
while ((Get-Date) -lt $deadline -and -not $process.HasExited) {
    Start-Sleep -Milliseconds 500
    if (-not (Test-Path $Log)) { continue }
    # The title's own output, not the log's size: xenia-edge's scheduler keeps writing its no-progress dumps.
    $size = (Select-String -Path $Log -Pattern '\(DbgPrint\)' | Measure-Object).Count
    if ($size -ne $lastSize) { $lastSize = $size; $lastGrowth = Get-Date }
    elseif (((Get-Date) - $lastGrowth).TotalSeconds -ge $IdleSeconds) {
        Write-Warning "xenia: the log stopped growing for $IdleSeconds s (hung?)"
        break
    }
}
if (-not $process.HasExited) { Stop-Process -Id $process.Id -Force }
elseif ($process.ExitCode) { Write-Warning "xenia: exited with code $($process.ExitCode) (crashed?)" }
Select-String -Path $Log -Pattern '\(DbgPrint\) (.*)' | ForEach-Object { $_.Matches[0].Groups[1].Value } |
    Where-Object { $_ -notmatch '^(log|open): ' }
