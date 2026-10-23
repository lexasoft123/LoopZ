; Inno Setup 6. Compile only after package_windows_release.ps1 verifies signatures.
#ifndef Payload
  #error Payload must point to a verified combined distribution
#endif
#ifndef AppVersion
  #define AppVersion "0.2.1"
#endif
#if !FileExists(Payload + "\manifest.json")
  #error Run package_windows_release.ps1 before compiling this installer
#endif
[Setup]
AppId={{9485E546-1442-4DC2-878E-65CB143674C5}
AppName=LoopZ
AppVersion={#AppVersion}
AppPublisher=LoopZ
DefaultDirName={autopf}\LoopZ
DefaultGroupName=LoopZ
PrivilegesRequired=admin
ArchitecturesAllowed=x64os
ArchitecturesInstallIn64BitMode=x64os
MinVersion=10.0.19045
OutputBaseFilename=LoopZ-{#AppVersion}-windows-x64-setup
Compression=lzma2
SolidCompression=yes
UninstallDisplayIcon={app}\LoopZ.exe
CloseApplications=yes
RestartApplications=no
SetupLogging=yes
SignTool=LoopZ
SignedUninstaller=yes
[Files]
Source: "{#Payload}\*"; DestDir: "{app}"; Excludes: "loopz-driver-setup.exe,prerequisites\*"; Flags: ignoreversion recursesubdirs createallsubdirs
Source: "{#Payload}\prerequisites\WebView2RuntimeInstaller.exe"; Flags: dontcopy
Source: "{#Payload}\loopz-driver-setup.exe"; DestDir: "{app}"; Flags: ignoreversion; AfterInstall: InstallMicrophones
[Icons]
Name: "{group}\LoopZ"; Filename: "{app}\LoopZ.exe"
Name: "{autodesktop}\LoopZ"; Filename: "{app}\LoopZ.exe"; Tasks: desktopicon
[Tasks]
Name: "desktopicon"; Description: "Create a desktop shortcut"; Flags: unchecked
[Run]
Filename: "{app}\LoopZ.exe"; Description: "Open LoopZ"; Flags: postinstall nowait skipifsilent runasoriginaluser; Check: CanLaunchLoopZ
[Code]
var
  DriverRebootRequired: Boolean;
  MicrophoneSetupSucceeded: Boolean;
  MicrophoneSetupAttempted: Boolean;

function WebView2MachineRuntimePresent(): Boolean;
var
  VersionString: String;
  Version: Int64;
begin
  { Require a machine install: Setup may run under a different administrator
    account from the user who will open LoopZ. Evergreen uses the 32-bit key. }
  Result := False;
  if RegQueryStringValue(HKLM32,
    'SOFTWARE\Microsoft\EdgeUpdate\Clients\{F3017226-FE2A-4295-8BDF-00C3A9A7E4C5}',
    'pv', VersionString) then begin
    if StrToVersion(VersionString, Version) then Result := Version > 0;
  end;
end;

function PrepareToInstall(var NeedsRestart: Boolean): String;
var
  Code: Integer;
begin
  Result := '';
  if WebView2MachineRuntimePresent then Exit;
  ExtractTemporaryFile('WebView2RuntimeInstaller.exe');
  Code := -1;
  if not Exec(ExpandConstant('{tmp}\WebView2RuntimeInstaller.exe'),
    '/silent /install', '', SW_HIDE, ewWaitUntilTerminated, Code) then begin
    Result := 'Microsoft WebView2 setup could not start. Run LoopZ setup again.';
    Exit;
  end;
  if Code = 3010 then begin
    NeedsRestart := True;
    Result := 'Restart Windows, then run LoopZ setup again to finish installing Microsoft WebView2.';
    Exit;
  end;
  if (Code <> 0) or not WebView2MachineRuntimePresent then
    Result := 'Microsoft WebView2 setup could not finish. Run LoopZ setup again. Your saved mixes have been kept.';
end;

function CanLaunchLoopZ(): Boolean;
begin
  Result := MicrophoneSetupSucceeded and not DriverRebootRequired;
end;

procedure InstallMicrophones;
var
  Code: Integer;
begin
  if not MicrophoneSetupAttempted then begin
    MicrophoneSetupAttempted := True;
    Code := -1;
    MicrophoneSetupSucceeded :=
      Exec(ExpandConstant('{app}\loopz-driver-setup.exe'), '--install-service', '',
        SW_HIDE, ewWaitUntilTerminated, Code);
    DriverRebootRequired := MicrophoneSetupSucceeded and (Code = 3010);
    MicrophoneSetupSucceeded := MicrophoneSetupSucceeded and ((Code = 0) or DriverRebootRequired);
    if not MicrophoneSetupSucceeded then
      RaiseException('LoopZ microphone setup could not finish. Run setup again to repair it. Your saved mixes have been kept.');
  end;
end;

procedure CurStepChanged(CurStep: TSetupStep);
begin
  if CurStep = ssPostInstall then InstallMicrophones;
end;

function NeedRestart(): Boolean;
begin
  Result := DriverRebootRequired;
end;

function GetCustomSetupExitCode(): Integer;
begin
  if MicrophoneSetupSucceeded then Result := 0 else Result := 1;
end;

function InitializeUninstall(): Boolean;
var
  Code: Integer;
begin
  if not UninstallSilent then begin
    if MsgBox('Remove LoopZ and all of its Windows microphones?', mbConfirmation, MB_YESNO) <> IDYES then begin
      Result := False;
      Exit;
    end;
  end;
  Result := Exec(ExpandConstant('{app}\loopz-driver-setup.exe'), '--uninstall', '', SW_HIDE, ewWaitUntilTerminated, Code);
  if Result then begin
    DriverRebootRequired := Code = 3010;
    if DriverRebootRequired then Log('LoopZ uninstall requests restart');
    Result := (Code = 0) or DriverRebootRequired;
  end;
  if not Result then begin
    Log('LoopZ driver removal failed; installed files retained for retry');
    if not UninstallSilent then
      MsgBox('The LoopZ driver could not be removed. Close applications using LoopZ microphones and try again. The installed files have been kept so removal can be retried.', mbError, MB_OK);
  end;
end;

function UninstallNeedRestart(): Boolean;
begin
  Result := DriverRebootRequired;
end;
