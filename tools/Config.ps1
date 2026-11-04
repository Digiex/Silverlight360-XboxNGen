<#
.SYNOPSIS
Gives one of the locations the tools use: from its environment variable if set, else from config.json at the
repository's root (tools\Configure.ps1 writes it), else its default.

.EXAMPLE
$xenia = & (Join-Path $PSScriptRoot 'Config.ps1') Xenia
#>
param(
    [Parameter(Mandatory)] [ValidateSet('Xenia', 'Work', 'Apps', 'Silverlight360', 'Xsdk')] [string]$Name
)
$variables = @{ Xenia = 'XENIA_EDGE'; Work = 'XBOXNGEN_WORK'; Apps = 'XBOXNGEN_APPS'; Silverlight360 = 'Silverlight360Dir';
    Xsdk = 'XSDK' }
$fromEnvironment = [Environment]::GetEnvironmentVariable($variables[$Name])
if ($fromEnvironment) { return $fromEnvironment }
$file = Join-Path $PSScriptRoot '..\config.json'
if (Test-Path $file) {
    $value = (Get-Content $file -Raw | ConvertFrom-Json).$Name
    if ($value) { return $value }
}
switch ($Name) {
    'Xenia' { 'xenia_edge.exe' }  # from PATH
    'Work' { [IO.Path]::GetFullPath((Join-Path $PSScriptRoot '..\work')) }
    default { '' }
}
