; Trigglow Dynamic Delay for OBS -- Windows installer (Inno Setup 6)
;
; Unsigned by default: building this in CI produces a working .exe, but
; without a code-signing certificate Windows SmartScreen will show an
; "Unknown publisher" warning on first run (click "More info" -> "Run
; anyway"). Normal for Early Access software; see README.md for the
; signing tradeoff.
;
; Detects the OBS Studio install path from the registry key the official
; OBS installer writes (HKLM\SOFTWARE\OBS Studio, default value), falling
; back to the standard Program Files path if that key is missing, and lets
; the user pick a different folder either way (DisableDirPage=no).
;
; Build locally: "C:\Program Files (x86)\Inno Setup 6\ISCC.exe" trigglow-dynamic-delay-setup.iss
; (expects the compiled plugin under build_x64\RelWithDebInfo\, matching
; the CMake preset output layout -- see PLUGIN_STAGE_DIR below to point at
; a different location, e.g. an already-extracted release zip.)

#define MyAppName "Trigglow Dynamic Delay"
#define MyAppVersion "0.1.0"
#define MyAppPublisher "Trigglow (VirosMs)"
#define MyAppURL "https://trigglow.virosms.com/dynamic-delay"

; Directory containing bin\64bit\obs-trigglow-dynamic-delay.dll and
; data\locale\en-US.ini, i.e. the same layout the release .zip ships.
; Override at compile time with: ISCC.exe /DPluginStageDir=path ...
#ifndef PluginStageDir
  #define PluginStageDir "..\..\build_x64\RelWithDebInfo"
#endif

[Setup]
AppId={{B7F3A6E2-9C4D-4A1F-8E2B-6D5C1F3A9B7E}
AppName={#MyAppName}
AppVersion={#MyAppVersion}
AppPublisher={#MyAppPublisher}
AppPublisherURL={#MyAppURL}
AppSupportURL={#MyAppURL}
AppUpdatesURL={#MyAppURL}
DefaultDirName={code:GetObsDir}
DisableDirPage=no
DisableProgramGroupPage=yes
DisableReadyPage=yes
PrivilegesRequired=admin
ArchitecturesAllowed=x64compatible
ArchitecturesInstallIn64BitMode=x64compatible
; Matches buildspec.json's "name" (not "displayName") plus the existing
; -windows-x64* naming convention build-project.yaml's Upload Artifacts step
; already globs for, so this installer rides along with the .zip without
; needing a separate upload/artifact step.
OutputBaseFilename=obs-trigglow-dynamic-delay-{#MyAppVersion}-windows-x64-setup
OutputDir=..\..\release
Compression=lzma
SolidCompression=yes
WizardStyle=modern
UninstallDisplayName={#MyAppName}
DisableWelcomePage=no

[Languages]
Name: "spanish"; MessagesFile: "compiler:Languages\Spanish.isl"
Name: "english"; MessagesFile: "compiler:Default.isl"

[Files]
Source: "{#PluginStageDir}\bin\64bit\obs-trigglow-dynamic-delay.dll"; DestDir: "{app}\obs-plugins\64bit"; Flags: ignoreversion
Source: "{#PluginStageDir}\data\locale\en-US.ini"; DestDir: "{app}\data\obs-plugins\obs-trigglow-dynamic-delay\locale"; Flags: ignoreversion

[UninstallDelete]
Type: dirifempty; Name: "{app}\data\obs-plugins\obs-trigglow-dynamic-delay\locale"
Type: dirifempty; Name: "{app}\data\obs-plugins\obs-trigglow-dynamic-delay"

[Code]
function GetObsDir(Param: string): string;
var
  RegPath: string;
begin
  if RegQueryStringValue(HKLM, 'SOFTWARE\OBS Studio', '', RegPath) and (RegPath <> '') then
    Result := RegPath
  else
    Result := ExpandConstant('{autopf}\obs-studio');
end;

function InitializeSetup(): Boolean;
begin
  Result := True;
  if not FileExists(GetObsDir('') + '\bin\64bit\obs64.exe') then
  begin
    if MsgBox('No se detecto una instalacion de OBS Studio en "' + GetObsDir('') + '". '
      + 'Puedes continuar y elegir la carpeta correcta en el siguiente paso, o cancelar '
      + 'e instalar OBS Studio primero.'#13#10#13#10'Continuar de todas formas?',
      mbConfirmation, MB_YESNO) = IDNO then
      Result := False;
  end;
end;

[Messages]
FinishedLabel=Instalacion completada. Reinicia OBS Studio para que cargue el plugin -- deberia aparecer en Vista > Paneles como "Trigglow Dynamic Delay".
