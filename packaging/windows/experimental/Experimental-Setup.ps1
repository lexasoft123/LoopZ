# Built-in Windows PowerShell only. No boot/security changes are performed here.
[CmdletBinding()]
param([ValidateSet('Install','Remove','Check')][string]$Operation='Install')
$ErrorActionPreference='Stop'
$expectedThumbprint='FEC5B498B1777655AF90E665A0856E2755212078'
try {
 if(![Environment]::Is64BitOperatingSystem -or ![Environment]::Is64BitProcess -or $env:PROCESSOR_ARCHITECTURE -ne 'AMD64'){throw 'This preview requires Windows x64. ARM64 is not supported.'}
 if([Environment]::OSVersion.Version.Build -lt 19045){throw 'Windows 10 22H2 or later is required.'}
 $root=[IO.Path]::GetFullPath($PSScriptRoot)
 $manifest=Get-Content -LiteralPath (Join-Path $root 'manifest.json') -Raw|ConvertFrom-Json
 if(!$manifest.developmentOnly -or $manifest.releaseVersion -ne '0.2.1'){throw 'Unexpected experimental package manifest.'}
 foreach($entry in $manifest.hashes.PSObject.Properties) {
  if([IO.Path]::IsPathRooted($entry.Name) -or $entry.Name -match '(^|[/\\])\.\.([/\\]|$)|:'){throw 'Invalid payload path.'}
  $path=[IO.Path]::GetFullPath((Join-Path $root $entry.Name))
  if(!$path.StartsWith($root+[IO.Path]::DirectorySeparatorChar,[StringComparison]::OrdinalIgnoreCase)){throw 'Payload path escapes the package.'}
  if((Get-FileHash -LiteralPath $path -Algorithm SHA256).Hash -ne $entry.Value){throw ('Damaged package file: '+$entry.Name)}
 }
 foreach($name in @('LoopZ.exe','loopz-driver-setup.exe','LoopZDeviceBroker.exe','LoopZ-Workbench-Public.cer','driver/LoopZAudio.sys','driver/loopzaudio.cat','driver/LoopZAudio.inf')) {
  if(!$manifest.hashes.PSObject.Properties[$name]){throw ('Missing verified file: '+$name)}
 }
 $certificate=New-Object Security.Cryptography.X509Certificates.X509Certificate2((Join-Path $root 'LoopZ-Workbench-Public.cer'))
 if($certificate.Thumbprint -ne $expectedThumbprint -or $certificate.HasPrivateKey -or ($Operation -eq 'Install' -and $certificate.NotAfter.ToUniversalTime() -le [DateTime]::UtcNow)){throw 'Development certificate is unexpected or expired.'}
 if($Operation -ne 'Remove') {
 foreach($name in @('driver/LoopZAudio.sys','driver/loopzaudio.cat')) {
  $signature=Get-AuthenticodeSignature -LiteralPath (Join-Path $root $name)
  if(!$signature.SignerCertificate -or $signature.SignerCertificate.Thumbprint -ne $expectedThumbprint -or $signature.Status -notin @('Valid','NotTrusted')){throw ('Unexpected driver signature: '+$name)}
 }
 }
 Add-Type @'
using System;
using System.Runtime.InteropServices;
public static class LoopZExperimentalIntegrity {
 [StructLayout(LayoutKind.Sequential)] struct Information {public uint Length;public uint Options;}
 [DllImport("ntdll.dll")] static extern int NtQuerySystemInformation(int kind,ref Information value,uint length,out uint returned);
 public static uint Options() {
  Information value=new Information {Length=8};uint returned;
  if(NtQuerySystemInformation(103,ref value,8,out returned)<0 || returned!=8)throw new InvalidOperationException("Cannot read active Windows driver-signing policy.");
  return value.Options;
 }
}
'@
 $options=[LoopZExperimentalIntegrity]::Options()
 if($Operation -eq 'Check') {
  @{ok=$true;version='0.2.1';developmentOnly=$true;payloadVerified=$true;activeTestSigning=(($options -band 2)-ne 0);codeIntegrityOptions=$options;certificateThumbprint=$certificate.Thumbprint}|ConvertTo-Json
  exit 0
 }
 $admin=([Security.Principal.WindowsPrincipal][Security.Principal.WindowsIdentity]::GetCurrent()).IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)
 if(!$admin){throw 'Right-click Install.cmd or Remove.cmd and choose Run as administrator. Read WINDOWS-SETUP.md first.'}
 if($Operation -eq 'Install') {
  if(($options -band 2)-eq 0){throw 'Windows Test Mode is not active. Follow WINDOWS-SETUP.md and restart Windows before installing. No settings were changed.'}
  Write-Host 'EXPERIMENTAL LOOPZ 0.2.1 - DEVELOPMENT-SIGNED DRIVER'
  Write-Host 'This installs the LoopZ microphone service/driver and trusts the included development certificate for this PC.'
  Write-Host 'Test Mode accepts development-signed drivers system-wide. This preview is intended for a test PC, not a normal production installation.'
  Write-Host 'Setup does not change Secure Boot, BitLocker, Memory Integrity, Test Mode, login choices or system audio defaults, and does not restart Windows.'
  if((Read-Host 'Type INSTALL EXPERIMENTAL LOOPZ to continue') -cne 'INSTALL EXPERIMENTAL LOOPZ'){throw 'Installation cancelled. No settings were changed.'}
  foreach($store in @('Root','TrustedPublisher')) {
   if(!(Test-Path ('Cert:\LocalMachine\'+$store+'\'+$expectedThumbprint))) {
    & "$env:SystemRoot\System32\certutil.exe" -addstore -f $store (Join-Path $root 'LoopZ-Workbench-Public.cer')
    if($LASTEXITCODE){throw ('Could not trust the development certificate in '+$store+'. See WINDOWS-SETUP.md for cleanup.')}
   }
  }
  & (Join-Path $root 'loopz-driver-setup.exe') --install-service
 } else {
  Write-Host 'This removes LoopZ microphones and its background device service. Saved mixes are kept. Close LoopZ and recording clients first.'
  if((Read-Host 'Type REMOVE LOOPZ to continue') -cne 'REMOVE LOOPZ'){throw 'Removal cancelled.'}
  & (Join-Path $root 'loopz-driver-setup.exe') --uninstall
 }
 $code=$LASTEXITCODE
 if($code -notin @(0,3010)){throw ('Native microphone maintenance failed (exit '+$code+'). Keep this folder for repair and see WINDOWS-SETUP.md.')}
 if($code -eq 3010){Write-Host 'Windows requires a restart. Save your work and restart when convenient.'}
 elseif($Operation -eq 'Install'){Write-Host 'Microphone setup finished. Open LoopZ.exe normally, without administrator rights.'}
 else {Write-Host 'Microphones removed. See WINDOWS-SETUP.md to remove certificate trust and restore normal driver signing.'}
 exit 0
} catch {
 Write-Host ('LoopZ setup stopped: '+$_.Exception.Message) -ForegroundColor Red
 exit 1
}
