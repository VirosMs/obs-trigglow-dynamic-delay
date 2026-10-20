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
; Before installing (PrepareToInstall), in this order:
; - Setup stops if C:\ProgramData\obs-studio, its plugins folder or the
;   plugin's own folder is a junction or a symbolic link (reparse point):
;   files written or deleted there would land wherever it points.
; - OBS's plugins folder (C:\ProgramData\obs-studio\plugins): if a user
;   without admin rights owns it or may write in it (ProgramData's inherited
;   "Users: create files" entry, or one for Everyone or Authenticated Users),
;   its permissions are set (that folder only, no /T) so that only
;   Administrators and SYSTEM write and Users read and run, and the setup log
;   says so. OBS only reads that folder; its shader cache and updater files
;   are in C:\ProgramData\obs-studio itself, which is left as it is.
; - An install of 0.7.0 or earlier inside OBS's folder (this installer's own
;   uninstall entry, same AppId), or a .zip extracted there: our known files
;   are deleted directly, never by running the old uninstaller (in an OBS
;   folder users may write to, it could have been replaced). Our DLL and data
;   folder; the FFmpeg DLLs only next to our DLL and only when they are byte
;   for byte the ones this installer ships; the old unins*.exe/.dat only once
;   checked to be that entry's; then the old uninstall entry. Every folder on
;   the way is checked for junctions and symbolic links first.
; - The plugin folder is owned by Administrators, its permissions reset
;   (Administrators and SYSTEM write, Users only read and run, nothing
;   inherited) and then emptied completely: it is ours alone, and a DLL
;   another user dropped there before (ProgramData lets any user create
;   files in its subfolders) would be loaded into OBS (DLL planting).
;   Junctions and links inside it are removed, never followed.
; OBS's plugin_config folder (your settings and sign-in) is never touched.
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
spanish.ReparsePoint="%1" es un enlace (junction o enlace simbólico) y no una carpeta normal. La instalación se ha detenido: los archivos que escribiera o borrara ahí irían a otro sitio.%n%nSi no lo has creado tú, puede que otro usuario de este equipo intente redirigir la instalación. Borra el enlace (no lo que contiene) y vuelve a ejecutar este instalador.
english.ReparsePoint="%1" is a link (a junction or symbolic link), not a normal folder. Setup stopped: files written or deleted there would end up somewhere else.%n%nIf you didn't create it, another user of this computer may be trying to redirect the installation. Delete the link (not what it points to), then run this installer again.
spanish.PluginsAclFailed=No se han podido comprobar o corregir los permisos de la carpeta de plugins de OBS, "%1" (%2). La instalación se ha detenido para que ningún otro usuario pueda dejar plugins ahí.%n%nRevisa sus permisos (solo Administradores y SYSTEM deberían poder escribir en ella) y vuelve a ejecutar este instalador.
english.PluginsAclFailed=Could not check or correct the permissions of OBS's plugins folder, "%1" (%2). Setup stopped so that no other user can place plugins there.%n%nCheck its permissions (only Administrators and SYSTEM should be able to write to it), then run this installer again.
spanish.EmptyFailed=No se ha podido vaciar la carpeta del plugin, "%1". La instalación se ha detenido para que no quede junto al plugin ningún archivo que no sea suyo. Detalles en el registro de la instalación.
english.EmptyFailed=Could not empty the plugin's folder, "%1". Setup stopped so that no file that isn't the plugin's stays next to it. Details are in the setup log.

[Messages]
spanish.FinishedLabel=Instalación completada. Abre OBS Studio: el panel "Trigglow Dynamic Delay" aparece en Ver > Paneles (Docks).
english.FinishedLabel=Setup is complete. Open OBS Studio: the "Trigglow Dynamic Delay" dock is under View > Docks.

[Dirs]
; The folder's permissions are reset, and the folder emptied, by
; LockDownPluginFolder() below, before any file is copied: Inno's Permissions
; only add entries, and ProgramData's inherited "Users: create files" entry
; has to go. This line states the same rights for the record.
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
; Only used while installing (SecurePluginsRoot), from Setup's own temporary
; folder: never installed.
Source: "check-folder-acl.ps1"; Flags: dontcopy

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
  // Administrators (S-1-5-32-544) and SYSTEM (S-1-5-18) full control, Users
  // (S-1-5-32-545) read and run, for the folder, its subfolders and files.
  // SIDs, not names: the names are translated on a Spanish Windows.
  LockedDownGrants = '/grant:r *S-1-5-32-544:(OI)(CI)F *S-1-5-18:(OI)(CI)F *S-1-5-32-545:(OI)(CI)RX';

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

function SetFileAttributes(lpFileName: string; dwFileAttributes: Cardinal): Integer;
  external 'SetFileAttributesW@kernel32.dll stdcall';

function RunIcacls(const Params: string; var Code: Integer): Boolean;
begin
  Result := Exec(ExpandConstant('{sys}\icacls.exe'), Params, '', SW_HIDE, ewWaitUntilTerminated, Code) and
    (Code = 0);
  Log('icacls ' + Params + ' -> ' + IntToStr(Code));
end;

// True when Path is a junction, a symbolic link or any other reparse point.
// FindFirst on the path itself (no wildcard) reports that entry as it is,
// without following it. False when Path doesn't exist.
function IsReparsePoint(const Path: string): Boolean;
var
  FindRec: TFindRec;
begin
  Result := False;
  if FindFirst(RemoveBackslashUnlessRoot(Path), FindRec) then
  try
    Result := (FindRec.Attributes and FILE_ATTRIBUTE_REPARSE_POINT) <> 0;
  finally
    FindClose(FindRec);
  end;
end;

// The first of Base and each folder below it on the way to Base\Rel that is
// a reparse point; '' when none is (or when the rest doesn't exist yet).
function FirstReparsePoint(const Base, Rel: string): string;
var
  Path, Rest: string;
  P: Integer;
begin
  Result := '';
  Path := RemoveBackslashUnlessRoot(Base);
  Rest := Rel;
  while True do
  begin
    if IsReparsePoint(Path) then
    begin
      Result := Path;
      Exit;
    end;
    if Rest = '' then
      Exit;
    P := Pos('\', Rest);
    if P = 0 then
    begin
      Path := Path + '\' + Rest;
      Rest := '';
    end
    else
    begin
      Path := Path + '\' + Copy(Rest, 1, P - 1);
      Rest := Copy(Rest, P + 1, Length(Rest));
    end;
  end;
end;

// OBS's folder for all users (C:\ProgramData\obs-studio).
function ObsDataDir(): string;
begin
  Result := ExpandConstant('{commonappdata}\obs-studio');
end;

// C:\ProgramData\obs-studio, its plugins folder or the plugin's folder when
// one of them is a junction or link, else ''.
function ReparsePointInPluginPath(): string;
begin
  Result := FirstReparsePoint(ObsDataDir(), 'plugins\' + PluginName);
end;

// Another user's file or folder may shut Administrators out with its own
// permissions: make Administrators its owner and give them full control
// (/L: a link itself, never its target), so that it can be deleted.
procedure TakeOver(const Path: string);
var
  Code: Integer;
begin
  RunIcacls('"' + Path + '" /setowner *S-1-5-32-544 /L /C /Q', Code);
  RunIcacls('"' + Path + '" /grant *S-1-5-32-544:F /L /C /Q', Code);
end;

function DeleteEntry(const Path: string; IsDir: Boolean): Boolean; forward;

// Deletes everything inside Dir. Junctions and symbolic links are removed
// themselves, never followed, so nothing outside Dir is touched.
function DeleteTreeContents(const Dir: string): Boolean;
var
  FindRec: TFindRec;
  Path: string;
  IsDir, Done: Boolean;
begin
  Result := True;
  if FindFirst(AddBackslash(Dir) + '*', FindRec) then
  try
    repeat
      if (FindRec.Name <> '.') and (FindRec.Name <> '..') then
      begin
        Path := AddBackslash(Dir) + FindRec.Name;
        IsDir := (FindRec.Attributes and FILE_ATTRIBUTE_DIRECTORY) <> 0;
        if (FindRec.Attributes and FILE_ATTRIBUTE_REPARSE_POINT) <> 0 then
        begin
          Log('Removing the link ' + Path + ' itself, not what it points to');
          if IsDir then
            Done := RemoveDir(Path)
          else
            Done := DeleteFile(Path);
        end
        else
        begin
          if (FindRec.Attributes and FILE_ATTRIBUTE_READONLY) <> 0 then
            SetFileAttributes(Path, FILE_ATTRIBUTE_NORMAL);
          Done := DeleteEntry(Path, IsDir);
          if not Done then
          begin
            TakeOver(Path);
            SetFileAttributes(Path, FILE_ATTRIBUTE_NORMAL);
            Done := DeleteEntry(Path, IsDir);
          end;
        end;
        if Done then
          Log('Deleted ' + Path)
        else
        begin
          Log('Could not delete ' + Path);
          Result := False;
        end;
      end;
    until not FindNext(FindRec);
  finally
    FindClose(FindRec);
  end;
end;

function DeleteEntry(const Path: string; IsDir: Boolean): Boolean;
begin
  if IsDir then
    Result := DeleteTreeContents(Path) and RemoveDir(Path)
  else
    Result := DeleteFile(Path);
end;

function IsDirEmpty(const Dir: string): Boolean;
var
  FindRec: TFindRec;
begin
  Result := True;
  if FindFirst(AddBackslash(Dir) + '*', FindRec) then
  try
    repeat
      if (FindRec.Name <> '.') and (FindRec.Name <> '..') then
        Result := False;
    until not Result or not FindNext(FindRec);
  finally
    FindClose(FindRec);
  end;
end;

// check-folder-acl.ps1 on Dir: 'ok', 'fix: <why>' or 'error: <why>'.
function CheckFolderAcl(const Dir: string): string;
var
  Script, OutFile: string;
  Verdict: AnsiString;
  Code: Integer;
begin
  Script := ExpandConstant('{tmp}\check-folder-acl.ps1');
  OutFile := ExpandConstant('{tmp}\check-folder-acl.txt');
  if not FileExists(Script) then
    ExtractTemporaryFile('check-folder-acl.ps1');
  DeleteFile(OutFile);
  if not Exec(ExpandConstant('{sys}\WindowsPowerShell\v1.0\powershell.exe'),
    '-NoLogo -NoProfile -NonInteractive -ExecutionPolicy Bypass -File "' + Script + '" -Path "' + Dir +
    '" -Out "' + OutFile + '"', '', SW_HIDE, ewWaitUntilTerminated, Code) then
    Result := 'error: could not start PowerShell: ' + SysErrorMessage(Code)
  else if not LoadStringFromFile(OutFile, Verdict) then
    Result := 'error: PowerShell exited with code ' + IntToStr(Code) + ' without a result'
  else
    Result := Trim(String(Verdict));
  Log('Permissions of ' + Dir + ': ' + Result);
end;

// OBS's plugins folder (C:\ProgramData\obs-studio\plugins). ProgramData lets
// every user create files and folders in it, so whoever created it may not
// be an administrator, and Users, Everyone or Authenticated Users may be
// able to write to it: any of them could then put a DLL there that OBS loads
// for every user. When that's the case, Administrators become its owner and
// only they and SYSTEM may write to it, Users read and run (that folder's
// own entries only, no /T; folders inside it that inherit get the same).
// OBS only reads this folder: its shader cache and the updater's files are
// in C:\ProgramData\obs-studio, which is left as it is. If anything else
// still may write afterwards (an entry granted to one user on purpose),
// Setup stops instead of guessing.
function SecurePluginsRoot(var Error: string): Boolean;
var
  Root, Verdict: string;
  Code: Integer;
begin
  Result := False;
  Root := ObsDataDir() + '\plugins';
  if not ForceDirectories(Root) then
  begin
    Error := FmtMessage(CustomMessage('PluginsAclFailed'), [Root, 'could not create it']);
    Exit;
  end;
  if ReparsePointInPluginPath() <> '' then
  begin
    Error := FmtMessage(CustomMessage('ReparsePoint'), [ReparsePointInPluginPath()]);
    Exit;
  end;
  Verdict := CheckFolderAcl(Root);
  if Verdict = 'ok' then
  begin
    Result := True;
    Exit;
  end;
  if Copy(Verdict, 1, 5) <> 'fix: ' then
  begin
    Error := FmtMessage(CustomMessage('PluginsAclFailed'), [Root, Verdict]);
    Exit;
  end;
  Log('NOTE: users without admin rights could write to ' + Root + ' (' + Copy(Verdict, 6, Length(Verdict)) +
    '). Setting its permissions, this folder only: owner Administrators, Administrators and SYSTEM ' +
    'full control, Users read and run, nothing inherited from ProgramData.');
  if not (RunIcacls('"' + Root + '" /setowner *S-1-5-32-544 /L /C /Q', Code) and
    RunIcacls('"' + Root + '" /remove:g *S-1-1-0 *S-1-5-11 /L /C /Q', Code) and
    RunIcacls('"' + Root + '" /inheritance:r ' + LockedDownGrants + ' /L /C /Q', Code)) then
  begin
    Error := FmtMessage(CustomMessage('PluginsAclFailed'), [Root, 'icacls exit code ' + IntToStr(Code)]);
    Exit;
  end;
  Verdict := CheckFolderAcl(Root);
  if Verdict <> 'ok' then
  begin
    Error := FmtMessage(CustomMessage('PluginsAclFailed'), [Root, Verdict]);
    Exit;
  end;
  Log('NOTE: ' + Root + ' now only lets Administrators and SYSTEM write to it.');
  Result := True;
end;

// The uninstall entry of 0.7.0 or earlier (same AppId) when it points into
// an OBS Studio folder, not into the plugin folder used now.
function FindOldUninstallEntry(var OldDir, Uninstaller: string): Boolean;
var
  DisplayName: string;
begin
  Result := False;
  if not RegQueryStringValue(HKLM, UninstallKey, 'Inno Setup: App Path', OldDir) or (OldDir = '') then
    Exit;
  OldDir := RemoveBackslashUnlessRoot(OldDir);
  if SameDir(OldDir, ExpandConstant('{app}')) then
    Exit; // Already the new location: a normal update.
  if not (RegQueryStringValue(HKLM, UninstallKey, 'DisplayName', DisplayName) and
    (DisplayName = '{#MyAppName}')) then
    Log('Old uninstall entry has an unexpected display name: ' + DisplayName)
  else if not FileExists(AddBackslash(OldDir) + 'bin\64bit\obs64.exe') then
    Log('Old install folder is not an OBS Studio folder: ' + OldDir)
  else
  begin
    if not RegQueryStringValue(HKLM, UninstallKey, 'UninstallString', Uninstaller) then
      Uninstaller := '';
    Uninstaller := RemoveQuotes(Uninstaller);
    Result := True;
  end;
end;

// unins000.exe .. unins999.exe directly in OldDir, with its .dat next to it:
// the uninstaller Inno Setup made for that entry. It is deleted, never run.
function IsOldUninstaller(const OldDir, Uninstaller: string): Boolean;
var
  Name: string;
begin
  Name := LowerCase(ExtractFileName(Uninstaller));
  Result := (Uninstaller <> '') and SameDir(ExtractFileDir(Uninstaller), OldDir) and
    (Length(Name) = 12) and (Copy(Name, 1, 5) = 'unins') and (Copy(Name, 9, 4) = '.exe') and
    (StrToIntDef(Copy(Name, 6, 3), -1) >= 0) and FileExists(Uninstaller) and
    FileExists(ChangeFileExt(Uninstaller, '.dat')) and not IsReparsePoint(Uninstaller) and
    not IsReparsePoint(ChangeFileExt(Uninstaller, '.dat'));
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

// Our files in an OBS Studio folder, as 0.7.0 and earlier installed them or
// a .zip extracted there leaves them: obs-plugins\64bit\<name>.dll (and its
// .pdb), data\obs-plugins\<name>\, and the FFmpeg DLLs next to our DLL only
// when they are byte for byte the ones this installer ships (the same build
// 0.7.0 shipped). Deleted directly, never by the old uninstaller; every
// folder on the way below OBS's folder is checked for junctions and links
// first (OBS's folder itself may be one: it is where the registry says OBS
// is, and only an administrator can change that).
function RemoveOursInObsFolder(const Dir: string; var Error: string): Boolean;
var
  PluginDir, DataDir, Name, Reparse: string;
  HadOurDll: Boolean;
  I: Integer;
begin
  Result := True;
  if (Dir = '') or not FileExists(AddBackslash(Dir) + 'bin\64bit\obs64.exe') then
    Exit;
  Reparse := FirstReparsePoint(AddBackslash(Dir) + 'obs-plugins', '64bit');
  if Reparse = '' then
    Reparse := FirstReparsePoint(AddBackslash(Dir) + 'data', 'obs-plugins\' + PluginName);
  if Reparse <> '' then
  begin
    Error := FmtMessage(CustomMessage('ReparsePoint'), [Reparse]);
    Result := False;
    Exit;
  end;

  PluginDir := AddBackslash(Dir) + 'obs-plugins\64bit\';
  HadOurDll := FileExists(PluginDir + PluginName + '.dll');
  if HadOurDll then
  begin
    Log('Deleting ' + PluginDir + PluginName + '.dll');
    if not DeleteFile(PluginDir + PluginName + '.dll') then
      Log('Could not delete ' + PluginDir + PluginName + '.dll');
    if FileExists(PluginDir + PluginName + '.pdb') then
      DeleteFile(PluginDir + PluginName + '.pdb');
    for I := 0 to 2 do
    begin
      Name := FFmpegDll(I);
      if FileExists(PluginDir + Name) then
      begin
        if SameFileContents(PluginDir + Name, ExpandConstant('{tmp}\') + Name) then
        begin
          Log('Deleting ' + PluginDir + Name);
          DeleteFile(PluginDir + Name);
        end
        else
          Log('Keeping ' + PluginDir + Name + ': not the file this installer ships');
      end;
    end;
  end;

  DataDir := AddBackslash(Dir) + 'data\obs-plugins\' + PluginName;
  if DirExists(DataDir) then
  begin
    Log('Deleting ' + DataDir);
    if not (DeleteTreeContents(DataDir) and RemoveDir(DataDir)) then
      Log('Could not delete all of ' + DataDir);
  end;
end;

// 0.7.0 and earlier, in OBS's own folder: our files, the old uninstaller
// (only once checked to be that entry's) and the old uninstall entry. The
// old uninstaller is never run: in an OBS folder users may write to, it
// could have been replaced.
function RemoveOldInstall(var Error: string): Boolean;
var
  OldDir, Uninstaller: string;
  I: Integer;
begin
  for I := 0 to 2 do
    ExtractTemporaryFile(FFmpegDll(I));

  Result := RemoveOursInObsFolder(ObsDir, Error);
  if not Result then
    Exit;
  if not FindOldUninstallEntry(OldDir, Uninstaller) then
    Exit;
  Log('Earlier install found in ' + OldDir);
  if not SameDir(OldDir, ObsDir) then
  begin
    Result := RemoveOursInObsFolder(OldDir, Error);
    if not Result then
      Exit;
  end;
  if IsOldUninstaller(OldDir, Uninstaller) then
  begin
    Log('Deleting the old uninstaller ' + Uninstaller + ' and its .dat (not running it)');
    DeleteFile(Uninstaller);
    DeleteFile(ChangeFileExt(Uninstaller, '.dat'));
  end
  else
    Log('Keeping "' + Uninstaller + '": not the uninstaller of that install');
  if RegDeleteKeyIncludingSubkeys(HKLM, UninstallKey) then
    Log('Deleted the old uninstall entry HKLM\' + UninstallKey)
  else
    Log('Could not delete the old uninstall entry HKLM\' + UninstallKey);
end;

// The plugin's folder: owned by Administrators, its permissions reset to
// Administrators and SYSTEM full control and Users read and run, nothing
// inherited from ProgramData (/reset first drops any entry someone added),
// and then emptied: it is ours alone, and whatever is in it now (an earlier
// version's files, or a DLL another user dropped there) goes. Only that
// folder's own permissions are set (no /T): everything in it is deleted,
// links themselves rather than what they point to, and the files installed
// next inherit the new permissions.
function LockDownPluginFolder(var Error: string): Boolean;
var
  Dir, Reparse: string;
  Code: Integer;
begin
  Result := False;
  Dir := ExpandConstant('{app}');
  Code := 0;
  if not ForceDirectories(Dir) then
  begin
    Error := FmtMessage(CustomMessage('AclFailed'), [Dir, '-1']);
    Exit;
  end;
  Reparse := ReparsePointInPluginPath();
  if Reparse <> '' then
  begin
    Error := FmtMessage(CustomMessage('ReparsePoint'), [Reparse]);
    Exit;
  end;
  if not (RunIcacls('"' + Dir + '" /setowner *S-1-5-32-544 /L /C /Q', Code) and
    RunIcacls('"' + Dir + '" /reset /L /C /Q', Code) and
    RunIcacls('"' + Dir + '" /inheritance:r ' + LockedDownGrants + ' /L /C /Q', Code)) then
  begin
    Error := FmtMessage(CustomMessage('AclFailed'), [Dir, IntToStr(Code)]);
    Exit;
  end;
  Log('Emptying ' + Dir);
  if not DeleteTreeContents(Dir) or not IsDirEmpty(Dir) then
  begin
    Error := FmtMessage(CustomMessage('EmptyFailed'), [Dir]);
    Exit;
  end;
  Result := True;
end;

function PrepareToInstall(var NeedsRestart: Boolean): String;
var
  Error, Reparse: string;
begin
  Result := '';
  Error := '';
  if CheckForMutexes(ObsMutex) then
  begin
    Result := CustomMessage('ObsStillRunning');
    Exit;
  end;
  Reparse := ReparsePointInPluginPath();
  if Reparse <> '' then
  begin
    Result := FmtMessage(CustomMessage('ReparsePoint'), [Reparse]);
    Exit;
  end;
  // One after the other: each step only runs when the one before succeeded.
  if not SecurePluginsRoot(Error) then
    Result := Error
  else if not RemoveOldInstall(Error) then
    Result := Error
  else if not LockDownPluginFolder(Error) then
    Result := Error;
end;
