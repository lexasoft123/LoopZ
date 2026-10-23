# Development-only harness: embeds production Files/Run and Code with only the
# runtime registry dependency replaced by an owned file. Helpers are harmless stubs.
param(
 [Parameter(Mandatory=$true)][string]$ProductionScript,
 [Parameter(Mandatory=$true)][string]$Compiler,
 [Parameter(Mandatory=$true)][string]$StubExecutable,
 [Parameter(Mandatory=$true)][string]$FixtureDirectory
)
$ErrorActionPreference='Stop'
if (Test-Path $FixtureDirectory) { throw 'Fixture directory must be new' }
New-Item -ItemType Directory $FixtureDirectory | Out-Null
$source=Get-Content -LiteralPath $ProductionScript -Raw
$codeOffset=$source.IndexOf('[Code]')
$runOffset=$source.IndexOf('[Run]')
$filesOffset=$source.IndexOf('[Files]')
$iconsOffset=$source.IndexOf('[Icons]')
if ($filesOffset -lt 0 -or $iconsOffset -le $filesOffset -or $runOffset -le $iconsOffset -or $codeOffset -le $runOffset) { throw 'Unexpected production section order' }
$files=$source.Substring($filesOffset,$iconsOffset-$filesOffset)
$code=$source.Substring($codeOffset)
# Replace only the registry dependency with an owned fixture file. The production
# version parsing, installation, verification and launch/error gates stay exact.
$registryQuery=@'
RegQueryStringValue(HKLM32,
    'SOFTWARE\Microsoft\EdgeUpdate\Clients\{F3017226-FE2A-4295-8BDF-00C3A9A7E4C5}',
    'pv', VersionString)
'@
if(!$code.Contains($registryQuery)){throw 'Production runtime registry seam changed'}
$code=$code.Replace($registryQuery, 'FixtureReadRuntimeVersion(VersionString)')
$runtimeReader=@'
function FixtureReadRuntimeVersion(var Value: String): Boolean;
var
  Content: AnsiString;
begin
  Result := LoadStringFromFile('{#FixtureRoot}\runtime-version.txt', Content);
  Value := String(Content);
end;

'@
$code=$code.Replace('function WebView2MachineRuntimePresent(): Boolean;', $runtimeReader + 'function WebView2MachineRuntimePresent(): Boolean;')
$run=$source.Substring($runOffset,$codeOffset-$runOffset)
# Ordinary Run entries precede ssPostInstall. Exercise the exact predicate only
# at completion, after the production hook has assigned its helper result.
$code += @'

<event('CurStepChanged')>
procedure FixtureLaunchGate(CurStep: TSetupStep);
var
  FixtureCode: Integer;
begin
  Log('Fixture step=' + IntToStr(Ord(CurStep)));
  if CanLaunchLoopZ then Log('Fixture launch predicate=true')
  else Log('Fixture launch predicate=false');
  if CurStep = ssDone then begin
    if CanLaunchLoopZ then begin
      FixtureCode := -1;
      if not Exec(ExpandConstant('{app}\LoopZ.exe'), '', '', SW_HIDE,
        ewWaitUntilTerminated, FixtureCode) then
        RaiseException('Fixture launch failed');
      if FixtureCode <> 0 then RaiseException('Fixture app returned failure');
    end;
  end;
end;
'@
function Test-QuietUninstall([string]$installed,[string]$root,[int]$helperExit,[string]$label) {
 $uninstaller=Join-Path $installed 'unins000.exe'
 if(!(Test-Path $uninstaller)){throw 'Fixture uninstaller missing'}
 [IO.File]::WriteAllText((Join-Path $installed 'uninstall-result-code.txt'),[string]$helperExit)
 $called=Join-Path $installed 'uninstall-called.txt'
 if(Test-Path $called){Remove-Item -LiteralPath $called}
 $log=Join-Path $root ($label+'-uninstall.log')
 $process=Start-Process -FilePath $uninstaller -ArgumentList @('/VERYSILENT','/SUPPRESSMSGBOXES','/NORESTART',('/LOG="'+$log+'"')) -PassThru
 if(!$process.WaitForExit(45000)) {
  # This is the just-launched isolated fixture, never the installed LoopZ app.
  $process.Kill()
  throw 'Silent fixture uninstall timed out'
 }
 if(!(Test-Path $called) -or (Get-Content $called -Raw) -ne '--uninstall'){throw 'Uninstall helper was not called'}
 $filesRemain=(Test-Path (Join-Path $installed 'LoopZ.exe')) -and (Test-Path (Join-Path $installed 'loopz-driver-setup.exe'))
 if($helperExit -eq 42) {
  if($process.ExitCode -eq 0 -or !$filesRemain -or (Get-Content $log -Raw) -notmatch 'installed files retained for retry'){throw 'Failed uninstall did not preserve retry files'}
 } else {
  if($process.ExitCode -ne 0 -or (Test-Path (Join-Path $installed 'LoopZ.exe')) -or (Test-Path (Join-Path $installed 'loopz-driver-setup.exe'))){throw 'Successful quiet uninstall did not remove fixture app files'}
  if($helperExit -eq 3010 -and (Get-Content $log -Raw) -notmatch 'LoopZ uninstall requests restart'){throw 'Uninstall restart requirement was lost'}
 }
 return @{name=$label;helperExit=$helperExit;uninstallExit=$process.ExitCode;helperCalled=$true;filesRetained=$filesRemain;silent=$true}
}
$report=[ordered]@{productionScriptHash=(Get-FileHash $ProductionScript).Hash;stubHash=(Get-FileHash $StubExecutable).Hash;cases=@();uninstallCases=@();fullWindowsPortComplete=$false}
$previousRuntimeFixture=$env:LOOPZ_INSTALLER_RUNTIME_FIXTURE
try {
foreach($case in @('success','restart','failure','runtime-install','runtime-failure','runtime-unverified','runtime-restart')) {
 $root=Join-Path $FixtureDirectory $case
 $payload=Join-Path $root 'payload'
 $installed=Join-Path $root 'installed'
 $output=Join-Path $root 'output'
 New-Item -ItemType Directory $payload,$output | Out-Null
 Copy-Item $StubExecutable (Join-Path $payload 'loopz-driver-setup.exe')
 Copy-Item $StubExecutable (Join-Path $payload 'LoopZ.exe')
 $prerequisites=New-Item -ItemType Directory (Join-Path $payload 'prerequisites')
 Copy-Item $StubExecutable (Join-Path $prerequisites.FullName 'WebView2RuntimeInstaller.exe')
 $env:LOOPZ_INSTALLER_RUNTIME_FIXTURE=$root
 $runtimeMissing=$case.StartsWith('runtime-')
 $runtimeBlocked=$case -in @('runtime-failure','runtime-unverified','runtime-restart')
 if(!$runtimeMissing){[IO.File]::WriteAllText((Join-Path $root 'runtime-version.txt'),'140.0.0.0')}
 $runtimeExit=if($case -eq 'runtime-failure'){42}elseif($case -eq 'runtime-restart'){3010}else{0}
 [IO.File]::WriteAllText((Join-Path $root 'runtime-result-code.txt'),[string]$runtimeExit)
 if($case -eq 'runtime-install'){[IO.File]::WriteAllText((Join-Path $root 'publish-runtime.txt'),'publish')}

 $expected=if($case -in @('success','runtime-install')){0}elseif($case -eq 'restart'){3010}else{42}
 [IO.File]::WriteAllText((Join-Path $payload 'result-code.txt'),[string]$expected)
 $id=[guid]::NewGuid().ToString()
 $script=@"
#define Payload "$payload"
#define FixtureRoot "$root"
[Setup]
AppId=LoopZInstallerFlowFixture-$id
AppName=LoopZ installer flow fixture
AppVersion=0.0.0
DefaultDirName=$installed
PrivilegesRequired=lowest
Uninstallable=yes
CreateUninstallRegKey=no
DisableDirPage=yes
DisableProgramGroupPage=yes
OutputDir=$output
OutputBaseFilename=flow
Compression=lzma2
$files
$run
$code
"@
 $scriptPath=Join-Path $root 'flow.iss'
 [IO.File]::WriteAllText($scriptPath,$script,[Text.UTF8Encoding]::new($false))
 & $Compiler /Qp $scriptPath
 if ($LASTEXITCODE -ne 0) { throw 'Installer flow harness compilation failed' }
 $log=Join-Path $root 'setup.log'
 $process=Start-Process -FilePath (Join-Path $output 'flow.exe') -ArgumentList @('/VERYSILENT','/SUPPRESSMSGBOXES','/NORESTART','/RESTARTEXITCODE=3010',('/LOG="'+$log+'"')) -PassThru
 if(!$process.WaitForExit(45000)) {
  $process.Kill()
  throw 'Silent fixture setup timed out'
 }
 $runtimeCalled=Test-Path (Join-Path $root 'runtime-called.txt')
 if($runtimeCalled -ne $runtimeMissing){throw 'Runtime prerequisite did not follow detection result'}
 if($runtimeBlocked) {
  if($process.ExitCode -eq 0 -or (Test-Path (Join-Path $installed 'loopz-driver-setup.exe')) -or (Test-Path (Join-Path $installed 'app-launched.txt'))){throw 'Unready runtime did not stop before application installation'}
  $expectedMessage=if($case -eq 'runtime-restart'){'Restart Windows'}else{'Microsoft WebView2 setup could not finish'}
  if((Get-Content $log -Raw) -notmatch $expectedMessage){throw 'Runtime failure or restart was not explained'}
  $report.cases+=@{name=$case;runtimeCalled=$runtimeCalled;runtimeHelperExit=$runtimeExit;setupExit=$process.ExitCode;applicationInstallPrevented=$true}
  continue
 }
 if($case -eq 'runtime-install' -and (Get-Content (Join-Path $root 'runtime-version.txt') -Raw) -ne '140.0.0.0'){throw 'Runtime post-install verification fixture failed'}
 $called=Join-Path $installed 'helper-called.txt'
 if (!(Test-Path $called) -or (Get-Content $called -Raw) -ne '--install-service') { throw 'Production post-install hook did not call helper' }
 if ($case -in @('success','runtime-install') -and $process.ExitCode -ne 0) { throw 'Successful helper did not complete setup' }
 if ($case -eq 'restart' -and $process.ExitCode -ne 3010) { throw 'Successful restart-required helper did not request restart' }
 if ($case -eq 'failure' -and ($process.ExitCode -eq 0 -or (Get-Content $log -Raw) -notmatch 'LoopZ microphone setup could not finish')) { throw 'Helper failure was not reported by setup' }
 $launched=Test-Path (Join-Path $installed 'app-launched.txt')
 if ($launched -ne ($case -in @('success','runtime-install'))) { throw 'Production app launch predicate failed' }
 $report.cases+=@{name=$case;helperExit=$expected;setupExit=$process.ExitCode;helperCalled=$true;launchPredicateAccepted=$launched;installerHash=(Get-FileHash (Join-Path $output 'flow.exe')).Hash}
 if($case -eq 'success') {
  $report.uninstallCases+=Test-QuietUninstall $installed $root 42 'failure-preserves-files'
  $report.uninstallCases+=Test-QuietUninstall $installed $root 0 'retry-succeeds'
 } elseif($case -eq 'restart') {
  $report.uninstallCases+=Test-QuietUninstall $installed $root 3010 'restart-required'
 }
}
} finally {
 $env:LOOPZ_INSTALLER_RUNTIME_FIXTURE=$previousRuntimeFixture
}
$report.result='PASS'
$report | ConvertTo-Json -Depth 5 | Set-Content (Join-Path $FixtureDirectory 'verification.json')
$report | ConvertTo-Json -Depth 5
