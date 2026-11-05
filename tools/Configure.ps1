<#
.SYNOPSIS
Records where this machine keeps what XboxNGen's tools need, once, for every later run.

.DESCRIPTION
Finds each location from its parameter, else its environment variable, else the place a checkout usually is (beside
this repository), and asks for any it can't find unless -NonInteractive. With no xenia_edge.exe, it offers to download
xenia-edge's silverlight-build release (README.md) beside this repository; -DownloadXenia does without asking. Writes, at the repository's root (git ignores
them):
  config.json   the locations, for the PowerShell tools (tools\Config.ps1 reads it; an environment variable still wins)
  config.mk     XSDK, as WSL sees it, for make in jit\, host\ and codecache\
  config.props  Silverlight360Dir, for the test apps' MSBuild projects
Run it again to change a location; -Show prints what is configured.

Making native images (tools\) needs:
  Xenia           xenia_edge.exe (xenia-edge's silverlight-build release, or a build of that branch)
  Xsdk            an xsdk checkout with libxsdk.a built, which the JIT, the host and the code cache link
  Work            the work folder: our images, staged packages, timings (default: work\ in this repository)
Testing the JIT (test\) also needs:
  Silverlight360  a Silverlight360 SDK checkout, which builds the test apps
  Apps            the folder of extracted app packages (Vimeo\, Plex\, ...): Test-Mscorlib.ps1, and research\'s
                  comparisons. Never asked for: give it with -Apps, or it is found beside this repository.
Work is never asked for either; its default serves.

.EXAMPLE
.\tools\Configure.ps1
.\tools\Configure.ps1 -Xenia D:\src\xenia-edge\build\bin\Windows\Release\xenia_edge.exe -Apps D:\xbox\apps
#>
param(
    [string]$Xenia = '',
    [string]$Work = '',
    [string]$Apps = '',
    [string]$Silverlight360 = '',
    [string]$Xsdk = '',
    # Downloads xenia-edge's release without asking when no xenia_edge.exe is found (as with -NonInteractive).
    [switch]$DownloadXenia,
    [switch]$NonInteractive,
    [switch]$Show
)
$ErrorActionPreference = 'Stop'
$root = [IO.Path]::GetFullPath((Join-Path $PSScriptRoot '..'))
$parent = Split-Path $root -Parent
$jsonFile = Join-Path $root 'config.json'

if ($Show) {
    foreach ($name in 'Xenia', 'Work', 'Apps', 'Silverlight360', 'Xsdk') {
        '{0,-15} {1}' -f $name, (& (Join-Path $PSScriptRoot 'Config.ps1') $name)
    }
    return
}

$previous = if (Test-Path $jsonFile) { Get-Content $jsonFile -Raw | ConvertFrom-Json } else { $null }

# The xenia-edge build README.md names: the silverlight-build branch's Windows release.
$xeniaRepository = 'jessenic/xenia-edge'
$xeniaTag = 'silverlight-build-20261105'
$xeniaAsset = 'xenia_edge_windows.zip'
# Where it goes: xenia-edge\ beside this repository, unless that holds something else (a checkout, say).
$xeniaFolder = "$parent\xenia-edge"
if (Get-ChildItem $xeniaFolder -Force -ErrorAction SilentlyContinue | Select-Object -First 1) { $xeniaFolder = "$parent\xenia-edge-$xeniaTag" }

# Downloads the release, checks it against the SHA-256 GitHub gives for it, extracts it and gives its xenia_edge.exe.
function Get-XeniaRelease {
    $agent = 'XboxNGen-Configure'
    $release = Invoke-RestMethod "https://api.github.com/repos/$xeniaRepository/releases/tags/$xeniaTag" -UserAgent $agent
    $asset = $release.assets | Where-Object { $_.name -eq $xeniaAsset } | Select-Object -First 1
    if (-not $asset) { throw "$xeniaTag has no $xeniaAsset" }
    $download = Join-Path ([IO.Path]::GetTempPath()) "$xeniaTag-$xeniaAsset"
    Write-Host "Downloading $($asset.browser_download_url)"
    Invoke-WebRequest $asset.browser_download_url -OutFile $download -UserAgent $agent -UseBasicParsing
    try {
        $actual = (Get-FileHash $download -Algorithm SHA256).Hash.ToLowerInvariant()
        if ($asset.digest -notmatch '^sha256:([0-9a-f]{64})$') { Write-Warning "GitHub gives no SHA-256 for $xeniaAsset; it is $actual" }
        elseif ($actual -ne $Matches[1]) { throw "$xeniaAsset's SHA-256 is $actual, GitHub says $($Matches[1])" }
        $null = New-Item -ItemType Directory $xeniaFolder -Force
        Expand-Archive $download -DestinationPath $xeniaFolder -Force
    } finally { Remove-Item $download -ErrorAction SilentlyContinue }
    Get-ChildItem $xeniaFolder -Recurse -File | Unblock-File
    $exe = Get-ChildItem $xeniaFolder -Filter xenia_edge.exe -Depth 1 | Select-Object -First 1
    if (-not $exe) { throw "$xeniaAsset has no xenia_edge.exe" }
    Write-Host "  $xeniaTag in $xeniaFolder"
    $exe.FullName
}

# Each location: how to recognise it, where it usually is, and what to ask.
$items = [ordered]@{
    Xenia = @{ Variable = 'XENIA_EDGE'; Test = { param($p) Test-Path $p -PathType Leaf }
        # The silverlight-build release extracted beside this repository (README.md), else a build of its branch.
        Guesses = @(@(Get-ChildItem "$parent\xenia-edge", "$parent\xenia-edge-$xeniaTag" -Filter xenia_edge.exe -Depth 1 -ErrorAction SilentlyContinue |
                ForEach-Object FullName) +
            "$parent\xenia-edge\build\bin\Windows\Release\xenia_edge.exe", "$parent\..\xenia-edge\build\bin\Windows\Release\xenia_edge.exe")
        Ask = 'xenia_edge.exe' }
    Work = @{ Variable = 'XBOXNGEN_WORK'; Test = { param($p) $true }; Guesses = @("$root\work"); Optional = $true }
    Apps = @{ Variable = 'XBOXNGEN_APPS'; Test = { param($p) Test-Path (Join-Path $p 'Vimeo\mscorlib.dll') }
        Guesses = @("$parent\Apps", "$parent\apps"); Optional = $true }
    Silverlight360 = @{ Variable = 'Silverlight360Dir'; Test = { param($p) Test-Path (Join-Path $p 'msbuild\XboxSilverlight.targets') }
        Guesses = @("$parent\Silverlight360")
        Ask = 'the Silverlight360 SDK checkout' }
    Xsdk = @{ Variable = 'XSDK'; Test = { param($p) Test-Path (Join-Path $p 'build\libxsdk.a') }
        Guesses = @("$parent\xsdk", "$parent\..\xsdk")
        Ask = 'the xsdk checkout (with libxsdk.a built)' }
}
$given = @{ Xenia = $Xenia; Work = $Work; Apps = $Apps; Silverlight360 = $Silverlight360; Xsdk = $Xsdk }

$config = [ordered]@{}
foreach ($name in $items.Keys) {
    $item = $items[$name]
    $candidates = @($given[$name], [Environment]::GetEnvironmentVariable($item.Variable))
    if ($previous) { $candidates += $previous.$name }
    $candidates += $item.Guesses
    $found = $candidates | Where-Object { $_ -and (& $item.Test $_) } | Select-Object -First 1
    if (-not $found -and $name -eq 'Xenia' -and ($DownloadXenia -or (-not $NonInteractive -and
            (Read-Host "No xenia_edge.exe found. Download xenia-edge's $xeniaTag release to $xeniaFolder? (Y/n)") -notmatch '^n'))) {
        try { $found = Get-XeniaRelease } catch { Write-Warning "Couldn't get xenia-edge's release: $_" }
    }
    while (-not $found -and -not $NonInteractive -and -not $item.Optional) {
        $answer = Read-Host "Where is $($item.Ask)? (empty to leave it unset)"
        if (-not $answer) { break }
        if (& $item.Test $answer) { $found = $answer } else { Write-Host "  that isn't it" }
    }
    $config[$name] = if ($found) { [IO.Path]::GetFullPath($found) } else { '' }
    '{0,-15} {1}' -f $name, $(if ($config[$name]) { $config[$name] } else { '(not set)' })
}

$config | ConvertTo-Json | Set-Content $jsonFile -Encoding utf8

# make runs in WSL: C:\x\y is /mnt/c/x/y there.
$mk = "# Written by tools\Configure.ps1.`n"
if ($config.Xsdk) {
    $drive = $config.Xsdk.Substring(0, 1).ToLowerInvariant()
    $mk += "XSDK ?= /mnt/$drive$($config.Xsdk.Substring(2) -replace '\\', '/')`n"
    # The JIT and the code cache module export by name, which xsdk's elf2xex has from its pull request 4 on.
    $elf2xex = Join-Path $config.Xsdk 'tools\elf2xex.py'
    if (-not ((Test-Path $elf2xex) -and (Select-String -Path $elf2xex -Pattern 'export-name' -SimpleMatch -Quiet))) {
        Write-Warning ("this xsdk's tools\elf2xex.py has no --export-name, which the JIT's build needs: use xsdk with " +
            "https://github.com/xenosworks/xsdk/pull/4 (branch dll-modules of https://github.com/jessenic/xsdk)")
    }
}
Set-Content (Join-Path $root 'config.mk') $mk -Encoding ascii -NoNewline

$sdk = if ($config.Silverlight360) { $config.Silverlight360.TrimEnd('\') + '\' } else { '' }
$props = @"
<!-- Written by tools\Configure.ps1. -->
<Project xmlns="http://schemas.microsoft.com/developer/msbuild/2003">
  <PropertyGroup>
    <Silverlight360Dir Condition=" '`$(Silverlight360Dir)' == '' and '$sdk' != '' ">$sdk</Silverlight360Dir>
  </PropertyGroup>
</Project>
"@
Set-Content (Join-Path $root 'config.props') $props -Encoding utf8
"Wrote config.json, config.mk and config.props in $root."
