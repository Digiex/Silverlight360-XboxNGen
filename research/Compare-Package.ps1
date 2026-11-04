<#
.SYNOPSIS
Compiles a package's assemblies with our JIT, one zapper run each, and compares each image with the one Microsoft
shipped in the package.

.DESCRIPTION
Works on a copy of the package (-Work\pkg), so the package itself is untouched. For each assembly that has both an IL
file and a native image: removes our earlier image from the copy, runs NGen-Package.ps1 for that assembly alone (the
other assemblies keep Microsoft's images, which the zapper loads as dependencies), moves our image to -Work\ours and
puts Microsoft's back, extracts both images' memory layout with xex.py and runs nicompare.py with the method names from
methodnames.py. Writes <name>.compare.txt per assembly and summary.txt with the category tallies, into -Work.

.EXAMPLE
.\Compare-Package.ps1 -Package <apps folder>\Plex -Work <work folder>\cmp\Plex -Assemblies System.Net,System.Core
#>
param(
    [Parameter(Mandatory)] [string]$Package,
    [Parameter(Mandatory)] [string]$Work,
    # Assembly simple names; default: every assembly of the package with both an IL file and a native image.
    [string[]]$Assemblies = @(),
    [int]$Seconds = 3600
)
$ErrorActionPreference = 'Stop'
$Package = (Resolve-Path $Package).Path
$tools = $PSScriptRoot
$ngen = Join-Path $PSScriptRoot '..\tools\NGen-Package.ps1'
New-Item -ItemType Directory -Force $Work | Out-Null
$Work = (Resolve-Path $Work).Path
$pkg = Join-Path $Work 'pkg'
$ours = Join-Path $Work 'ours'
$ms = Join-Path $Work 'ms'
foreach ($d in $pkg, $ours, $ms) { New-Item -ItemType Directory -Force $d | Out-Null }
if (-not (Test-Path (Join-Path $pkg 'default.xex'))) { Copy-Item (Join-Path $Package '*') $pkg -Recurse -Force }
# One JIT for the whole batch, whatever is rebuilt in jit\ meanwhile.
# The JIT is copied into the work folder (one for every assembly of a run), refreshed when a newer one has been built.
$jit = Join-Path $Work 'xclrjit.dll'
$built = Join-Path (Split-Path $tools -Parent) 'jit\xclrjit.dll'
if (-not (Test-Path $jit) -or (Get-Item $built).LastWriteTime -gt (Get-Item $jit).LastWriteTime) { Copy-Item $built $jit -Force }

if (-not $Assemblies) {
    $Assemblies = Get-ChildItem $Package -Filter '*.ni.dll' | ForEach-Object { $_.Name -replace '\.ni\.dll$', '' } |
        Where-Object { Test-Path (Join-Path $Package "$_.dll") }
}

$summary = Join-Path $Work 'summary.txt'
foreach ($name in $Assemblies) {
    Write-Host "=== $name"
    $theirs = Join-Path $Package "$name.ni.dll"
    $mine = Join-Path $ours "$name.ni.dll"
    if (-not (Test-Path $mine)) {
        Copy-Item $theirs $pkg -Force
        $out = & $ngen -Package $pkg -Assemblies $name -Seconds $Seconds -BaseAddress '95000000' -Jit $jit *>&1
        $out | Where-Object { $_ -match 'ngenhost: D:|WARNING|methods compiled' } | ForEach-Object { Write-Host $_ }
        Copy-Item (Join-Path $env:TEMP 'xboxngen-stage\xenia.log') (Join-Path $Work "$name.log") -Force
        $made = Join-Path $pkg "$name.ni.dll"
        if ((Get-Item $made).Length -eq (Get-Item $theirs).Length -and (Get-FileHash $made).Hash -eq (Get-FileHash $theirs).Hash) {
            Write-Host "  no image of ours for $name"
            continue
        }
        Move-Item $made $mine -Force
        Copy-Item $theirs $pkg -Force
    }
    $mspe = Join-Path $ms "$name.pe"
    $ourpe = Join-Path $ours "$name.pe"
    # Extracted again whenever the image is newer: a recompiled image was compared through its old extraction.
    if (-not (Test-Path $mspe)) { & python (Join-Path $tools 'xex.py') -b $mspe $theirs | Out-Null }
    if (-not (Test-Path $ourpe) -or (Get-Item $mine).LastWriteTime -gt (Get-Item $ourpe).LastWriteTime) {
        & python (Join-Path $tools 'xex.py') -b $ourpe $mine | Out-Null
    }
    $names = Join-Path $Work "$name.methods.txt"
    & python (Join-Path $tools 'methodnames.py') (Join-Path $Package "$name.dll") | Set-Content $names -Encoding utf8
    $bases = & python -c "import sys; sys.path.insert(0, r'$tools'); from pe import PE; print('%x %x' % (PE(sys.argv[1]).base, PE(sys.argv[2]).base))" $mspe $ourpe
    $msbase, $ourbase = $bases.Split(' ')
    $report = Join-Path $Work "$name.compare.txt"
    & python (Join-Path $tools 'nicompare.py') $mspe $msbase $ourpe $ourbase (Join-Path $Work "$name.log") $name $names -v | Set-Content $report -Encoding utf8
    Add-Content $summary "=================== $name"
    Get-Content $report | Where-Object { $_ -match '^pairing|^ms bodies|^ {4,5}\d+: ' } | Add-Content $summary
}
Write-Host "summary: $summary"
