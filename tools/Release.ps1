<#
.SYNOPSIS
Builds the JIT, the host and the code cache module, tests them, and publishes them on GitHub Releases.

.DESCRIPTION
As Silverdash360's tools\Release.ps1 does:
1. The release is made from a commit as it is, which GitHub has: nothing may be uncommitted, and the commit must be
   pushed already. Nothing is pushed.
2. The version is -Version if given, else the last release's (the highest v<version> tag) with its patch (or -Bump's
   part) raised; the first release is 0.1.0. It is tagged v<version> on the commit, by gh.
3. jit\, host\ and codecache\ are built from scratch in WSL (make clean, then make), with the xsdk config.mk names.
4. test\Test-Mscorlib.ps1 runs on the new JIT; the release stops unless NGenBench passes with 0 failed. -SkipTest
   skips it.
5. <work folder>\release\XboxNGen-<version>.zip has, in an XboxNGen folder, the tracked tools\ (but this script) and docs\, README.md,
   LICENSE.md, the three binaries in their places (so tools\NGen-Package.ps1 runs from it without xsdk) and
   BUILD.txt: what was built from what, and how to use it.
6. The release's notes are the commits since the last release, and the zip is its file.
Needs the GitHub CLI (gh), signed in, unless -DryRun.

.EXAMPLE
.\tools\Release.ps1 -DryRun -SkipTest

Shows the version, builds and zips, and shows the notes, without publishing.

.EXAMPLE
.\tools\Release.ps1 -Bump Minor
#>
param(
    # The version to release, when it isn't the last one's raised.
    [ValidatePattern('^\d+\.\d+\.\d+$')] [string]$Version = '',
    # The part of the last release's version to raise.
    [ValidateSet('Patch', 'Minor', 'Major')] [string]$Bump = 'Patch',
    # Releases without running NGenBench on the new JIT.
    [switch]$SkipTest,
    # Shows what would be released, without publishing.
    [switch]$DryRun,
    [switch]$Prerelease
)
$ErrorActionPreference = 'Stop'
$root = [IO.Path]::GetFullPath((Join-Path $PSScriptRoot '..'))
$repository = 'Digiex/Silverlight360-XboxNGen'
$binaries = 'jit\xclrjit.dll', 'host\ngenhost.xex', 'codecache\CLRCodeCachedll_0.xex'

# UTF-8 without a byte order mark, which GitHub would show in the notes.
function Write-Utf8([string]$path, [string[]]$lines) {
    [IO.File]::WriteAllLines($path, $lines, (New-Object Text.UTF8Encoding $false))
}
function Invoke-Git { $output = & git.exe -C $root @args; if ($LASTEXITCODE) { throw "git $args failed" }; $output }

# 1. The commit.
if (Invoke-Git status --porcelain --untracked-files=no) { throw 'There are changes not committed. Commit them, or stash them, first.' }
Invoke-Git fetch --quiet --tags origin
$commit = Invoke-Git rev-parse HEAD
if (-not $DryRun) {
    if (-not (Get-Command gh -ErrorAction SilentlyContinue)) { throw 'The GitHub CLI (gh) is needed to publish: https://cli.github.com' }
    if (-not (Invoke-Git branch -r --contains $commit)) { throw "Commit $commit isn't on GitHub yet. Push it first." }
}
$xsdk = & (Join-Path $PSScriptRoot 'Config.ps1') Xsdk
if (-not $xsdk -or -not (Test-Path (Join-Path $root 'config.mk'))) { throw 'No xsdk is configured: run tools\Configure.ps1.' }

# 2. The version.
$released = @(foreach ($t in (Invoke-Git tag --list 'v*')) {
    $parsed = $null
    if ([version]::TryParse($t.Substring(1), [ref]$parsed)) { [pscustomobject]@{ Tag = $t; Version = $parsed } }
})
$last = $released | Sort-Object Version -Descending | Select-Object -First 1
if (-not $Version) {
    $Version = if (-not $last) { '0.1.0' } else {
        $v = $last.Version
        switch ($Bump) {
            'Major' { "$($v.Major + 1).0.0" }
            'Minor' { "$($v.Major).$($v.Minor + 1).0" }
            default { "$($v.Major).$($v.Minor).$([Math]::Max($v.Build, 0) + 1)" }
        }
    }
} elseif ($last -and [version]$Version -le $last.Version) { throw "$Version isn't higher than $($last.Tag)." }
$tag = "v$Version"
if ($released | Where-Object { $_.Tag -eq $tag }) { throw "$tag is released already." }
Write-Host ("Version $Version" + $(if ($last) { ", after $($last.Tag)" } else { ', the first release' }))

# 3. The binaries, from scratch.
wsl --cd $root -e bash -lc 'set -e; for d in jit host codecache; do make -C $d clean; make -C $d; done'
if ($LASTEXITCODE) { throw 'The build failed.' }
foreach ($b in $binaries) { if (-not (Test-Path (Join-Path $root $b))) { throw "The build made no $b." } }
$toolchain = wsl -e bash -lc '${TOOLCHAIN:-$HOME/xsdk-gcc/bin/powerpc-xsdk-elf-}gcc --version | head -n 1'
$xsdkCommit = (& git.exe -C $xsdk rev-parse --short=12 HEAD), (& git.exe -C $xsdk rev-parse --abbrev-ref HEAD)

# 4. NGenBench on the new JIT.
$tested = ''
if (-not $SkipTest) {
    & (Join-Path $root 'test\Test-Mscorlib.ps1') -Jit (Join-Path $root 'jit\xclrjit.dll') | Tee-Object -Variable lines | Out-Host
    $summary = $lines | Select-String -Pattern 'NGenBench: [0-9]+ passed, [0-9]+ failed' | Select-Object -Last 1
    if (-not $summary -or $summary.Matches[0].Value -notmatch ', 0 failed$') {
        throw "NGenBench didn't pass on the new JIT. (It hangs at PInvokeUnderGC now and then: run it again.)"
    }
    $tested = $summary.Matches[0].Value -replace '^NGenBench: ', ''
}

# 5. The zip.
$name = "XboxNGen $Version"
$out = Join-Path (& (Join-Path $PSScriptRoot 'Config.ps1') Work) 'release'
$staging = Join-Path $out 'XboxNGen'
if (Test-Path -LiteralPath $out) { Remove-Item -LiteralPath $out -Recurse -Force }
foreach ($f in @(Invoke-Git ls-files tools docs README.md LICENSE.md | Where-Object { $_ -ne 'tools/Release.ps1' }) + $binaries) {
    $to = Join-Path $staging $f
    $null = New-Item -ItemType Directory (Split-Path $to -Parent) -Force
    Copy-Item -LiteralPath (Join-Path $root $f) -Destination $to
}
$build = @"
$name
https://github.com/$repository

Built from commit $($commit.Substring(0, 12)), with xsdk $($xsdkCommit[0]) ($($xsdkCommit[1])) and $toolchain.
$(if ($tested) { "NGenBench on the JIT's images of mscorlib and the platform: $tested." } else { 'Not tested.' })

jit\xclrjit.dll, host\ngenhost.xex and codecache\CLRCodeCachedll_0.xex are built, so making native images needs
only xenia-edge (README.md, "Making native images"), not xsdk:
1. Extract xenia_edge_windows.zip from
   https://github.com/jessenic/xenia-edge/releases/tag/silverlight-build-20261105
   to xenia-edge\ beside this folder.
2. .\tools\Configure.ps1, leaving xsdk unset.
3. .\tools\NGen-Package.ps1 -Package <package folder>
The images it makes are unsigned: they run in xenia-edge and on a console that runs unsigned code.
"@
Write-Utf8 (Join-Path $staging 'BUILD.txt') ($build -split "`r?`n")
$zip = Join-Path $out "XboxNGen-$Version.zip"
Compress-Archive -Path $staging -DestinationPath $zip

# 6. The notes: the commits since the last release before this commit.
$previous = $null
foreach ($candidate in ($released | Sort-Object Version -Descending)) {
    & git.exe -C $root merge-base --is-ancestor $candidate.Tag $commit
    if ($LASTEXITCODE -eq 0) { $previous = $candidate.Tag; break }
}
$range = if ($previous) { "$previous..$commit" } else { $commit }
$notes = @(if ($previous) { "## Changes since $previous" } else { '## Changes' }) + ''
$notes += Invoke-Git log $range --no-merges --format='- %s (%h)'
$notes += '', ("From $($commit.Substring(0, 7))" + $(if ($tested) { "; NGenBench: $tested" }) + '. The zip has the tools ' +
    'with the JIT, the zapper host and the code cache module built: see BUILD.txt in it.')
$notesFile = Join-Path $out 'notes.md'
Write-Utf8 $notesFile $notes

Write-Host "$name ($tag)"
Write-Host "Zip: $zip"
Write-Host ''
Get-Content -LiteralPath $notesFile | Write-Host
if ($DryRun) { Write-Host ''; Write-Host 'Dry run: nothing published.'; return }

$arguments = @('release', 'create', $tag, $zip, '--repo', $repository, '--target', $commit, '--title', $name,
    '--notes-file', $notesFile)
if ($Prerelease) { $arguments += '--prerelease' }
& gh @arguments
if ($LASTEXITCODE) { throw 'gh release create failed.' }
Invoke-Git fetch --quiet --tags origin
