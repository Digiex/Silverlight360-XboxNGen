<#
.SYNOPSIS
Runs an app package in xenia-edge twice, on Microsoft's images (the package as shipped) and on ours alone
(Run-OnOurs.ps1), and prints how far each got: the run's end state, its error dialogs, the app's DbgPrint lines and
the last guest frame number. For apps without a done line of their own (the shipped packages), which stop when the
log goes quiet. Logs and screenshots go to <Work>\<name>-ms and <Work>\<name>-ours.

.EXAMPLE
.\Compare-Run.ps1 -Package <apps folder>\Plex
#>
param(
    [Parameter(Mandatory)] [string]$Package,
    [string]$Work = (Join-Path (& (Join-Path $PSScriptRoot '..\tools\Config.ps1') Work) 'apps'),
    [int]$IdleSeconds = 30,
    [int]$Seconds = 300
)
$ErrorActionPreference = 'Stop'
$Package = (Resolve-Path $Package).Path
$name = (Split-Path $Package -Leaf) -replace '[^A-Za-z0-9.+-]', '_'
$ms = Join-Path $Work "$name-ms"
if (-not (Test-Path $ms)) { $null = New-Item -ItemType Directory $ms }

function Summary([string]$label, [string[]]$output, [string]$log) {
    $state = ($output | Where-Object { $_ -match '^xenia: ' } | Select-Object -Last 1)
    $dialogs = @($output | Where-Object { $_ -match '^UI (dialog|message box)' })
    $frames = if (Test-Path $log) {
        (Select-String -Path $log -Pattern '^.> f:([0-9A-F]+) ' | Select-Object -Last 1).Matches[0].Groups[1].Value
    }
    $prints = if (Test-Path $log) { @(Select-String -Path $log -Pattern '\(DbgPrint\) (.*)' | ForEach-Object { $_.Matches[0].Groups[1].Value } | Where-Object { $_ -notmatch '^xclrjit: ' }) } else { @() }
    "== $label : $state; last frame 0x$frames; $($dialogs.Count) dialogs; $($prints.Count) DbgPrint lines"
    $dialogs | ForEach-Object { "   $_" }
    $prints | Select-Object -Last 8 | ForEach-Object { "   | $_" }
}

$out = & (Join-Path $PSScriptRoot '..\tools\Run-App.ps1') -Xex (Join-Path $Package 'default.xex') -Log (Join-Path $ms 'xenia.log') `
    -Screenshot (Join-Path $ms 'xenia.png') -IdleSeconds $IdleSeconds -Seconds $Seconds *>&1 | Out-String -Stream
Summary 'microsoft' $out (Join-Path $ms 'xenia.log')
$ours = Join-Path $Work "$name-ours"
$out = & (Join-Path $PSScriptRoot 'Run-OnOurs.ps1') -Package $Package -Work $ours -IdleSeconds $IdleSeconds *>&1 | Out-String -Stream
$declined = @($out | Where-Object { $_ -match 'xclrjit: declined' })
$failed = @($out | Where-Object { $_ -match 'hr 0x8|ngenhost: done, [1-9]' })
"== ours: $($declined.Count) declined, $($failed.Count) failed compiles"
$declined | Select-Object -First 5 | ForEach-Object { "   $_" }
$failed | ForEach-Object { "   $_" }
Summary 'ours' $out (Join-Path $ours 'xenia.log')
