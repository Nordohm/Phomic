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
LicenseFile=..\..\LICENSE
SetupIconFile=..\Phomic\Assets\Phomic.ico
UninstallDisplayIcon={app}\Phomic.exe
UninstallDisplayName=Phomic
OutputDir=..\release
OutputBaseFilename=Phomic-Setup-{#AppVersion}
Compression=lzma2/max
SolidCompression=yes
WizardStyle=modern
; Updating while Phomic runs (it may be in the tray): close it first.
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

[Icons]
Name: "{autoprograms}\Phomic"; Filename: "{app}\Phomic.exe"
Name: "{autodesktop}\Phomic"; Filename: "{app}\Phomic.exe"; Tasks: desktopicon

[Run]
Filename: "{app}\Phomic.exe"; Description: "{cm:LaunchProgram,Phomic}"; Flags: nowait postinstall skipifsilent

[Code]
function Switch(const Name: String): Boolean;
begin
  Result := ExpandConstant('{param:' + Name + '|0}') = '1';
end;

procedure CurUninstallStepChanged(CurUninstallStep: TUninstallStep);
var
  ResultCode: Integer;
  Command: String;
begin
  if CurUninstallStep <> usUninstall then
    Exit;

  // Close this copy of Phomic (it may be running in the tray); leave other copies alone.
  Exec(ExpandConstant('{sys}\WindowsPowerShell\v1.0\powershell.exe'),
    '-NoProfile -Command "Get-Process Phomic -ErrorAction SilentlyContinue | Where-Object { $_.Path -eq ''' +
    ExpandConstant('{app}\Phomic.exe') + ''' } | Stop-Process -Force"',
    '', SW_HIDE, ewWaitUntilTerminated, ResultCode);

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
