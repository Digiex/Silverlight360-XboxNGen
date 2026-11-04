<#
.SYNOPSIS
Runs an app package on our native images alone: a copy of the package gets our mscorlib and platform images (from
<work folder>\ours and its platform-ours, as Test-Mscorlib.ps1 leaves them), every other native image
is removed, the app's own assemblies are compiled against them (one NGen-Package run each, at the address after the
platform's), and Run-App.ps1 runs the result.

The images bind to the IL they were compiled from, by MVID, and Microsoft rebuilt the framework for every app (IGN's,
Crackle's and Netflix's System.Windows have one version and size and three MVIDs). So a package whose platform IL
isn't the set's (-SetIL) has the whole platform compiled in it, against the set's mscorlib (NGen-Platform.ps1). Its
mscorlib.dll has to be the set's: a package of another runtime build needs a set of its own (-Ours).

.EXAMPLE
.\Run-OnOurs.ps1 -Package <Silverlight360>\samples\HelloXbox\bin\Release\xbox -IdleSeconds 30
#>
param(
    [Parameter(Mandatory)] [string]$Package,
    [string]$Work = '',
    # The work folder (README.md, "Configuration").
    [string]$Ours = (Join-Path (& (Join-Path $PSScriptRoot '..\tools\Config.ps1') Work) 'ours'),
    # The app's assemblies to compile; default: the AppManifest.xaml parts.
    [string[]]$Assemblies = @(),
    # Platform assemblies to run without a native image (the runtime compiles them at run time with xclrjit.dll,
    # copied into the package): tells a fault of the compiled code from one of the image.
    [string[]]$WithoutImages = @(),
    [string]$DonePattern = '',
    [int]$IdleSeconds = 30,
    [int]$Seconds = 3000,
    # The folder holding the IL the set's images were compiled from: $Ours\il when there is one, else NGenBench's
    # package (Plex's files).
    [string]$SetIL = ''
)
$ErrorActionPreference = 'Stop'
if (-not $SetIL) {
    $SetIL = if (Test-Path (Join-Path $Ours 'il')) { Join-Path $Ours 'il' } else { Join-Path $PSScriptRoot 'NGenBench\bin\Release\xbox' }
}
$platformNames = @('System', 'System.Core', 'System.Xml', 'System.Net', 'System.Windows', 'System.Windows.RuntimeHost',
    'System.Xml.Linq', 'System.Windows.Interactivity', 'System.Windows.Controls.Navigation',
    'Microsoft.Web.Media.SmoothStreaming', 'Microsoft.Xbox.Core', 'Microsoft.Xbox', 'Microsoft.Xbox.Services',
    'Microsoft.Xbox.Storage')
function Same-File([string]$a, [string]$b) {
    (Test-Path $a) -and (Test-Path $b) -and (Get-FileHash $a -Algorithm MD5).Hash -eq (Get-FileHash $b -Algorithm MD5).Hash
}
$Package = (Resolve-Path $Package).Path
$name = Split-Path (Split-Path (Split-Path (Split-Path $Package -Parent) -Parent) -Parent) -Leaf
if (-not $Work) { $Work = Join-Path $Ours "..\apps\$name" }
if (Test-Path $Work) { Remove-Item $Work -Recurse -Force }
Copy-Item $Package $Work -Recurse
$Work = (Resolve-Path $Work).Path
Get-ChildItem $Work -Filter '*.ni.dll' | ForEach-Object { [IO.File]::Delete($_.FullName) }
if (-not (Same-File (Join-Path $Work 'mscorlib.dll') (Join-Path $SetIL 'mscorlib.dll'))) {
    throw "the package's mscorlib.dll isn't the one the set in $Ours was compiled from ($SetIL): it needs a set for its runtime build"
}
Copy-Item (Join-Path $Ours 'mscorlib.ni.dll') $Work -Force
$present = @($platformNames | Where-Object { Test-Path (Join-Path $Work "$_.dll") })
$own = @($present | Where-Object { -not (Same-File (Join-Path $Work "$_.dll") (Join-Path $SetIL "$_.dll")) })
if ($own) {
    # The set's images of the others bind to the set's builds of these, so the whole platform is compiled here.
    Write-Host "== the package's own platform build ($($own -join ', ')): compiling the platform"
    & (Join-Path $PSScriptRoot '..\tools\NGen-Platform.ps1') -Package $Work -Assemblies $present -Reference (Join-Path $Ours 'bench-ni-aside') -Seconds $Seconds 2>&1 |
        Where-Object { $_ -match '^== |hr 0x|WARNING|wrote no' } | ForEach-Object { Write-Host "   $_" }
} else {
    foreach ($ni in Get-ChildItem (Join-Path $Ours 'platform-ours') -Filter '*.ni.dll') {
        # Only the images of assemblies the package has (a package without SmoothStreaming doesn't get its image).
        $il = $ni.Name -replace '\.ni\.dll$', '.dll'
        $aliases = @{ 'SW.Controls.Navigation.dll' = 'System.Windows.Controls.Navigation.dll'; 'MWM.SmoothStreaming.dll' = 'Microsoft.Web.Media.SmoothStreaming.dll' }
        if ($aliases.ContainsKey($il)) { $il = $aliases[$il] }
        if (Test-Path (Join-Path $Work $il)) { Copy-Item $ni.FullName $Work -Force }
    }
}
if ($WithoutImages) {
    $aliases2 = @{ 'System.Windows.Controls.Navigation' = 'SW.Controls.Navigation'; 'Microsoft.Web.Media.SmoothStreaming' = 'MWM.SmoothStreaming' }
    foreach ($n in $WithoutImages) {
        $image = if ($aliases2.ContainsKey($n)) { $aliases2[$n] } else { $n }
        $ni = Join-Path $Work "$image.ni.dll"
        if (Test-Path $ni) { [IO.File]::Delete($ni); Write-Host "== $n without its image" }
    }
    Copy-Item (Join-Path (Split-Path $PSScriptRoot -Parent) 'jit\xclrjit.dll') $Work -Force
}
if (-not $Assemblies) {
    [xml]$manifest = Get-Content (Join-Path $Work 'AppManifest.xaml')
    # The manifest's parts without an image yet (the platform assemblies some samples list have ours already), and
    # with a file: 45 shipped apps list System.Xml.Serialization and Microsoft.Xbox.RuntimeHelpers, which the binder
    # unifies with System.Xml and mscorlib and no package carries.
    $short = @{ 'System.Windows.Controls.Navigation' = 'SW.Controls.Navigation'; 'Microsoft.Web.Media.SmoothStreaming' = 'MWM.SmoothStreaming' }
    $Assemblies = $manifest.Deployment.'Deployment.Parts'.AssemblyPart | ForEach-Object { [IO.Path]::GetFileNameWithoutExtension($_.Source) } |
        Where-Object { $i = if ($short.ContainsKey($_)) { $short[$_] } else { $_ }; -not (Test-Path (Join-Path $Work "$i.ni.dll")) -and (Test-Path (Join-Path $Work "$_.dll")) }
}
$next = & (Join-Path $PSScriptRoot '..\tools\NGen-Platform.ps1') -Package $Work -Reference (Join-Path $Ours 'bench-ni-aside') -Plan | Select-Object -Last 1
foreach ($asm in $Assemblies) {
    Write-Host "== $asm at $next"
    & (Join-Path $PSScriptRoot '..\tools\NGen-Package.ps1') -Package $Work -Assemblies @($asm) -BaseAddress $next -Seconds $Seconds 2>&1 |
        Where-Object { $_ -match 'hr 0x|WARNING|\.ni\.dll at |compiled [0-9]+|declined' } | ForEach-Object { Write-Host "   $_" }
    $next = ([Convert]::ToUInt64($next, 16) + 0x400000).ToString('X8')
}
# The SDK samples (HelloXbox and the ui\ ones) log "<name>: ready" thirty frames in and keep running after it, so the
# log never goes quiet; other apps go by -IdleSeconds.
$ready = @('HelloXbox', 'XboxControls', 'PageNavigation', 'FocusNavigation')
if (-not $DonePattern -and $ready -contains $name) { $DonePattern = "${name}: ready" }
& (Join-Path $PSScriptRoot '..\tools\Run-App.ps1') -Xex (Join-Path $Work 'default.xex') -DonePattern $DonePattern -IdleSeconds $IdleSeconds
