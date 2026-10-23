# Sign a development package using an existing workbench certificate.
# This never changes certificates, boot security, installed devices, or reboot state.
[CmdletBinding()]
param(
    [Parameter(Mandatory=$true)][string]$UnsignedDriverDirectory,
    [Parameter(Mandatory=$true)][string]$OutputDirectory,
    [Parameter(Mandatory=$true)][string]$CertificateThumbprint,
    [Parameter(Mandatory=$true)][string]$SignTool,
    [Parameter(Mandatory=$true)][string]$Inf2Cat,
    [switch]$MachineStore
)
$ErrorActionPreference = 'Stop'
if ($CertificateThumbprint -notmatch '^[a-fA-F0-9]{40}$') { throw 'Invalid certificate thumbprint.' }
$source = (Resolve-Path -LiteralPath $UnsignedDriverDirectory).Path
$output = [IO.Path]::GetFullPath($OutputDirectory)
if (Test-Path -LiteralPath $output) { throw 'Output directory must be new.' }
$store = if ($MachineStore) { 'LocalMachine' } else { 'CurrentUser' }
$cert = Get-Item -LiteralPath "Cert:\$store\My\$CertificateThumbprint"
if (!$cert.HasPrivateKey) { throw 'The development signing key is unavailable.' }
New-Item -ItemType Directory -Path $output -Force | Out-Null
foreach ($file in @('LoopZAudio.inf','LoopZAudio.sys')) {
    Copy-Item -LiteralPath (Join-Path $source $file) -Destination $output
}
$license = Join-Path $PSScriptRoot '..\native\platform\windows\driver\LICENSE'
Copy-Item -LiteralPath $license -Destination (Join-Path $output 'LICENSE.txt')
$arguments = @('sign','/s','My','/sha1',$CertificateThumbprint,'/fd','SHA256')
if ($MachineStore) { $arguments += '/sm' }
& $SignTool @arguments (Join-Path $output 'LoopZAudio.sys')
if ($LASTEXITCODE -ne 0) { throw 'Development SYS signing failed.' }
& $Inf2Cat "/driver:$output" /os:10_VB_X64 /uselocaltime
if ($LASTEXITCODE -ne 0) { throw 'Development catalog generation failed.' }
& $SignTool @arguments (Join-Path $output 'LoopZAudio.cat')
if ($LASTEXITCODE -ne 0) { throw 'Development catalog signing failed.' }
'Development package only. Installation still requires a separately configured test workbench.' | Set-Content -LiteralPath (Join-Path $output 'DEVELOPMENT-ONLY.txt') -Encoding UTF8
Get-ChildItem -LiteralPath $output -File | Get-FileHash -Algorithm SHA256
