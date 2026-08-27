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
; Was a bare literal, hand-edited and left stuck at "0.1.0" long after
; buildspec.json moved on -- the mismatch was invisible for regular commit
; builds (this .iss's own filename was still whatever it was, downloaded by
; its literal name) but silently broke the TAGGED release build: the
; installer landed in release/ as
; "obs-trigglow-dynamic-delay-0.1.0-windows-x64-setup.exe" while
; build-project.yaml's Upload Artifacts step (and push.yaml's Rename Files
; step) glob for "${pluginVersion}-windows-x64*", so the .exe just silently
; failed to match and never made it into the artifact/release at all
; (found live, 2026-08-26, missing from the 0.2.0 draft release). Now passed
; in from CI via `/DMyAppVersion=...`, matching the existing PluginStageDir
; override pattern below -- falls back to buildspec.json's value at the time
; of this fix for anyone building locally without passing it explicitly.
#ifndef MyAppVersion
  #define MyAppVersion "0.3.0"
#endif
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
; This installer always targets an EXISTING OBS Studio folder (that's the
; whole point — we're installing a plugin into it, not creating a new app
; folder), so Inno's default "this folder already exists, continue anyway?"
; warning is just confusing noise here, not a real "are you sure?" moment.
DirExistsWarning=no
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
; v0.3.0 Phase 0 (FFmpeg groundwork, docs/ROADMAP.md): CMake's install() rule
; already places these next to the plugin's own .dll in the packaged output
; (CMakeLists.txt's OS_WINDOWS block) specifically so Windows' DLL search
; order resolves them from here before anywhere else -- see that file's
; comment for the full reasoning. Found missing from this installer live,
; 2026-08-27: the .zip release asset had them (its own CMake install()
; output), but this .iss only listed the plugin .dll + locale file, so
; installing via the .exe (the documented primary path) would have shipped
; without them entirely.
Source: "{#PluginStageDir}\bin\64bit\avcodec-63.dll"; DestDir: "{app}\obs-plugins\64bit"; Flags: ignoreversion
Source: "{#PluginStageDir}\bin\64bit\avutil-61.dll"; DestDir: "{app}\obs-plugins\64bit"; Flags: ignoreversion
; swresample-7.dll: avcodec-63.dll has a real (non-delay-loaded) dependency
; on it -- missing from here caused "LoadLibrary failed ... (126)" the
; moment Phase 1's real avcodec_* calls made this DLL actually load for the
; first time (found live, 2026-08-27; see CMakeLists.txt's OS_WINDOWS block
; for the full story -- this had been silently missing since Phase 0, just
; never exercised until now).
Source: "{#PluginStageDir}\bin\64bit\swresample-7.dll"; DestDir: "{app}\obs-plugins\64bit"; Flags: ignoreversion

[UninstallDelete]
Type: dirifempty; Name: "{app}\data\obs-plugins\obs-trigglow-dynamic-delay\locale"
Type: dirifempty; Name: "{app}\data\obs-plugins\obs-trigglow-dynamic-delay"

[Run]
; "postinstall skipifsilent" puts this as a checkbox on the Finished page
; (checked by default) instead of running unconditionally -- the user still
; gets asked, just via a checkbox instead of a separate prompt. {app} (not
; GetObsDir again) so this launches whatever folder was actually chosen,
; even if the user picked a different one than the auto-detected default.
Filename: "{app}\bin\64bit\obs64.exe"; Description: "{cm:LaunchProgram,OBS Studio}"; Flags: nowait postinstall skipifsilent

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
