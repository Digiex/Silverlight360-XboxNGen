<#
.SYNOPSIS
Times NGenBench's timing loops (test\NGenBench\Timing.cs) on Microsoft's images and on ours.

An image is bound to the mscorlib image it was compiled against, and the retail runtime has no JIT, so NGenBench's
own code can't be compiled for Microsoft's images: there it runs interpreted. Three configurations, each a copy of the
bench package as Test-Mscorlib.ps1 leaves it, under <work folder>\timing:
  microsoft    Microsoft's mscorlib and platform images, NGenBench interpreted
  interpreted  our mscorlib and platform images, NGenBench interpreted
  ours         our images, NGenBench compiled
microsoft against interpreted compares the library code (StringBuilder, Dictionary, List<T>, Math); interpreted
against ours shows what compiling the bench's own code buys. Interpreted, the loops of the bench's own code do a
sixteenth of the work (they log "/16"); their times are scaled up and marked ~. -Runs repeats each configuration (one
xenia at a time) and prints every run's times.

.EXAMPLE
.\Test-Mscorlib.ps1 -SkipMscorlib -SkipPlatform; .\Bench-Timing.ps1 -Runs 2
#>
param(
    [string]$Bench = (Join-Path $PSScriptRoot 'NGenBench\bin\Release\xbox'),
    # The work folder (README.md, "Configuration").
    [string]$Ours = (Join-Path (& (Join-Path $PSScriptRoot '..\tools\Config.ps1') Work) 'ours'),
    [string]$Work = (Join-Path (& (Join-Path $PSScriptRoot '..\tools\Config.ps1') Work) 'timing'),
    [string[]]$Configurations = @('microsoft', 'interpreted', 'ours'),
    [int]$Runs = 1,
    [int]$Seconds = 900
)
$ErrorActionPreference = 'Stop'
$Bench = (Resolve-Path $Bench).Path
if (-not (Test-Path (Join-Path $Bench 'NGenBench.ni.dll'))) { throw "no NGenBench.ni.dll in ${Bench}: run Test-Mscorlib.ps1 first" }
if (-not (Test-Path $Work)) { $null = New-Item -ItemType Directory $Work }
$table = [ordered]@{}
$estimated = @{}
foreach ($config in $Configurations) {
    $dir = Join-Path $Work $config
    if (Test-Path $dir) { Remove-Item $dir -Recurse -Force }
    Copy-Item $Bench $dir -Recurse
    switch ($config) {
        'microsoft' {
            Copy-Item (Join-Path $Ours 'mscorlib.ni.dll.microsoft') (Join-Path $dir 'mscorlib.ni.dll') -Force
            Get-ChildItem (Join-Path $Ours 'bench-ni-aside') -Filter '*.ni.dll' | Copy-Item -Destination $dir -Force
            [IO.File]::Delete((Join-Path $dir 'NGenBench.ni.dll'))
        }
        'interpreted' { [IO.File]::Delete((Join-Path $dir 'NGenBench.ni.dll')) }
        'ours' { }
        default { throw "unknown configuration $config" }
    }
    for ($run = 1; $run -le $Runs; $run++) {
        $r = & (Join-Path $PSScriptRoot '..\tools\Run-App.ps1') -Xex (Join-Path $dir 'default.xex') -DonePattern 'NGenBench: [0-9]+ passed' `
            -Seconds $Seconds -IdleSeconds 120 2>&1 | Out-String -Stream |
            Select-String -Pattern 'NGenBench: [0-9]+ passed|crash|idle|error dialog' | Select-Object -Last 1
        $summary = if ($r) { ($r.Line -replace '^.*NGenBench: ', '') -replace ';.*', '' } else { 'no result' }
        "== $config run ${run}: $summary"
        foreach ($m in Select-String -Path (Join-Path $dir 'xenia.log') -Pattern 'NGenBench: time (\w+) (\d+) ms(?: /(\d+))?') {
            $name = $m.Matches[0].Groups[1].Value
            $part = if ($m.Matches[0].Groups[3].Success) { [int]$m.Matches[0].Groups[3].Value } else { 1 }
            if ($part -gt 1) { $estimated["$name/$config"] = $true }
            if (-not $table.Contains($name)) { $table[$name] = [ordered]@{} }
            $key = $config
            if (-not $table[$name].Contains($key)) { $table[$name][$key] = @() }
            $table[$name][$key] += [int]$m.Matches[0].Groups[2].Value * $part
        }
    }
}
# One row a loop: each configuration's times (ms), then the best run's ratios.
$header = '{0,-18}' -f 'loop'
foreach ($config in $Configurations) { $header += '{0,16}' -f $config }
$header += '   ms/interp  interp/ours'
$header
foreach ($name in $table.Keys) {
    $row = '{0,-18}' -f $name
    $best = @{}
    foreach ($config in $Configurations) {
        $times = $table[$name][$config]
        $mark = if ($estimated["$name/$config"]) { '~' } else { '' }
        $row += '{0,16}' -f $(if ($times) { $mark + ($times -join '/') } else { '-' })
        if ($times) { $best[$config] = ($times | Measure-Object -Minimum).Minimum }
    }
    $ratio = { param($a, $b) if ($best[$a] -and $best[$b]) { '{0,9:0.00}x' -f ($best[$a] / [double]$best[$b]) } else { '{0,10}' -f '-' } }
    $row += '  ' + (& $ratio 'microsoft' 'interpreted') + '  ' + (& $ratio 'interpreted' 'ours')
    $row
}
