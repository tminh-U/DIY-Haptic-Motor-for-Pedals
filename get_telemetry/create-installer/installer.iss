; Inno Setup packaging script for Haptic Brake Control (DIY Haptic Motor for Pedals)
#define MyAppName "Haptic Brake Control"
#define MyAppVersion "1.0.3"
#define MyAppPublisher "tminh"
#define MyAppURL "https://github.com/tminh-U/DIY-Haptic-Motor-for-Pedals"
#define MyAppExeName "get_telemetry.exe"

[Setup]
AppId={{D3F9E2A1-8B4C-4E7B-9021-DIYHAPTIC001}
AppName={#MyAppName}
AppVersion={#MyAppVersion}
AppVerName={#MyAppName}
UninstallDisplayName={#MyAppName}
AppPublisher={#MyAppPublisher}
AppPublisherURL={#MyAppURL}
AppSupportURL={#MyAppURL}
AppUpdatesURL={#MyAppURL}
DefaultDirName={autopf}\{#MyAppName}
DefaultGroupName={#MyAppName}
AllowNoIcons=yes
LicenseFile=..\..\LICENSE
SetupIconFile=..\app.ico
OutputDir=Output
OutputBaseFilename=HapticBrakeControl_Setup
Compression=lzma2/ultra64
SolidCompression=yes
WizardStyle=modern
ArchitecturesInstallIn64BitMode=x64compatible
DisableProgramGroupPage=yes
PrivilegesRequiredOverridesAllowed=dialog
CloseApplications=yes
RestartApplications=no
AppMutex=Local\HapticBrakeControl.SingleInstance

[Languages]
Name: "english"; MessagesFile: "compiler:Default.isl"

[Tasks]
Name: "desktopicon"; Description: "{cm:CreateDesktopIcon}"; GroupDescription: "{cm:AdditionalIcons}"

[Files]
; Main application executable
Source: "..\get_telemetry.exe"; DestDir: "{app}"; Flags: ignoreversion

; Application icon
Source: "..\app.ico"; DestDir: "{app}"; Flags: ignoreversion

; Google Sans Flex is loaded privately by the app, so it is not installed system-wide
Source: "..\fonts\GoogleSansFlex-Regular.ttf"; DestDir: "{app}\fonts"; Flags: ignoreversion
Source: "..\fonts\GoogleSansFlex-Medium.ttf"; DestDir: "{app}\fonts"; Flags: ignoreversion
Source: "..\fonts\OFL.txt"; DestDir: "{app}\fonts"; DestName: "OFL-Google-Sans-Flex.txt"; Flags: ignoreversion

; Bundled Arduino CLI (optional, used by the built-in firmware flasher)
Source: "..\arduino-cli.exe"; DestDir: "{app}"; Flags: ignoreversion skipifsourcedoesntexist

; License files
Source: "..\..\LICENSE"; DestDir: "{app}"; DestName: "LICENSE.txt"; Flags: ignoreversion
Source: "LICENSE-arduino-cli.txt"; DestDir: "{app}"; Flags: ignoreversion skipifsourcedoesntexist

; Assetto Corsa Python application bundle
Source: "..\..\assetto_corsa_python_app\haptic_telemetry\*"; DestDir: "{app}\assetto_corsa_app\haptic_telemetry"; Flags: ignoreversion recursesubdirs createallsubdirs

[Icons]
Name: "{group}\{#MyAppName}"; Filename: "{app}\{#MyAppExeName}"; IconFilename: "{app}\app.ico"
Name: "{group}\Assetto Corsa Python App Folder"; Filename: "{app}\assetto_corsa_app\haptic_telemetry"
Name: "{group}\{cm:UninstallProgram,{#MyAppName}}"; Filename: "{uninstallexe}"
Name: "{autodesktop}\{#MyAppName}"; Filename: "{app}\{#MyAppExeName}"; IconFilename: "{app}\app.ico"; Tasks: desktopicon

[Run]
Filename: "{app}\{#MyAppExeName}"; Description: "{cm:LaunchProgram,{#StringChange(MyAppName, '&', '&&')}}"; Flags: nowait postinstall skipifsilent
