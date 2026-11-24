#ifndef PayloadDir
  #error PayloadDir must point to the verified, extracted Windows release
#endif
[Setup]
AppId={{1A8CDD07-D4C5-4F9E-9788-08636A6D7C31}
AppName=AC:Unreal and AC:VR
AppVersion={#ProductVersion}
AppPublisher=Thwargle
AppPublisherURL=https://thwargle.com/unreal/
AppSupportURL=https://thwargle.com/unreal/#help
DefaultDirName={localappdata}\Programs\ACUnreal
DefaultGroupName=AC Unreal
PrivilegesRequired=lowest
ArchitecturesAllowed=x64compatible
ArchitecturesInstallIn64BitMode=x64compatible
MinVersion=10.0
DisableProgramGroupPage=yes
WizardStyle=modern
WizardImageFile={#BrandWizard}
WizardSmallImageFile={#BrandMark}
WizardImageBackColor=$00181B11
SetupIconFile={#BrandIcon}
UninstallDisplayIcon={app}\AC-Icon.ico
Uninstallable=not IsPortableUpdate
CreateUninstallRegKey=not IsPortableUpdate
OutputDir={#ReleaseDir}
OutputBaseFilename=AC-Unreal-Setup-v{#ReleaseNumber}
Compression=lzma2/fast
SolidCompression=yes
CloseApplications=yes
RestartApplications=no
LicenseFile={#PayloadDir}\LICENSE
InfoBeforeFile={#PayloadDir}\README-WINDOWS.txt
VersionInfoVersion={#ProductVersion}
VersionInfoProductName=AC:Unreal and AC:VR
VersionInfoDescription=AC:Unreal and AC:VR Setup

[Tasks]
Name: desktopicon; Description: "Create an AC:Unreal desktop shortcut"; GroupDescription: "Shortcuts:"; Flags: checkedonce
Name: vrdesktopicon; Description: "Create an AC:VR (SteamVR) desktop shortcut"; GroupDescription: "Shortcuts:"; Flags: unchecked

[Files]
Source: "{#PayloadDir}\*"; DestDir: "{app}"; Flags: ignoreversion recursesubdirs createallsubdirs
Source: "{#BrandIcon}"; DestDir: "{app}"; DestName: "AC-Icon.ico"; Flags: ignoreversion
Source: "{#Prerequisites}"; DestDir: "{app}\Prerequisites"; Flags: ignoreversion
Source: "{#GameInput}"; DestDir: "{app}\Prerequisites"; Flags: ignoreversion

[Icons]
Name: "{group}\AC Unreal"; Filename: "{app}\ACUnreal.exe"; Parameters: "-nohmd"; Check: not IsPortableUpdate; WorkingDir: "{app}"; IconFilename: "{app}\AC-Icon.ico"
Name: "{group}\AC VR (SteamVR)"; Filename: "{app}\AC-VR.bat"; Check: not IsPortableUpdate; WorkingDir: "{app}"; IconFilename: "{app}\AC-Icon.ico"
Name: "{group}\Setup guide"; Filename: "https://thwargle.com/unreal/"; Check: not IsPortableUpdate
Name: "{autodesktop}\AC Unreal"; Filename: "{app}\ACUnreal.exe"; Parameters: "-nohmd"; Check: not IsPortableUpdate; WorkingDir: "{app}"; IconFilename: "{app}\AC-Icon.ico"; Tasks: desktopicon
Name: "{autodesktop}\AC VR (SteamVR)"; Filename: "{app}\AC-VR.bat"; Check: not IsPortableUpdate; WorkingDir: "{app}"; IconFilename: "{app}\AC-Icon.ico"; Tasks: vrdesktopicon

[Run]
Filename: "{app}\Prerequisites\vc_redist.x64.exe"; Parameters: "/install /norestart"; Description: "Install or update Microsoft Visual C++ runtime (required)"; Check: NeedsVisualCppRuntime; Flags: postinstall shellexec skipifsilent waituntilterminated
Filename: "{app}\Prerequisites\GameInputRedist.msi"; Description: "Install or update Microsoft GameInput (required)"; Check: NeedsGameInputRuntime; Flags: postinstall shellexec skipifsilent waituntilterminated
Filename: "{app}\ACUnreal.exe"; Parameters: "-nohmd"; Description: "Open AC:Unreal"; WorkingDir: "{app}"; Flags: postinstall nowait skipifsilent unchecked

[Messages]
WelcomeLabel2=This installs the AC:Unreal desktop client and AC:VR for SteamVR.%n%nYou will need your own updated Asheron's Call DAT files and a community server account. The setup guide is available at thwargle.com/unreal.%n%nFor native Quest installation, use the separate AC:VR Quest installer.

[Code]
#include "Windows-Prerequisites.iss"

procedure CurStepChanged(CurStep: TSetupStep);
var
  ProjectDir: String;
begin
  ProjectDir := ExpandConstant('{app}\ACUnreal');
  if (CurStep = ssInstall) and FileExists(ExpandConstant('{app}\ACUnreal.exe'))
     and not FileExists(ProjectDir + '\catalog-client.txt') then begin
    ForceDirectories(ProjectDir);
    SaveStringToFile(ProjectDir + '\legacy-client-install.txt', 'upgrade', False);
  end;
  if CurStep = ssPostInstall then
    SaveStringToFile(ProjectDir + '\catalog-client.txt', '1', False);
end;

function IsPortableUpdate(): Boolean;
begin
  Result := ExpandConstant('{param:ACPORTABLE|0}') = '1';
end;
