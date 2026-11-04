<#
.SYNOPSIS
Compiles a package's platform assemblies (System, System.Windows, Microsoft.Xbox.* ...) against the mscorlib.ni.dll in
the package, one NGen-Package run per assembly, at load addresses that follow each other.

.DESCRIPTION
One run per assembly: the zapper finds the raw image of an assembly compiled earlier in the same run in its stage,
which xenia can't load, and the dependents fail with 0x80070002. Each assembly gets a slot of twice the size of the
reference image (-Reference: a folder holding Microsoft's .ni.dll files, for sizing) rounded up to 1 MB, or
-DefaultSlot without one. Dependencies first: the default order is the one the Silverlight platform needs.
#>
param(
    [Parameter(Mandatory)] [string]$Package,
    [string[]]$Assemblies = @('System', 'System.Core', 'System.Xml', 'System.Net', 'System.Windows',
        'System.Windows.RuntimeHost', 'System.Xml.Linq', 'System.Windows.Interactivity',
        'System.Windows.Controls.Navigation', 'Microsoft.Web.Media.SmoothStreaming', 'Microsoft.Xbox.Core',
        'Microsoft.Xbox', 'Microsoft.Xbox.Services', 'Microsoft.Xbox.Storage'),
    [string]$BaseAddress = '96200000',
    [string]$Reference = '',
    [uint64]$DefaultSlot = 0x800000,
    [int]$Seconds = 3000,
    # The JIT to use (NGen-Package.ps1's default: the one built in jit\).
    [string]$Jit = '',
    # Only compute the load addresses (the result is the next free one); compile nothing.
    [switch]$Plan
)
$ErrorActionPreference = 'Stop'
# The image file names xcoreclr shortens (NGen-Package.ps1).
$aliases = @{ 'System.Windows.Controls.Navigation' = 'SW.Controls.Navigation'; 'Microsoft.Web.Media.SmoothStreaming' = 'MWM.SmoothStreaming' }
[uint64]$base = [Convert]::ToUInt64($BaseAddress, 16)
foreach ($name in $Assemblies) {
    if (-not (Test-Path (Join-Path $Package "$name.dll"))) { Write-Warning "${name}: no $name.dll in the package"; continue }
    $image = if ($aliases.ContainsKey($name)) { $aliases[$name] } else { $name }
    [uint64]$slot = $DefaultSlot
    if ($Reference -and (Test-Path (Join-Path $Reference "$image.ni.dll"))) {
        $slot = ((((Get-Item (Join-Path $Reference "$image.ni.dll")).Length * 2) + 0xFFFFF) -band 0xFFF00000)
    }
    $hex = $base.ToString('X8')
    if ($Plan) { $base = $base + $slot; continue }
    Write-Host "== $name at $hex (slot $($slot.ToString('X')))"
    & (Join-Path $PSScriptRoot 'NGen-Package.ps1') -Package $Package -Assemblies @($name) -BaseAddress $hex -Seconds $Seconds -Jit $Jit 2>&1 |
        Where-Object { $_ -match 'hr 0x|WARNING|\.ni\.dll at |compiled [0-9]+' } | ForEach-Object { Write-Host "   $_" }
    $base = $base + $slot
}
# The next free load address, for the app's own assemblies.
$base.ToString('X8')
