# Build the combined distribution only from a previously Microsoft-signed driver.
[CmdletBinding()]
param(
    [Parameter(Mandatory=$true)][string]$AppDirectory,
    [Parameter(Mandatory=$true)][string]$SignedDriverDirectory,
    [Parameter(Mandatory=$true)][string]$WebView2Installer,
    [Parameter(Mandatory=$true)][string]$OutputDirectory,
    [Parameter(Mandatory=$true)][string]$SignTool
)
$ErrorActionPreference = 'Stop'
$app = (Resolve-Path -LiteralPath $AppDirectory).Path
$driver = (Resolve-Path -LiteralPath $SignedDriverDirectory).Path
$runtime = (Resolve-Path -LiteralPath $WebView2Installer).Path
$output = [IO.Path]::GetFullPath($OutputDirectory)
if (Test-Path -LiteralPath $output) { throw 'Output directory must be new.' }
$runtimeSignature = Get-AuthenticodeSignature -LiteralPath $runtime
if ($runtimeSignature.Status -ne 'Valid' -or $runtimeSignature.SignerCertificate.Subject -notmatch '(^|, )CN=Microsoft Corporation(,|$)') {
    throw 'A valid Microsoft-signed Evergreen WebView2 standalone installer is required.'
}
& $SignTool verify /pa $runtime
if ($LASTEXITCODE -ne 0) { throw 'WebView2 installer signature verification failed.' }
foreach ($file in @('LoopZ.exe','loopz-driver-setup.exe','loopz-windows-control.exe')) {
    if (!(Test-Path -LiteralPath (Join-Path $app $file))) { throw "Missing $file" }
    & $SignTool verify /pa (Join-Path $app $file)
    if ($LASTEXITCODE -ne 0) { throw "Invalid application signature: $file" }
}
foreach ($file in @('LoopZAudio.inf','LoopZAudio.sys','LoopZAudio.cat')) {
    if (!(Test-Path -LiteralPath (Join-Path $driver $file))) { throw "Missing $file" }
}
$catalogSignature = Get-AuthenticodeSignature -LiteralPath (Join-Path $driver 'LoopZAudio.cat')
if ($catalogSignature.Status -ne 'Valid' -or $catalogSignature.SignerCertificate.Subject -notmatch '(^|, )CN=Microsoft Windows Hardware Compatibility Publisher(,|$)') {
    throw 'A Microsoft Hardware Dashboard signed catalog is required; development certificates are not accepted.'
}
# /kp checks kernel policy; /c checks that the actual INF and SYS are catalog members.
foreach ($file in @('LoopZAudio.inf','LoopZAudio.sys')) {
    & $SignTool verify /kp /c (Join-Path $driver 'LoopZAudio.cat') (Join-Path $driver $file)
    if ($LASTEXITCODE -ne 0) { throw "Driver signature or catalog membership invalid: $file" }
}
New-Item -ItemType Directory -Path $output | Out-Null
# Replace an existing combined package's driver and manifest rather than
# carrying stale development files or a self-referential manifest forward.
Get-ChildItem -LiteralPath $app | Where-Object {
    $_.Name -notin @('driver','prerequisites','manifest.json')
} | Copy-Item -Destination $output -Recurse -Force
$target = New-Item -ItemType Directory -Path (Join-Path $output 'driver')
foreach ($file in @('LoopZAudio.inf','LoopZAudio.sys','LoopZAudio.cat')) {
    Copy-Item -LiteralPath (Join-Path $driver $file) -Destination $target.FullName
}
Copy-Item -LiteralPath (Join-Path $PSScriptRoot '..\native\platform\windows\driver\LICENSE') -Destination (Join-Path $target.FullName 'LICENSE.txt')
$prerequisites = New-Item -ItemType Directory -Path (Join-Path $output 'prerequisites')
Copy-Item -LiteralPath $runtime -Destination (Join-Path $prerequisites.FullName 'WebView2RuntimeInstaller.exe')
$manifest = Get-ChildItem -LiteralPath $output -Recurse -File | Sort-Object FullName | ForEach-Object {
    [ordered]@{path=$_.FullName.Substring($output.Length+1); sha256=(Get-FileHash -LiteralPath $_.FullName -Algorithm SHA256).Hash.ToLowerInvariant()}
}
ConvertTo-Json -InputObject @($manifest) -Depth 3 | Set-Content -LiteralPath (Join-Path $output 'manifest.json') -Encoding UTF8
Write-Output "Verified combined distribution: $output"
