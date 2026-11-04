<#
.SYNOPSIS
Compiles mscorlib with the current JIT, then the platform assemblies and NGenBench against it, and runs NGenBench
on those images alone.

1. mscorlib is compiled in the Vimeo package (NGen-Package writes the wrapped image back into that folder), the
   image is moved to <work folder>\ours and Microsoft's restored. -SkipMscorlib keeps the image already there.
2. The bench package gets our mscorlib.ni.dll; its platform assemblies (NGen-Platform.ps1, sized by Microsoft's
   images in <work folder>\ours\bench-ni-aside) and NGenBench are compiled against it, each in a run of its own.
3. NGenBench is run on the result.
Prints the results; the logs stay in the stage and package folders, the platform images are also copied to
<work folder>\ours\platform-ours.
#>
param(
    # The extracted app packages (Vimeo's is the mscorlib build's) and the work folder (README.md, "Configuration").
    [string]$Apps = (& (Join-Path $PSScriptRoot '..\tools\Config.ps1') Apps),
    [string]$Ours = (Join-Path (& (Join-Path $PSScriptRoot '..\tools\Config.ps1') Work) 'ours'),
    [switch]$SkipMscorlib,
    [switch]$SkipPlatform,
    [int]$Seconds = 3000,
    [string]$Jit = ''
)
$ErrorActionPreference = 'Stop'
if (-not $Apps) { throw 'no app packages folder: run tools\Configure.ps1 -Apps <folder>, set XBOXNGEN_APPS or pass -Apps' }
$root = Split-Path $PSScriptRoot -Parent
$bench = Join-Path $root 'test\NGenBench\bin\Release\xbox'
$vimeo = Join-Path $Apps 'Vimeo'
$microsoft = Join-Path $Ours 'mscorlib.ni.dll.microsoft'
$aside = Join-Path $Ours 'bench-ni-aside'
$stage = Join-Path $env:TEMP 'xboxngen-stage'
if (-not (Test-Path $microsoft)) { Copy-Item (Join-Path $vimeo 'mscorlib.ni.dll') $microsoft }
$platform = @('System', 'System.Core', 'System.Xml', 'System.Net', 'System.Windows', 'System.Windows.RuntimeHost',
    'System.Xml.Linq', 'System.Windows.Interactivity', 'System.Windows.Controls.Navigation',
    'Microsoft.Web.Media.SmoothStreaming', 'Microsoft.Xbox.Core', 'Microsoft.Xbox', 'Microsoft.Xbox.Services',
    'Microsoft.Xbox.Storage')
# The image file names xcoreclr shortens (NGen-Package.ps1).
$aliases = @{ 'System.Windows.Controls.Navigation' = 'SW.Controls.Navigation'; 'Microsoft.Web.Media.SmoothStreaming' = 'MWM.SmoothStreaming' }
function Image-Name($name) { if ($aliases.ContainsKey($name)) { $aliases[$name] } else { $name } }

# 1. mscorlib.
if (-not $SkipMscorlib) {
    Copy-Item $microsoft (Join-Path $vimeo 'mscorlib.ni.dll') -Force
    $r = & (Join-Path $PSScriptRoot '..\tools\NGen-Package.ps1') -Package $vimeo -Assemblies mscorlib -Seconds $Seconds -Jit $Jit 2>&1 |
        Out-String -Stream | Select-String -Pattern 'ngenhost: done|WARNING|crash|hr 0x' | Select-Object -Last 2
    "1. mscorlib: $r"
    $log = Get-Content (Join-Path $stage 'xenia.log')
    $compiled = ($log | Select-String -Pattern 'xclrjit: compiled').Count
    $declines = $log | Select-String -Pattern 'xclrjit: declined' | ForEach-Object { $_.Line -replace '.*xclrjit: declined ', '' }
    "   compiled $compiled, declined $($declines.Count)"
    $declines | ForEach-Object { ($_ -replace '.*: ', '') -replace ' at IL.*', '' } | Group-Object | Sort-Object Count -Descending |
        Select-Object -First 25 | ForEach-Object { "   {0,5} {1}" -f $_.Count, $_.Name }
    # Written even when empty (an empty pipeline leaves the old file in place).
    Set-Content (Join-Path $Ours 'mscorlib-declines.txt') -Value @($declines)
    $image = Join-Path $vimeo 'mscorlib.ni.dll'
    if ((Get-Item $image).Length -ne (Get-Item $microsoft).Length) {
        Copy-Item $image (Join-Path $Ours 'mscorlib.ni.dll') -Force
        # The zapper's own output (based at 0, with .reloc and .pdata), for tools that read the raw image.
        Copy-Item (Join-Path $stage 'mscorlib.ni.dll') (Join-Path $Ours 'mscorlib.ni.raw.pe') -Force
        Copy-Item $microsoft $image -Force
        "   image saved to $Ours"
    } else {
        "   no image written"
        return
    }
}

# 2. The platform and NGenBench against it.
Copy-Item (Join-Path $Ours 'mscorlib.ni.dll') (Join-Path $bench 'mscorlib.ni.dll') -Force
foreach ($name in $platform + @('NGenBench')) {
    $ni = Join-Path $bench "$(Image-Name $name).ni.dll"
    if (Test-Path $ni) { [IO.File]::Delete($ni) }
}
$next = '96200000'
if (-not $SkipPlatform) {
    $next = & (Join-Path $PSScriptRoot '..\tools\NGen-Platform.ps1') -Package $bench -Assemblies $platform -Reference $aside -Seconds $Seconds -Jit $Jit |
        Select-Object -Last 1
    $saved = Join-Path $Ours 'platform-ours'
    if (-not (Test-Path $saved)) { $null = New-Item -ItemType Directory $saved }
    foreach ($name in $platform) {
        $ni = Join-Path $bench "$(Image-Name $name).ni.dll"
        if (Test-Path $ni) { Copy-Item $ni $saved -Force } else { Write-Warning "${name}: no image" }
    }
} else {
    foreach ($name in $platform) { Copy-Item (Join-Path $Ours "platform-ours\$(Image-Name $name).ni.dll") $bench -Force }
    $next = & (Join-Path $PSScriptRoot '..\tools\NGen-Platform.ps1') -Package $bench -Assemblies $platform -Reference $aside -Plan | Select-Object -Last 1
}
$r = & (Join-Path $PSScriptRoot '..\tools\NGen-Package.ps1') -Package $bench -Assemblies NGenBench -BaseAddress $next -Seconds $Seconds -Jit $Jit 2>&1 |
    Out-String -Stream | Select-String -Pattern 'hr 0x|WARNING|compiled [0-9]+' | Select-Object -Last 1
"2. NGenBench at ${next}: $r"
$declined = Select-String -Path (Join-Path $stage 'xenia.log') -Pattern 'xclrjit: declined' | ForEach-Object { $_.Line -replace '.*xclrjit: declined ', '' }
"   NGenBench declines: $($declined.Count)"; $declined | ForEach-Object { "     $_" }

# 3. NGenBench on our images.
$r = & (Join-Path $PSScriptRoot '..\tools\Run-App.ps1') -Xex (Join-Path $bench 'default.xex') -DonePattern 'NGenBench: [0-9]+ passed' 2>&1 |
    Out-String -Stream | Select-String -Pattern 'NGenBench: [0-9]+ passed|crash|idle|error dialog' | Select-Object -Last 1
"3. NGenBench on our images: $r"
Select-String -Path (Join-Path $bench 'xenia.log') -Pattern 'faulted at|No guest code|DbgBreakPoint from|Host-side crash' |
    Select-Object -First 3 | ForEach-Object { "   " + $_.Line.Substring([Math]::Min(25, $_.Line.Length)) }
Select-String -Path (Join-Path $bench 'xenia.log') -Pattern '^.*r24=' | Select-Object -First 2 | ForEach-Object { "   " + $_.Line }
