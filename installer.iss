; Inno Setup Script for CEFDecklink
; This script creates an installer for the CEFDecklink application

#define MyAppName "CEFDecklink"
#define MyAppVersion "1.0.0"
#define MyAppPublisher "CEFDecklink Project"
#define MyAppExeName "DeckLinkDX11.exe"

[Setup]
; Basic application information
AppId={{8F9A3B2C-1D4E-5F6A-7B8C-9D0E1F2A3B4C}
AppName={#MyAppName}
AppVersion={#MyAppVersion}
AppPublisher={#MyAppPublisher}
DefaultDirName={localappdata}\{#MyAppName}
DefaultGroupName={#MyAppName}
DisableProgramGroupPage=yes
OutputDir=build
OutputBaseFilename=CEFDecklink-Setup
Compression=lzma2/max
SolidCompression=yes
WizardStyle=modern
ArchitecturesInstallIn64BitMode=x64

; Privileges
PrivilegesRequired=lowest
PrivilegesRequiredOverridesAllowed=dialog

; Uninstall
UninstallDisplayIcon={app}\{#MyAppExeName}

[Languages]
Name: "english"; MessagesFile: "compiler:Default.isl"
Name: "japanese"; MessagesFile: "compiler:Languages\Japanese.isl"

[Tasks]
Name: "desktopicon"; Description: "{cm:CreateDesktopIcon}"; GroupDescription: "{cm:AdditionalIcons}"; Flags: unchecked

[Files]
; Main executable
Source: "build\Release\{#MyAppExeName}"; DestDir: "{app}"; Flags: ignoreversion

; Configuration file
Source: "build\Release\config.json"; DestDir: "{app}"; Flags: onlyifdoesntexist
Source: "build\Release\licensekey.json"; DestDir: "{app}"; Flags: onlyifdoesntexist skipifsourcedoesntexist

; CEF binaries and libraries
Source: "build\Release\*.dll"; DestDir: "{app}"; Flags: ignoreversion
Source: "build\Release\*.exe"; DestDir: "{app}"; Flags: ignoreversion
Source: "build\Release\*.bin"; DestDir: "{app}"; Flags: ignoreversion
Source: "build\Release\*.dat"; DestDir: "{app}"; Flags: ignoreversion
Source: "build\Release\vk_swiftshader_icd.json"; DestDir: "{app}"; Flags: ignoreversion

; CEF resources
Source: "build\Release\*.pak"; DestDir: "{app}"; Flags: ignoreversion

; Shaders
Source: "build\Release\shaders\*"; DestDir: "{app}\shaders"; Flags: ignoreversion recursesubdirs

; Essential locales only (Japanese and English)
Source: "build\Release\locales\ja*.pak"; DestDir: "{app}\locales"; Flags: ignoreversion
Source: "build\Release\locales\en-US*.pak"; DestDir: "{app}\locales"; Flags: ignoreversion

[Icons]
Name: "{group}\{#MyAppName}"; Filename: "{app}\{#MyAppExeName}"
Name: "{group}\{cm:UninstallProgram,{#MyAppName}}"; Filename: "{uninstallexe}"
Name: "{autodesktop}\{#MyAppName}"; Filename: "{app}\{#MyAppExeName}"; Tasks: desktopicon

[UninstallDelete]
Type: filesandordirs; Name: "{app}\cache"
Type: filesandordirs; Name: "{app}\*.log"

[Code]
function InitializeSetup(): Boolean;
begin
  Result := True;
end;
