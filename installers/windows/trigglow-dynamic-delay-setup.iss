; Trigglow Dynamic Delay for OBS -- Windows installer (Inno Setup 6)
;
; Unsigned by default: building this in CI produces a working .exe, but
; without a code-signing certificate Windows SmartScreen will show an
; "Unknown publisher" warning on first run (click "More info" -> "Run
; anyway"). Normal for Early Access software; see README.md for the
; signing tradeoff.
;
; v0.7.1: installs into OBS's plugin folder for all users,
; C:\ProgramData\obs-studio\plugins\obs-trigglow-dynamic-delay, never into
; OBS's own folder any more. OBS 33 marks plugins inside its folder as
; "Legacy" and OBS 34 stops loading them. Two copies of the DLL, one data
; folder (the layout cmake --install and the release .zip use too):
;   <folder>\obs-trigglow-dynamic-delay.dll + FFmpeg DLLs   OBS 33 and later
;   <folder>\bin\64bit\  the same DLLs                       OBS 31 and 32
;   <folder>\data\                                           both
; OBS 33 skips the bin\64bit copy as a legacy duplicate. Each copy has its own
; FFmpeg DLLs next to it: libobs loads a module with
; LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR, so its imports are only looked for in the
; module's own folder.
;
; Before installing (PrepareToInstall):
; - an install of 0.7.0 or earlier inside OBS's folder (this installer's own
;   uninstall entry, same AppId) is removed with its own uninstaller, once the
;   entry's folder and uninstaller are checked to be ours;
; - the plugin folder's permissions are reset: Administrators and SYSTEM
;   write, Users only read and run. ProgramData lets any user create files
;   in its subfolders, and a DLL another user drops next to ours would be
;   loaded into OBS (DLL planting).
; After installing, anything of ours still left in OBS's folder (a .zip
; extracted there, an uninstall entry that was missing) is deleted: our DLL,
; our data folder, and the FFmpeg DLLs only when our DLL was there too and
; they are byte for byte the ones this installer ships. OBS's plugin_config
; folder (your settings and sign-in) is never touched.
;
; Build locally: "C:\Program Files (x86)\Inno Setup 6\ISCC.exe" trigglow-dynamic-delay-setup.iss
; (expects the cmake --install output under release\RelWithDebInfo\obs-trigglow-dynamic-delay\,
; see PluginStageDir below to point at a different location, e.g. an
; already-extracted release zip).

#define MyAppName "Trigglow Dynamic Delay"
#define PluginName "obs-trigglow-dynamic-delay"
#define MyAppPublisher "Trigglow (VirosMs)"
#define MyAppURL "https://www.trigglow.com/dynamic-delay"

; The version and the oldest OBS Studio supported come from buildspec.json
; (its top-level "version" and obsCompat's "minimum"), the only place they
; are set. CI may still pass /DMyAppVersion=... or /DObsMinVersion=...; both
; then win over the file.
#define JsonValue(str S) Copy(Copy(S, Pos('": "', S) + 4), 1, Pos('"', Copy(S, Pos('": "', S) + 4)) - 1)
#define BuildSpecFile AddBackslash(SourcePath) + "..\..\buildspec.json"
#define BuildSpecHandle 0
#define BuildSpecLine ""
#define BuildSpecVersion ""
#define BuildSpecObsMin ""
#sub ReadBuildSpecLine
  #expr BuildSpecLine = FileRead(BuildSpecHandle)
  #if Pos('    "version": "', BuildSpecLine) == 1
    #expr BuildSpecVersion = JsonValue(BuildSpecLine)
  #elif Pos('"minimum": "', BuildSpecLine) > 0
    #expr BuildSpecObsMin = JsonValue(BuildSpecLine)
  #endif
#endsub
#expr BuildSpecHandle = FileOpen(BuildSpecFile)
#if !BuildSpecHandle
  #error Cannot open buildspec.json at the root of this repository
#endif
#for {0; !FileEof(BuildSpecHandle); 0} ReadBuildSpecLine
#expr FileClose(BuildSpecHandle)

#ifndef MyAppVersion
  #define MyAppVersion BuildSpecVersion
#endif
#ifndef ObsMinVersion
  #define ObsMinVersion BuildSpecObsMin
#endif
#if MyAppVersion == "" || ObsMinVersion == ""
  #error buildspec.json has no top-level "version" or no obsCompat "minimum"
#endif
#pragma message "Trigglow Dynamic Delay " + MyAppVersion + ", OBS Studio " + ObsMinVersion + " or newer"

; Directory containing obs-trigglow-dynamic-delay.dll, bin\64bit\ and data\,
; i.e. the root folder of the release .zip.
; Override at compile time with: ISCC.exe /DPluginStageDir=path ...
#ifndef PluginStageDir
  #define PluginStageDir "..\..\release\RelWithDebInfo\" + PluginName
#endif

[Setup]
; Unchanged since the first release: an update keeps (and rewrites) the same
; uninstall entry, which is how the 0.7.0 install inside OBS's folder is found.
AppId={{B7F3A6E2-9C4D-4A1F-8E2B-6D5C1F3A9B7E}
AppName={#MyAppName}
AppVersion={#MyAppVersion}
AppPublisher={#MyAppPublisher}
AppPublisherURL={#MyAppURL}
AppSupportURL={#MyAppURL}
AppUpdatesURL={#MyAppURL}
VersionInfoVersion={#MyAppVersion}
; OBS's plugin folder for all users: fixed, there is nothing to choose. Not
; the folder an earlier version used either (UsePreviousAppDir=no): that was
; OBS's own folder.
DefaultDirName={commonappdata}\obs-studio\plugins\{#PluginName}
DisableDirPage=yes
UsePreviousAppDir=no
DirExistsWarning=no
DisableProgramGroupPage=yes
DisableReadyPage=yes
PrivilegesRequired=admin
ArchitecturesAllowed=x64compatible
ArchitecturesInstallIn64BitMode=x64compatible
; Never restart OBS from an elevated installer: the [Run] entry below starts it
; as the signed-in user, if they tick the box.
RestartApplications=no
SetupLogging=yes
; Matches buildspec.json's "name" (not "displayName") plus the existing
; -windows-x64* naming convention build-project.yaml's Upload Artifacts step
; already globs for, so this installer rides along with the .zip without
; needing a separate upload/artifact step.
OutputBaseFilename={#PluginName}-{#MyAppVersion}-windows-x64-setup
OutputDir=..\..\release
Compression=lzma
SolidCompression=yes
WizardStyle=modern
UninstallDisplayName={#MyAppName}
DisableWelcomePage=no
; Trigglow branding -- generated from a dedicated Dynamic Delay icon (from
; Trigglow's web brand assets) via
; installers/windows/branding/generate-assets.py, which also documents the
; exact Inno Setup size/aspect-ratio requirements these were built against.
; SetupIconFile becomes the .exe's own icon (Explorer,
; taskbar); WizardImageFile is the tall banner on the Welcome/Finished
; pages; WizardSmallImageFile is the small logo shown top-right of every
; other page.
SetupIconFile=branding\setup-icon.ico
WizardImageFile=branding\wizard-image.png
WizardSmallImageFile=branding\wizard-small.png

[Languages]
Name: "spanish"; MessagesFile: "compiler:Languages\Spanish.isl"
Name: "english"; MessagesFile: "compiler:Default.isl"

[CustomMessages]
spanish.ObsNotFound=No se ha encontrado OBS Studio en este equipo.%n%nEl plugin se instalará en la carpeta de plugins de OBS y se cargará cuando instales OBS Studio %1 o posterior.%n%n¿Continuar?
english.ObsNotFound=OBS Studio was not found on this computer.%n%nThe plugin will be installed in OBS's plugin folder and will load once you install OBS Studio %1 or newer.%n%nContinue?
spanish.ObsTooOld=Trigglow Dynamic Delay %1 necesita OBS Studio %2 o posterior, y el OBS Studio de "%3" es la versión %4.%n%nActualiza OBS Studio y vuelve a ejecutar este instalador.
english.ObsTooOld=Trigglow Dynamic Delay %1 requires OBS Studio %2 or newer, and the OBS Studio in "%3" is version %4.%n%nUpdate OBS Studio, then run this installer again.
spanish.ObsRunning=OBS Studio está abierto. Ciérralo (Archivo > Salir) y pulsa Reintentar.%n%nOBS carga el plugin al arrancar, y la actualización tiene que cambiar archivos que OBS usa mientras está abierto.
english.ObsRunning=OBS Studio is open. Close it (File > Exit), then click Retry.%n%nOBS loads the plugin when it starts, and the update has to replace files OBS uses while it is open.
spanish.ObsStillRunning=OBS Studio sigue abierto. Ciérralo (Archivo > Salir) y vuelve a ejecutar este instalador.
english.ObsStillRunning=OBS Studio is still open. Close it (File > Exit), then run this installer again.
spanish.AclFailed=No se han podido fijar los permisos de "%1" (icacls, código %2). La instalación se ha detenido para que ningún otro usuario pueda dejar archivos junto al plugin.
english.AclFailed=Could not set the permissions of "%1" (icacls, exit code %2). Setup stopped so that no other user can place files next to the plugin.

[Messages]
spanish.FinishedLabel=Instalación completada. Abre OBS Studio: el panel "Trigglow Dynamic Delay" aparece en Ver > Paneles (Docks).
english.FinishedLabel=Setup is complete. Open OBS Studio: the "Trigglow Dynamic Delay" dock is under View > Docks.

[Dirs]
; The folder's permissions are reset by LockDownPluginFolder() below, before
; any file is copied: Inno's Permissions only add entries, and ProgramData's
; inherited "Users: create files" entry has to go. This line states the same
; rights for the record.
Name: "{app}"; Permissions: admins-full system-full users-readexec

[Files]
; OBS 33 and later: <folder>\<name>.dll, FFmpeg next to it.
Source: "{#PluginStageDir}\{#PluginName}.dll"; DestDir: "{app}"; Flags: ignoreversion
; FFmpeg DLLs (LGPL; CMakeLists.txt's OS_WINDOWS block). swresample-7.dll:
; avcodec-63.dll has a real (non-delay-loaded) dependency on it.
Source: "{#PluginStageDir}\avcodec-63.dll"; DestDir: "{app}"; Flags: ignoreversion
Source: "{#PluginStageDir}\avutil-61.dll"; DestDir: "{app}"; Flags: ignoreversion
Source: "{#PluginStageDir}\swresample-7.dll"; DestDir: "{app}"; Flags: ignoreversion
; OBS 31 and 32: <folder>\bin\64bit\<name>.dll, with its own FFmpeg copies.
; The same source files as above (the stage's bin\64bit copies are identical):
; Inno stores a source file used twice only once, so the second copy adds
; nothing to the download.
Source: "{#PluginStageDir}\{#PluginName}.dll"; DestDir: "{app}\bin\64bit"; Flags: ignoreversion
Source: "{#PluginStageDir}\avcodec-63.dll"; DestDir: "{app}\bin\64bit"; Flags: ignoreversion
Source: "{#PluginStageDir}\avutil-61.dll"; DestDir: "{app}\bin\64bit"; Flags: ignoreversion
Source: "{#PluginStageDir}\swresample-7.dll"; DestDir: "{app}\bin\64bit"; Flags: ignoreversion
; One data folder for both. Listed one by one, so a staged build without any
; of them fails to compile here instead of quietly packaging an installer
; without it.
Source: "{#PluginStageDir}\data\locale\en-US.ini"; DestDir: "{app}\data\locale"; Flags: ignoreversion
Source: "{#PluginStageDir}\data\locale\es-ES.ini"; DestDir: "{app}\data\locale"; Flags: ignoreversion
; v0.5.x overlay badge frame, kept as a fallback (src/overlay-badge.cpp).
Source: "{#PluginStageDir}\data\images\delay-badge.png"; DestDir: "{app}\data\images"; Flags: ignoreversion
; Brand fonts for the dock and the overlay (Archivo, Geist Mono; SIL OFL 1.1),
; with their licenses next to them as the OFL requires.
Source: "{#PluginStageDir}\data\fonts\*.ttf"; DestDir: "{app}\data\fonts"; Flags: ignoreversion
Source: "{#PluginStageDir}\data\fonts\OFL-*.txt"; DestDir: "{app}\data\fonts"; Flags: ignoreversion
; Notices of third-party components: the Lucide icons compiled into the plugin
; (ISC) and the FFmpeg DLLs (LGPL 3.0 or later, with the build's tag and its
; source).
Source: "{#PluginStageDir}\data\licenses\LICENSE-lucide.txt"; DestDir: "{app}\data\licenses"; Flags: ignoreversion
Source: "{#PluginStageDir}\data\licenses\LICENSE-ffmpeg.txt"; DestDir: "{app}\data\licenses"; Flags: ignoreversion

[UninstallDelete]
Type: dirifempty; Name: "{app}\data\locale"
Type: dirifempty; Name: "{app}\data\images"
Type: dirifempty; Name: "{app}\data\fonts"
Type: dirifempty; Name: "{app}\data\licenses"
Type: dirifempty; Name: "{app}\data"
Type: dirifempty; Name: "{app}\bin\64bit"
Type: dirifempty; Name: "{app}\bin"
Type: dirifempty; Name: "{app}"

[Run]
; obs64.exe lives in OBS's own folder, not in {app}, and needs bin\64bit as
; its working folder (it finds its locale files from there). Started as the
; signed-in user, never elevated, and only when OBS was found.
Filename: "{code:GetObsExe}"; WorkingDir: "{code:GetObsBinDir}"; Description: "{cm:LaunchProgram,OBS Studio}"; Flags: nowait postinstall skipifsilent runasoriginaluser; Check: ObsExeExists

[Code]
const
  UninstallKey = 'Software\Microsoft\Windows\CurrentVersion\Uninstall\{B7F3A6E2-9C4D-4A1F-8E2B-6D5C1F3A9B7E}_is1';
  PluginName = '{#PluginName}';
  ObsMinVersion = '{#ObsMinVersion}';
  // OBS's single-instance mutex (frontend/utility/platform-windows.cpp); a
  // portable OBS uses a per-folder name and isn't detected.
  ObsMutex = 'OBSStudioCore';

var
  // OBS Studio's install folder, '' when not found.
  ObsDir: string;

function FFmpegDll(Index: Integer): string;
begin
  case Index of
    0: Result := 'avcodec-63.dll';
    1: Result := 'avutil-61.dll';
  else
    Result := 'swresample-7.dll';
  end;
end;

function SameDir(const A, B: string): Boolean;
begin
  Result := CompareText(RemoveBackslashUnlessRoot(A), RemoveBackslashUnlessRoot(B)) = 0;
end;

// The folder the official OBS installer records (HKLM\SOFTWARE\OBS Studio),
// else the standard Program Files one; only when obs64.exe is there.
function DetectObsDir(): string;
var
  Path: string;
begin
  Result := '';
  if RegQueryStringValue(HKLM, 'SOFTWARE\OBS Studio', '', Path) and (Path <> '') and
    FileExists(AddBackslash(Path) + 'bin\64bit\obs64.exe') then
    Result := RemoveBackslashUnlessRoot(Path)
  else if FileExists(ExpandConstant('{autopf}\obs-studio\bin\64bit\obs64.exe')) then
    Result := ExpandConstant('{autopf}\obs-studio');
end;

function GetObsBinDir(Param: string): string;
begin
  Result := AddBackslash(ObsDir) + 'bin\64bit';
end;

function GetObsExe(Param: string): string;
begin
  Result := GetObsBinDir('') + '\obs64.exe';
end;

function ObsExeExists(): Boolean;
begin
  Result := (ObsDir <> '') and FileExists(GetObsExe(''));
end;

// "31.1.0" or "32.2.2.0" -> a packed version; False when it isn't one.
function ParseVersion(const S: string; var Packed: Int64): Boolean;
var
  Parts: array[0..3] of Word;
  Rest, Part: string;
  I, P, N: Integer;
begin
  Result := False;
  for I := 0 to 3 do
    Parts[I] := 0;
  Rest := Trim(S);
  I := 0;
  while (Rest <> '') and (I <= 3) do
  begin
    P := Pos('.', Rest);
    if P = 0 then
    begin
      Part := Rest;
      Rest := '';
    end
    else
    begin
      Part := Copy(Rest, 1, P - 1);
      Rest := Copy(Rest, P + 1, Length(Rest));
    end;
    N := StrToIntDef(Part, -1);
    if (N < 0) or (N > 65535) then
      Exit;
    Parts[I] := N;
    I := I + 1;
  end;
  if I = 0 then
    Exit;
  Packed := PackVersionComponents(Parts[0], Parts[1], Parts[2], Parts[3]);
  Result := True;
end;

function InitializeSetup(): Boolean;
var
  Minimum, Found: Int64;
  FoundText: string;
begin
  Result := True;
  ObsDir := DetectObsDir();
  if ObsDir = '' then
  begin
    Log('OBS Studio not found (no HKLM\SOFTWARE\OBS Studio folder with bin\64bit\obs64.exe)');
    if SuppressibleMsgBox(FmtMessage(CustomMessage('ObsNotFound'), [ObsMinVersion]),
      mbConfirmation, MB_YESNO, IDYES) = IDNO then
      Result := False;
    Exit;
  end;

  // The minimum is checked again by the plugin itself when OBS loads it
  // (src/plugin-main.cpp), for an OBS updated or replaced later.
  if not ParseVersion(ObsMinVersion, Minimum) then
    RaiseException('Invalid minimum OBS version: ' + ObsMinVersion);
  if GetVersionNumbersString(GetObsExe(''), FoundText) and ParseVersion(FoundText, Found) then
  begin
    Log('OBS Studio ' + FoundText + ' in ' + ObsDir + '; minimum ' + ObsMinVersion);
    if ComparePackedVersion(Found, Minimum) < 0 then
    begin
      // One line: Inno reads a line starting with '[' as a section tag.
      SuppressibleMsgBox(FmtMessage(CustomMessage('ObsTooOld'), ['{#MyAppVersion}', ObsMinVersion,
        ObsDir, FoundText]), mbCriticalError, MB_OK, IDOK);
      Result := False;
      Exit;
    end;
  end
  else
    Log('Could not read the version of ' + GetObsExe('') + '; minimum not checked here');

  while Result and CheckForMutexes(ObsMutex) do
    if SuppressibleMsgBox(CustomMessage('ObsRunning'), mbError, MB_RETRYCANCEL, IDCANCEL) = IDCANCEL then
      Result := False;
end;

// The uninstall entry of an earlier version installed inside OBS's folder:
// its folder (OBS's), its uninstaller and our files there are all checked
// before anything runs.
function FindOldInstallInObsFolder(var OldDir, Uninstaller: string): Boolean;
var
  DisplayName, Name: string;
begin
  Result := False;
  if not RegQueryStringValue(HKLM, UninstallKey, 'Inno Setup: App Path', OldDir) or (OldDir = '') then
    Exit;
  OldDir := RemoveBackslashUnlessRoot(OldDir);
  if SameDir(OldDir, ExpandConstant('{app}')) then
    Exit; // Already the new location: a normal update.
  if not RegQueryStringValue(HKLM, UninstallKey, 'UninstallString', Uninstaller) then
    Exit;
  Uninstaller := RemoveQuotes(Uninstaller);
  Name := LowerCase(ExtractFileName(Uninstaller));
  if not (RegQueryStringValue(HKLM, UninstallKey, 'DisplayName', DisplayName) and
    (DisplayName = '{#MyAppName}')) then
    Log('Old uninstall entry has an unexpected display name: ' + DisplayName)
  else if not FileExists(AddBackslash(OldDir) + 'bin\64bit\obs64.exe') then
    Log('Old install folder is not an OBS Studio folder: ' + OldDir)
  else if not (FileExists(AddBackslash(OldDir) + 'obs-plugins\64bit\' + PluginName + '.dll') or
    DirExists(AddBackslash(OldDir) + 'data\obs-plugins\' + PluginName)) then
    Log('No files of the plugin in the old install folder: ' + OldDir)
  else if not SameDir(ExtractFileDir(Uninstaller), OldDir) or (Copy(Name, 1, 5) <> 'unins') or
    (ExtractFileExt(Name) <> '.exe') or not FileExists(Uninstaller) or
    not FileExists(ChangeFileExt(Uninstaller, '.dat')) then
    Log('Old uninstaller is not the one of that install: ' + Uninstaller)
  else
    Result := True;
end;

procedure UninstallOldInstallInObsFolder();
var
  OldDir, Uninstaller: string;
  Code, I: Integer;
begin
  if not FindOldInstallInObsFolder(OldDir, Uninstaller) then
    Exit;
  Log('Removing the earlier install in ' + OldDir + ' with ' + Uninstaller);
  if not Exec(Uninstaller, '/VERYSILENT /SUPPRESSMSGBOXES /NORESTART', '', SW_HIDE,
    ewWaitUntilTerminated, Code) then
  begin
    Log('Could not start the old uninstaller: ' + SysErrorMessage(Code));
    Exit;
  end;
  // The uninstaller hands over to a copy of itself in %TEMP% and deletes the
  // original last: wait for that (at most 30 s).
  for I := 1 to 120 do
  begin
    if not FileExists(Uninstaller) then
      Break;
    Sleep(250);
  end;
  Log('Old uninstaller finished with code ' + IntToStr(Code));
end;

function RunIcacls(const Params: string; var Code: Integer): Boolean;
begin
  Result := Exec(ExpandConstant('{sys}\icacls.exe'), Params, '', SW_HIDE, ewWaitUntilTerminated, Code) and
    (Code = 0);
  Log('icacls ' + Params + ' -> ' + IntToStr(Code));
end;

// Administrators (S-1-5-32-544) and SYSTEM (S-1-5-18) full control, Users
// (S-1-5-32-545) read and run, nothing inherited from ProgramData, owned by
// Administrators. SIDs, not names: the names are translated on a Spanish
// Windows.
function LockDownPluginFolder(var Code: Integer): Boolean;
var
  Dir: string;
begin
  Dir := ExpandConstant('{app}');
  Code := 0;
  Result := ForceDirectories(Dir) and
    RunIcacls('"' + Dir + '" /setowner *S-1-5-32-544 /T /C /Q', Code) and
    RunIcacls('"' + Dir + '" /reset /T /C /Q', Code) and
    RunIcacls('"' + Dir + '" /inheritance:r /grant:r *S-1-5-32-544:(OI)(CI)F *S-1-5-18:(OI)(CI)F ' +
      '*S-1-5-32-545:(OI)(CI)RX /C /Q', Code);
end;

function PrepareToInstall(var NeedsRestart: Boolean): String;
var
  Code: Integer;
begin
  Result := '';
  if CheckForMutexes(ObsMutex) then
  begin
    Result := CustomMessage('ObsStillRunning');
    Exit;
  end;
  UninstallOldInstallInObsFolder();
  if not LockDownPluginFolder(Code) then
    Result := FmtMessage(CustomMessage('AclFailed'), [ExpandConstant('{app}'), IntToStr(Code)]);
end;

function SameFileContents(const A, B: string): Boolean;
begin
  Result := False;
  try
    Result := GetSHA256OfFile(A) = GetSHA256OfFile(B);
  except
    Log('Could not hash ' + A + ': ' + GetExceptionMessage);
  end;
end;

// What a .zip extracted into OBS's folder, or an install whose uninstall entry
// is gone, leaves there: our DLL and data folder, and the FFmpeg DLLs only
// when our DLL was there too and they are the very files installed now.
procedure RemoveLeftoversInObsFolder(const Dir: string);
var
  PluginDir, Name: string;
  HadOurDll: Boolean;
  I: Integer;
begin
  if (Dir = '') or not FileExists(AddBackslash(Dir) + 'bin\64bit\obs64.exe') then
    Exit;
  PluginDir := AddBackslash(Dir) + 'obs-plugins\64bit\';
  HadOurDll := FileExists(PluginDir + PluginName + '.dll');
  if HadOurDll then
  begin
    Log('Deleting ' + PluginDir + PluginName + '.dll');
    DeleteFile(PluginDir + PluginName + '.dll');
    DeleteFile(PluginDir + PluginName + '.pdb');
  end;
  if DirExists(AddBackslash(Dir) + 'data\obs-plugins\' + PluginName) then
  begin
    Log('Deleting ' + AddBackslash(Dir) + 'data\obs-plugins\' + PluginName);
    DelTree(AddBackslash(Dir) + 'data\obs-plugins\' + PluginName, True, True, True);
  end;
  if not HadOurDll then
    Exit;
  for I := 0 to 2 do
  begin
    Name := FFmpegDll(I);
    if FileExists(PluginDir + Name) then
    begin
      if SameFileContents(PluginDir + Name, ExpandConstant('{app}\') + Name) then
      begin
        Log('Deleting ' + PluginDir + Name);
        DeleteFile(PluginDir + Name);
      end
      else
        Log('Keeping ' + PluginDir + Name + ': not the file this installer ships');
    end;
  end;
end;

procedure CurStepChanged(CurStep: TSetupStep);
begin
  if CurStep = ssPostInstall then
    RemoveLeftoversInObsFolder(ObsDir);
end;
