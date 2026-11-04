<#
.SYNOPSIS
Makes native images (.ni.dll) for a Silverlight360 / Xbox 360 Silverlight package with xcoreclr's own zapper.

.DESCRIPTION
1. Copies the package to a staging folder, adds the JIT (jit\xclrjit.dll) and makes the zapper host
   (host\ngenhost.xex) its default.xex, with ngen.txt listing the assemblies to compile.
2. Runs the staging folder in xenia-edge. The host calls xcoreclr's NGenWorker for each assembly; the zapper loads
   xclrjit.dll and writes <name>.ni.dll (a PE based at 0) beside the assembly.
3. Rebases each PE to a free load address and wraps it in the XEX module xcoreclr loads (tools\pe2nixex.py), with
   the package's title, then copies it into the package.

The images are not signed: they load in xenia-edge, not on a retail console.

.EXAMPLE
.\NGen-Package.ps1 -Package ..\test\NGenProbe\bin\Release\xbox -Assemblies NGenProbe
#>
param(
    [Parameter(Mandatory)] [string]$Package,
    # Assembly simple names; default: the AppManifest.xaml parts that have no native image yet.
    [string[]]$Assemblies = @(),
    # First load address; each image takes the next 64 KB-aligned slot. Plex's own images end at 0x94DD0000.
    [string]$BaseAddress = '95000000',
    [string]$Stage = (Join-Path $env:TEMP 'xboxngen-stage'),
    [int]$Seconds = 300,
    [string]$Xenia = (& (Join-Path $PSScriptRoot 'Config.ps1') Xenia),
    # The JIT to use; default: the one built in jit\.
    [string]$Jit = '',
    # More xenia-edge options, for diagnosis (--log_lr_at_instruction=<address> names a code address's callers).
    [string[]]$ExtraArgs = @()
)
$ErrorActionPreference = 'Stop'
$root = Split-Path $PSScriptRoot -Parent
$Package = (Resolve-Path $Package).Path
$jit = if ($Jit) { (Resolve-Path $Jit).Path } else { Join-Path $root 'jit\xclrjit.dll' }
$host_ = Join-Path $root 'host\ngenhost.xex'
# The module xcoreclr takes executable memory for its stubs from (codecache\codecache.c).
$codecache = Join-Path $root 'codecache\CLRCodeCachedll_0.xex'
foreach ($f in $jit, $host_) { if (-not (Test-Path $f)) { throw "$f is missing: build it with make in WSL" } }

if (-not $Assemblies) {
    [xml]$manifest = Get-Content (Join-Path $Package 'AppManifest.xaml')
    $Assemblies = $manifest.Deployment.'Deployment.Parts'.AssemblyPart | ForEach-Object { [IO.Path]::GetFileNameWithoutExtension($_.Source) } |
        Where-Object { -not (Test-Path (Join-Path $Package "$_.ni.dll")) }
}
if (-not $Assemblies) { Write-Host 'Nothing to compile.'; return }
# xcoreclr's own table (in its .rdata) of image file names shorter than the console's file system allows.
$aliases = @{ 'System.Windows.Controls.Navigation' = 'SW.Controls.Navigation'; 'Microsoft.Web.Media.SmoothStreaming' = 'MWM.SmoothStreaming';
    'Microsoft.Xna.Framework' = 'MXF' }
function Image-Name($name) { if ($aliases.ContainsKey($name)) { $aliases[$name] } else { $name } }

if (Test-Path $Stage) { Remove-Item $Stage -Recurse -Force }
Copy-Item $Package $Stage -Recurse
# An image already beside an assembly makes the zapper report success without compiling it.
foreach ($name in $Assemblies) { Remove-Item (Join-Path $Stage "$(Image-Name $name).ni.dll") -ErrorAction SilentlyContinue }
Copy-Item $jit (Join-Path $Stage 'xclrjit.dll') -Force
# The runtime interprets what the zapper process itself runs (tools\xmrun.xex.config), as on a console.
Copy-Item (Join-Path $PSScriptRoot 'xmrun.xex.config') $Stage -Force
Copy-Item $codecache $Stage -Force
Copy-Item $host_ (Join-Path $Stage 'default.xex') -Force
($Assemblies | ForEach-Object { "D:\$_.dll" }) -join "`r`n" | Set-Content (Join-Path $Stage 'ngen.txt') -Encoding ascii
Set-Content (Join-Path $Stage 'dbgprint.txt') '' -Encoding ascii

# As a devkit: with xbdm.xex loaded, xcoreclr generates the stubs the zapper asks it for (an array's accessors, a
# delegate's thunk) in the code cache module; on a retail console it refuses and the zapper fails the image.
$output = & (Join-Path $PSScriptRoot 'Run-Xenia.ps1') -Stage $Stage -Seconds $Seconds -Xenia $Xenia -ExtraArgs (@('--console_type=0', '--writable_code_segments=true') + $ExtraArgs)
$output | Where-Object { $_ -match '^(ngenhost|xclrjit: (compiled|\d+ methods))' } | ForEach-Object { Write-Host $_ }

$base = [Convert]::ToUInt32($BaseAddress, 16)
$template = Join-Path $Package 'default.xex'
foreach ($name in $Assemblies) {
    $pe = Join-Path $Stage "$name.ni.dll"
    if (-not (Test-Path $pe)) { $pe = Join-Path $Stage "$(Image-Name $name).ni.dll" }
    if (-not (Test-Path $pe)) { Write-Warning "${name}: the zapper wrote no native image"; continue }
    $out = Join-Path $Package "$(Image-Name $name).ni.dll"
    & python -I (Join-Path $PSScriptRoot 'pe2nixex.py') $pe $out --template $template --base ('{0:X8}' -f $base) --name "$name.ni.dll"
    if ($LASTEXITCODE) { throw "pe2nixex failed for $name" }
    Write-Host ('{0}.ni.dll at {1:X8}' -f (Image-Name $name), $base)
    # The next image starts on the next 64 KB boundary after this one's PE image size.
    $bytes = [IO.File]::ReadAllBytes($pe)
    $e = [BitConverter]::ToInt32($bytes, 0x3C)
    $size = [BitConverter]::ToUInt32($bytes, $e + 24 + 56)
    $base = [uint32](($base + $size + 0x1FFFF) -band 0xFFFF0000)
}
