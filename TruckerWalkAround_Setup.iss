; Script generated for Inno Setup 6
; TruckerWalkAround Setup Script

#define MyAppName "TruckerWalkAround"
#define MyAppVersion "1.0.1"
#define MyAppPublisher "TruckerWalkAround Team"
#define MyAppExeName "TruckerWalkAround.exe"

[Setup]
AppId={{9F57BA7E-B8B4-4D6C-8B3E-B5E3C756F311}
AppName={#MyAppName}
AppVersion={#MyAppVersion}
AppPublisher={#MyAppPublisher}
DefaultDirName={autopf}\{#MyAppName}
DisableProgramGroupPage=yes
OutputDir=d:\ATS_MOD\walk_free\TruckerWalkAround
OutputBaseFilename=TruckerWalkAround_Setup_v1.0.1
SetupIconFile=d:\ATS_MOD\walk_free\TruckerWalkAround\TWAico.ico
Compression=lzma2/ultra64
SolidCompression=yes
WizardStyle=modern
PrivilegesRequired=lowest

[Languages]
Name: "japanese"; MessagesFile: "compiler:Languages\Japanese.isl"
Name: "english"; MessagesFile: "compiler:Default.isl"

[Tasks]
Name: "desktopicon"; Description: "{cm:CreateDesktopIcon}"; GroupDescription: "{cm:AdditionalIcons}"

[Files]
Source: "d:\ATS_MOD\walk_free\TruckerWalkAround\TruckerWalkAround.exe"; DestDir: "{app}"; Flags: ignoreversion
Source: "d:\ATS_MOD\walk_free\TruckerWalkAround\TruckerWalkAround_ATS.dll"; DestDir: "{app}"; Flags: ignoreversion
Source: "d:\ATS_MOD\walk_free\TruckerWalkAround\TruckerWalkAround_ETS2.dll"; DestDir: "{app}"; Flags: ignoreversion
Source: "d:\ATS_MOD\walk_free\TruckerWalkAround\README.txt"; DestDir: "{app}"; Flags: ignoreversion
Source: "d:\ATS_MOD\walk_free\TruckerWalkAround\CHANGELOG.txt"; DestDir: "{app}"; Flags: ignoreversion
Source: "d:\ATS_MOD\walk_free\TruckerWalkAround\TWAico.ico"; DestDir: "{app}"; Flags: ignoreversion

[Icons]
Name: "{autoprograms}\{#MyAppName}"; Filename: "{app}\{#MyAppExeName}"; IconFilename: "{app}\TWAico.ico"
Name: "{autodesktop}\{#MyAppName}"; Filename: "{app}\{#MyAppExeName}"; IconFilename: "{app}\TWAico.ico"; Tasks: desktopicon

[Run]
Filename: "{app}\{#MyAppExeName}"; Description: "{cm:LaunchProgram,{#StringChange(MyAppName, '&', '&&')}}"; Flags: nowait postinstall skipifsilent
