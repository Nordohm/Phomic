; Phomic installer (Inno Setup 7). Build it with windows\build-release.ps1, which first builds
; the Release version of the app; the version number comes from Phomic.exe (Version.h).
;
; Installs for the current user (no admin) by default. Uninstalling closes Phomic, gives the
; VB-CABLE devices their original names back and removes Phomic's settings and autostart entry.
; Uninstaller switches: /KEEPSETTINGS=1 keeps HKCU\Software\Phomic (pairing, PC id, ...),
; /KEEPMICROPHONE=1 leaves the VB-CABLE devices named "Phomic".

#define AppExe "..\x64\Release\Phomic\Phomic.exe"
#define AppVersion GetStringFileInfo(AppExe, "ProductVersion")

[Setup]
AppId={{CBC2C114-BCA3-41D6-84B6-77F2ED716618}
AppName=Phomic
AppVersion={#AppVersion}
AppVerName=Phomic {#AppVersion}
AppPublisher=Nordohm
AppCopyright=Copyright (c) 2026 Nordohm
VersionInfoVersion={#AppVersion}
DefaultDirName={autopf}\Phomic
DisableProgramGroupPage=yes
PrivilegesRequired=lowest
PrivilegesRequiredOverridesAllowed=dialog
ArchitecturesAllowed=x64compatible
ArchitecturesInstallIn64BitMode=x64compatible
MinVersion=10.0.17763
LicenseFile=License.txt
SetupIconFile=..\Phomic\Assets\PhomicTile.ico
UninstallDisplayIcon={app}\Phomic.exe
UninstallDisplayName=Phomic
OutputDir=..\release
OutputBaseFilename=Phomic-Setup-{#AppVersion}
Compression=lzma2/max
SolidCompression=yes
WizardStyle=modern
; Updating while Phomic runs (it may be in the tray): close it first.
; A folder left behind (say, by an uninstall that couldn't remove everything) needn't be asked about.
DirExistsWarning=no
CloseApplications=force
RestartApplications=no

[Tasks]
Name: "desktopicon"; Description: "{cm:CreateDesktopIcon}"; GroupDescription: "{cm:AdditionalIcons}"; Flags: unchecked

[Files]
; The app, the Windows App SDK runtime (self-contained) and licenses. VB-CABLE isn't included:
; people get it from VB-Audio (the app links to it).
Source: "..\x64\Release\Phomic\*"; DestDir: "{app}"; Excludes: "*.pdb,*.lib,*.exp,*.ilk,VBCABLE"; Flags: ignoreversion recursesubdirs createallsubdirs
Source: "..\..\LICENSE"; DestDir: "{app}\licenses"; DestName: "Phomic.txt"; Flags: ignoreversion
Source: "..\third_party\README.md"; DestDir: "{app}\licenses"; DestName: "third-party.md"; Flags: ignoreversion
Source: "License.txt"; DestDir: "{app}\licenses"; DestName: "README.txt"; Flags: ignoreversion

[Icons]
Name: "{autoprograms}\Phomic"; Filename: "{app}\Phomic.exe"
Name: "{autodesktop}\Phomic"; Filename: "{app}\Phomic.exe"; Tasks: desktopicon

[Run]
Filename: "{app}\Phomic.exe"; Description: "{cm:LaunchProgram,Phomic}"; Flags: nowait postinstall skipifsilent
; Phomic's own Update runs Setup silently with /RESTARTPHOMIC=1: start it again afterwards.
Filename: "{app}\Phomic.exe"; Flags: nowait; Check: Switch('RESTARTPHOMIC')

[Code]
function Switch(const Name: String): Boolean;
begin
  Result := ExpandConstant('{param:' + Name + '|0}') = '1';
end;

// Updating: first close the running Phomic the way Quit in its tray menu does (1.2.1 and later
// understand --quit). Otherwise Windows' Restart Manager closes only its window, and Setup waits
// at "Closing applications" until it gives up and ends the process.
function PrepareToInstall(var NeedsRestart: Boolean): String;
var
  Exe: String;
  VersionMS, VersionLS: Cardinal;
  ResultCode: Integer;
begin
  Result := '';
  Exe := ExpandConstant('{app}\Phomic.exe');
  if FileExists(Exe) and GetVersionNumbers(Exe, VersionMS, VersionLS) and
     ((VersionMS > $00010002) or ((VersionMS = $00010002) and (VersionLS >= $00010000))) then
    Exec(Exe, '--quit', '', SW_HIDE, ewWaitUntilTerminated, ResultCode);
end;

// VB-CABLE's own setup, which also removes it (VB-Audio's window, with a Remove Driver button).
function VBCableSetup: String;
begin
  Result := ExpandConstant('{commonpf64}\VB\CABLE\VBCABLE_Setup_x64.exe');
end;

procedure CurUninstallStepChanged(CurUninstallStep: TUninstallStep);
var
  ResultCode: Integer;
  Command: String;
begin
  // Phomic is gone: VB-CABLE stays unless the user wants it gone too. Other apps may use it,
  // and removing a driver needs VB-Audio's setup (and admin rights), so ask, and default to no.
  if (CurUninstallStep = usPostUninstall) and not UninstallSilent and not Switch('KEEPMICROPHONE') and
     FileExists(VBCableSetup) then
  begin
    // Short lines with breaks of their own: the box wraps long ones at odd places.
    if MsgBox('Also remove VB-CABLE?' + #13#10#13#10 +
      'It''s the audio driver behind Phomic''s microphone.' + #13#10 +
      'Other apps may use it too.' + #13#10#13#10 +
      'VB-Audio''s setup then opens: click Remove Driver.',
      mbConfirmation, MB_YESNO or MB_DEFBUTTON2) = IDYES then
      ShellExec('runas', VBCableSetup, '', '', SW_SHOWNORMAL, ewNoWait, ResultCode); // it needs admin rights
    Exit;
  end;

  if CurUninstallStep <> usUninstall then
    Exit;

  // Close this copy of Phomic (it may be running in the tray) so its files can be removed:
  // it asks it to quit, and ends it if it doesn't. Copies elsewhere are left alone.
  Exec(ExpandConstant('{app}\Phomic.exe'), '--quit', '', SW_HIDE, ewWaitUntilTerminated, ResultCode);

  // Original names for the VB-CABLE devices, both playback endpoints enabled again.
  if not Switch('KEEPMICROPHONE') then
    Exec(ExpandConstant('{app}\Phomic.exe'), '--restore-microphone', '', SW_HIDE, ewWaitUntilTerminated, ResultCode);

  if not Switch('KEEPSETTINGS') then
    RegDeleteKeyIncludingSubkeys(HKEY_CURRENT_USER, 'Software\Phomic');

  // "Launch app in tray when Windows starts" would point at a deleted file (but leave an
  // entry that belongs to another copy of Phomic alone).
  if RegQueryStringValue(HKEY_CURRENT_USER, 'Software\Microsoft\Windows\CurrentVersion\Run', 'Phomic', Command) and
     (Pos(Lowercase(ExpandConstant('{app}\Phomic.exe')), Lowercase(Command)) > 0) then
  begin
    RegDeleteValue(HKEY_CURRENT_USER, 'Software\Microsoft\Windows\CurrentVersion\Run', 'Phomic');
    RegDeleteValue(HKEY_CURRENT_USER, 'Software\Microsoft\Windows\CurrentVersion\Explorer\StartupApproved\Run', 'Phomic');
  end;
end;
