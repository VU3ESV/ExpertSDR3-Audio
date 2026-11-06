; Built by scripts/package-windows.ps1, which supplies the /D defines.

[Setup]
AppId={{7C4C6A3E-5F3B-4F0E-9B1A-2E5D8C7F4A11}
AppName=ExpertSDR3 Audio
AppVersion={#AppVersion}
AppPublisher=VU3ESV
AppPublisherURL=https://github.com/VU3ESV/ExpertSDR3-Audio
DefaultDirName={autopf}\ExpertSDR3 Audio
DefaultGroupName=ExpertSDR3 Audio
DisableProgramGroupPage=yes
LicenseFile=..\..\LICENSE
OutputDir={#OutputDir}
OutputBaseFilename={#OutputBase}
SetupIconFile=..\icons\ExpertSDR3Audio.ico
UninstallDisplayIcon={app}\ExpertSDR3Audio.exe
Compression=lzma2/max
SolidCompression=yes
ArchitecturesAllowed=x64compatible
ArchitecturesInstallIn64BitMode=x64compatible
; Installs for one user without an administrator prompt, unless asked for everyone.
PrivilegesRequired=lowest
PrivilegesRequiredOverridesAllowed=dialog
WizardStyle=modern

[Tasks]
Name: "desktopicon"; Description: "Create a desktop icon"; Flags: unchecked

[Files]
Source: "{#StageDir}\*"; DestDir: "{app}"; Flags: recursesubdirs ignoreversion

[Icons]
Name: "{group}\ExpertSDR3 Audio"; Filename: "{app}\ExpertSDR3Audio.exe"
Name: "{autodesktop}\ExpertSDR3 Audio"; Filename: "{app}\ExpertSDR3Audio.exe"; Tasks: desktopicon

[Run]
Filename: "{app}\ExpertSDR3Audio.exe"; Description: "Start ExpertSDR3 Audio"; Flags: nowait postinstall skipifsilent
