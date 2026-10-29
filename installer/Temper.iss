; Temper â€” Inno Setup installer
; Packages the CLAP and the VST3 bundle to their standard system-wide
; locations. Run build.bat first so build\dist\ is populated.

#define MyAppName "Temper"
#define MyAppVersion "0.3.0"
#define MyAppPublisher "Dewpoint AV"
#define MyAppURL "https://dewpoint.live"

[Setup]
AppId={{6D2E8F4A-1B3C-4D5E-8F9A-0B1C2D3E4F5A}
AppName={#MyAppName}
AppVersion={#MyAppVersion}
AppVerName={#MyAppName} {#MyAppVersion}
AppPublisher={#MyAppPublisher}
AppPublisherURL={#MyAppURL}
AppSupportURL={#MyAppURL}
AppUpdatesURL={#MyAppURL}
DefaultDirName={autopf}\Dewpoint AV\{#MyAppName}
DefaultGroupName=Dewpoint AV\{#MyAppName}
DisableProgramGroupPage=yes
DisableDirPage=yes
OutputDir=output
OutputBaseFilename={#MyAppName}-Setup-{#MyAppVersion}
Compression=lzma2
SolidCompression=yes
ArchitecturesAllowed=x64compatible
ArchitecturesInstallIn64BitMode=x64compatible
PrivilegesRequired=admin
WizardStyle=modern
SetupLogging=yes
UninstallDisplayName={#MyAppName} (Dewpoint AV)

[Languages]
Name: "english"; MessagesFile: "compiler:Default.isl"

[Files]
Source: "..\build\dist\Temper.vst3\*"; DestDir: "{commoncf}\VST3\{#MyAppName}.vst3"; Flags: ignoreversion recursesubdirs createallsubdirs
Source: "..\build\dist\Temper.clap"; DestDir: "{commoncf}\CLAP"; Flags: ignoreversion

[InstallDelete]
Type: filesandordirs; Name: "{commoncf}\VST3\Temper.vst3"
Type: files; Name: "{commoncf}\CLAP\Temper.clap"

[Icons]
Name: "{group}\{cm:UninstallProgram,{#MyAppName}}"; Filename: "{uninstallexe}"

[Messages]
WelcomeLabel1=Temper Dynamic Resonance Suppressor
WelcomeLabel2=This will install Temper (VST3 + CLAP) on your computer.%n%nClick Next to continue.
