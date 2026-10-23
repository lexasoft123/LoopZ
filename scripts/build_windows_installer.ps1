[CmdletBinding()]
param(
    [Parameter(Mandatory=$true)][string]$AppDirectory,
    [Parameter(Mandatory=$true)][string]$SignedDriverDirectory,
    [Parameter(Mandatory=$true)][string]$WebView2Installer,
    [Parameter(Mandatory=$true)][string]$OutputDirectory,
    [Parameter(Mandatory=$true)][string]$SignTool,
    [Parameter(Mandatory=$true)][string]$InnoCompiler,
    [Parameter(Mandatory=$true)][string]$SigningCertificateThumbprint,
    [Parameter(Mandatory=$true)][string]$TimestampUrl,
    [string]$Version = '0.2.1'
)
$ErrorActionPreference = 'Stop'
if ($Version -notmatch '^\d+\.\d+\.\d+([.-][A-Za-z0-9.-]+)?$') { throw 'Invalid version.' }
if ($SigningCertificateThumbprint -notmatch '^[A-Fa-f0-9]{40}$') { throw 'Invalid certificate thumbprint.' }
$output = [IO.Path]::GetFullPath($OutputDirectory)
if (Test-Path -LiteralPath $output) { throw 'Output directory must be new.' }
New-Item -ItemType Directory -Path $output -Force | Out-Null
$payload = Join-Path $output 'payload'
& (Join-Path $PSScriptRoot 'package_windows_release.ps1') -AppDirectory $AppDirectory -SignedDriverDirectory $SignedDriverDirectory -WebView2Installer $WebView2Installer -OutputDirectory $payload -SignTool $SignTool
$timestamp = [Uri]$TimestampUrl
if ($timestamp.Scheme -ne 'https') { throw 'Use an HTTPS timestamp service.' }
# Inno uses $f as its signed-file placeholder; keep it literal.
$signCommand = '$q' + $SignTool + '$q sign /fd SHA256 /sha1 ' + $SigningCertificateThumbprint + ' /tr $q' + $TimestampUrl + '$q /td SHA256 $f'
& $InnoCompiler "/SLoopZ=$signCommand" "/DPayload=$payload" "/DAppVersion=$Version" "/O$output" (Join-Path $PSScriptRoot '..\packaging\windows\LoopZ.iss')
if ($LASTEXITCODE -ne 0) { throw 'Installer compilation failed.' }
$installer = Join-Path $output "LoopZ-$Version-windows-x64-setup.exe"
& $SignTool verify /pa $installer
if ($LASTEXITCODE -ne 0) { throw 'Installer signature verification failed.' }
Get-FileHash -LiteralPath $installer -Algorithm SHA256
