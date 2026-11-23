<#
.SYNOPSIS
Records where this machine keeps what XboxNGen's tools need, once, for every later run.

.DESCRIPTION
Finds each location from its parameter, else its environment variable, else the place a checkout usually is (beside
this repository), and asks for any it can't find unless -NonInteractive. With no xenia_edge.exe, it offers to download
xenia-edge's Windows build (README.md) beside this repository; -DownloadXenia does without asking. It puts the
Direct3D 12 runtime files that build loads in its D3D12 folder. An older xenia-edge without --disable_ui (a build of
the silverlight-build branch of jessenic/xenia-edge) isn't used. Writes, at the repository's root (git ignores
them):
  config.json   the locations, for the PowerShell tools (tools\Config.ps1 reads it; an environment variable still wins)
  config.mk     XSDK, as WSL sees it, for make in jit\, host\ and codecache\
  config.props  Silverlight360Dir, for the test apps' MSBuild projects
Run it again to change a location; -Show prints what is configured.

Making native images (tools\) needs:
  Xenia           xenia_edge.exe (xenia-edge's Windows build, or your own build of it)
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

# The xenia-edge build README.md names: upstream's Windows build of that commit.
$xeniaRepository = 'has207/xenia-edge'
$xeniaTag = '1198c6b'
$xeniaAsset = 'xenia_edge_windows.zip'
# The Direct3D 12 runtime files that build loads from D3D12\ beside it, as its redist_installer_wx.cc pins them. It
# offers to download them when they're missing, and the tools' --unattended runs answer no, so they go there first.
$d3d12Runtime = @(
    @{ Url = 'https://github.com/microsoft/DirectXShaderCompiler/releases/download/v1.9.2602.24/dxc_2026_05_27.zip'
        Sha256 = 'cf658aacf070d3045e31b8f1f8a696c2945f37c1095019481ef7c513368db3b4'; Entry = 'bin/x64/dxil.dll'; Name = 'dxil.dll' }
    @{ Url = 'https://www.nuget.org/api/v2/package/Microsoft.Direct3D.D3D12/1.619.3'
        Sha256 = '43a7d5a3973812eb4b42623fae5275c790a005b8e48b8d7f5bb43cef39e073c5'
        Entry = 'build/native/bin/x64/D3D12Core.dll'; Name = 'D3D12Core.dll' }
)
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

# Puts the Direct3D 12 runtime files missing from D3D12\ beside xenia_edge.exe, each checked against its SHA-256.
function Install-D3D12Runtime([string]$exe) {
    $folder = Join-Path (Split-Path $exe -Parent) 'D3D12'
    foreach ($file in $d3d12Runtime) {
        $target = Join-Path $folder $file.Name
        if (Test-Path $target) { continue }
        $download = Join-Path ([IO.Path]::GetTempPath()) "xboxngen-$($file.Name).zip"
        Write-Host "Downloading $($file.Url) for $target"
        Invoke-WebRequest $file.Url -OutFile $download -UserAgent 'XboxNGen-Configure' -UseBasicParsing
        try {
            $actual = (Get-FileHash $download -Algorithm SHA256).Hash.ToLowerInvariant()
            if ($actual -ne $file.Sha256) { throw "its SHA-256 is $actual, not $($file.Sha256)" }
            Add-Type -AssemblyName System.IO.Compression.FileSystem
            $zip = [IO.Compression.ZipFile]::OpenRead($download)
            try {
                $entry = $zip.GetEntry($file.Entry)
                if (-not $entry) { throw "it has no $($file.Entry)" }
                $null = New-Item -ItemType Directory $folder -Force
                [IO.Compression.ZipFileExtensions]::ExtractToFile($entry, $target)
            } finally { $zip.Dispose() }
        } finally { Remove-Item $download -ErrorAction SilentlyContinue }
    }
}

# Upstream xenia-edge's --disable_ui, which Run-Xenia.ps1 passes: builds of the silverlight-build branch of
# jessenic/xenia-edge (the release XboxNGen used before) have --headless instead, and fail on it.
function Test-XeniaFlags([string]$exe) {
    [Text.Encoding]::ASCII.GetString([IO.File]::ReadAllBytes($exe)).Contains('disable_ui needs unattended')
}

# Each location: how to recognise it, where it usually is, and what to ask.
$items = [ordered]@{
    Xenia = @{ Variable = 'XENIA_EDGE'
        Test = { param($p)
            if (-not (Test-Path $p -PathType Leaf)) { return $false }
            if (Test-XeniaFlags $p) { return $true }
            Write-Warning "$p has no --disable_ui: it's an older xenia-edge (the silverlight-build branch); use upstream's"
            $false }
        # The release extracted beside this repository (README.md), else a build of xenia-edge there.
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
if ($config.Xenia) {
    try { Install-D3D12Runtime $config.Xenia } catch { Write-Warning "Couldn't get xenia-edge's Direct3D 12 runtime files: $_" }
}

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
