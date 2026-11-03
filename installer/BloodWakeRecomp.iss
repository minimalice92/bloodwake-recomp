; Blood Wake Recomp: Setup and Uninstall (Inno Setup 6.7.3).
;
; Build it with installer/build_installer.py, which stages the public source
; tree and passes SrcTree, OutDir, AppVer and the retail XBE fingerprint.
;
; Setup's pages: welcome, disclaimer (tick "I own Blood Wake..."), your disc
; (a folder, an XISO or a full disc image; its main program is checked against
; the retail fingerprint at once), install folder (per-user, no admin), what's
; needed (detects Visual Studio 2022's C++ tools; offers Microsoft's Build Tools
; if missing), shortcuts, ready, then the install: our files, a private Python
; and the pinned Python packages (downloaded, SHA-256 checked), then
; tools/build_from_disc.py with its progress on the bar, then the shortcuts.
;
; Unattended:
;   BloodWakeRecomp-Setup.exe /VERYSILENT /SUPPRESSMSGBOXES /IOWNTHEGAME
;       /DISC="<folder or image>" [/DIR="<install folder>"] [/TASKS=desktopicon]
;       [/INSTALLVS] [/VSPLANONLY] [/JOBS=N] [/TOOLKIT=<dir>] [/LOG="<file>"]
;   /IOWNTHEGAME  you own Blood Wake and made this copy from your own disc (required when silent)
;   /DISC         your copy (required when silent: without it Setup stops at the disc page and logs why)
;   /ACCEPTUNVERIFIED  build a copy Setup doesn't recognise (another region, modified or damaged),
;                 after reading the warning Setup shows in its window (required when silent for such
;                 a copy, with /IOWNTHEGAME; it passes --allow-unverified to build_from_disc.py)
;   /INSTALLVS    allowed to install Microsoft's Build Tools if no Visual Studio 2022 C++ is found
;   /VSPLANONLY   download and hash-check the Build Tools bootstrapper and write the
;                 command Setup would run to deps\vs_plan.txt, but never run it (testing)
;   /SIMULATENOVS act as if no Visual Studio were found (testing the Build Tools path)
;   /SIMULATENOCMAKE act as if no CMake were found (testing pip's CMake)
;   /SIMULATESAC  act as if Smart App Control were on (testing its page; never turn it on to test)
;   /SIMULATEUACDECLINE (with /SIMULATENOVS) act as if Windows' permission prompt for the Build
;                 Tools was declined (testing the message: the bootstrapper is downloaded and
;                 hash-checked as usual, but never run)
;   /STUBBUILD    a fake 5-second build with placeholder files (testing the pages and uninstall)
;   Exit codes: 0 ok; 100 + build_from_disc.py's code when the build stopped (102 not an
;   unmodified retail copy, 103 a dependency, 104 pipeline, 105 generated code differs,
;   106 compiler, 107 disk space or I/O, 108 you stopped the build with Cancel);
;   110 the Build Tools install failed or was declined;
;   Inno's own codes otherwise (1 = a silent install stopped on a page: the log says why;
;   2 = cancelled).
;
; One install per Windows user (item 98): there's one Settings > Apps entry and one set of
; shortcuts, so when Blood Wake Recomp is installed, Setup offers that folder and won't
; install into another (it offers to update the install there, or cancels; silent: stops).
;
; Uninstall (unins000.exe, or Settings > Apps): always removes the build, the
; installed files and the shortcuts. Options (a window, or these when silent):
;   saves and settings   kept by default            /REMOVESAVES removes them
;   the private Python   removed by default         /KEEPPYTHON keeps it
;   the game's files     removed by default         /KEEPGAME keeps them
;   the shader cache     removed by default         /KEEPSHADERCACHE keeps it
;   Build Tools          kept by default; offered only if this Setup installed them
;                                                   /REMOVEVS removes them
;   /USERDATAROOT=<dir>  (testing) act on <dir>\xboxrecomp and <dir>\BloodWake instead of
;                        %LOCALAPPDATA%'s, so a test uninstall never touches real saves
;   /VSPLANONLY          (testing) log the Build Tools uninstall command instead of running it

#ifndef SrcTree
  #error Build with installer/build_installer.py (SrcTree is not defined)
#endif
#ifndef OutDir
  #define OutDir "out"
#endif
#ifndef AppVer
  #define AppVer "0.1.0"
#endif
#ifndef XbeSize
  #error XbeSize is not defined (build_installer.py reads it from fingerprints/retail.json)
#endif
#ifndef XbeSha256
  #error XbeSha256 is not defined
#endif
#ifndef RetailLabel
  #define RetailLabel "Xbox retail"
#endif

; ── Pinned downloads (official sources; SHA-256 checked by Setup) ─────────
#define PyVer        "3.13.15"
#define PyZipUrl     "https://www.python.org/ftp/python/3.13.15/python-3.13.15-amd64.zip"
#define PyZipSha256  "6479223746cdfb79d25865110d6f524ac98de081324e119af1dc3ae36bddc7a5"
#define PyZipBytes   "34324622"
#define PyUnpacked   "125031554"
#define CapWhl       "capstone-5.0.9-py3-none-win_amd64.whl"
#define CapUrl       "https://files.pythonhosted.org/packages/50/e6/6f06fdb6a9ed32b2f7cd9c036b92d5324112c3ef7080f2c71efc367d40dd/capstone-5.0.9-py3-none-win_amd64.whl"
#define CapSha256    "732cedbbb56d42e723f14d7af6387f1454194a820b4b96b56d1e53f865ef85d0"
#define CapBytes     "1273459"
#define PefWhl       "pefile-2024.8.26-py3-none-any.whl"
#define PefUrl       "https://files.pythonhosted.org/packages/54/16/12b82f791c7f50ddec566873d5bdd245baa1491bac11d15ffb98aecc8f8b/pefile-2024.8.26-py3-none-any.whl"
#define PefSha256    "76f8b485dcd3b1bb8166f1128d395fa3d87af26360c2358fb75b80019b957c6f"
#define PefBytes     "74766"
#define CmakeWhl     "cmake-4.4.4-py3-none-win_amd64.whl"
#define CmakeUrl     "https://files.pythonhosted.org/packages/87/9c/a7918eeeed09b34aa5dc1b767367fb1a2dcbb56171be00be0fd1280fe752/cmake-4.4.4-py3-none-win_amd64.whl"
#define CmakeSha256  "ad8e0a38b5707e27882701146bdfffecacd80e7703fa0fc36c77528518d545af"
#define CmakeBytes   "42321211"
; Microsoft's fixed-version bootstrapper for Build Tools 17.14.43 (2026-10-25), from the
; Visual Studio 2022 release history page; the URL's second part is the file's SHA-256.
#define VsVer        "17.14.43"
#define VsUrl        "https://download.visualstudio.microsoft.com/download/pr/4bc08c27-bd31-470c-9a30-3b62707fbf0f/dbea89df9de387e415075d40d713ecc1a73091d03e81080c3a957101f390f499/vs_BuildTools.exe"
#define VsSha256     "dbea89df9de387e415075d40d713ecc1a73091d03e81080c3a957101f390f499"
#define VsBytes      "4474088"

[Setup]
AppId={{0FB930E5-4F64-4720-9243-3E2404D38AB5}
AppName=Blood Wake Recomp
AppVersion={#AppVer}
AppVerName=Blood Wake Recomp {#AppVer}
AppPublisher=Blood Wake Recomp (unofficial fan project)
AppPublisherURL=https://github.com/minimalice92/bloodwake-recomp
AppSupportURL=https://github.com/minimalice92/bloodwake-recomp/issues
AppComments=Unofficial, non-commercial. Not affiliated with Microsoft or Stormfront Studios. Built on this PC from your own copy of Blood Wake.
DefaultDirName={autopf}\Blood Wake Recomp
DefaultGroupName=Blood Wake Recomp
DisableProgramGroupPage=yes
DisableWelcomePage=no
DisableDirPage=no
UsePreviousAppDir=yes
DirExistsWarning=no
PrivilegesRequired=lowest
ArchitecturesAllowed=x64compatible
ArchitecturesInstallIn64BitMode=x64compatible
MinVersion=10.0
ArchiveExtraction=full
Compression=lzma2/max
SolidCompression=yes
#ifndef WizStyle
  #define WizStyle "modern dynamic windows11"
#endif
WizardStyle={#WizStyle}
; Our pictures (item 15, docs/025/STYLE.md), drawn by tools/bw_icon.py in several sizes
; for Windows' display scaling: the banner's scene beside the Welcome and Finished pages,
; the icon at the top right of the others. They suit Windows' light and dark modes alike.
; (The ...DynamicDark ones too: left blank, Inno shows its own dark pictures instead.)
#ifdef WizSide
WizardImageFile={#WizSide}
WizardImageFileDynamicDark={#WizSide}
#endif
#ifdef WizSmall
WizardSmallImageFile={#WizSmall}
WizardSmallImageFileDynamicDark={#WizSmall}
#endif
OutputDir={#OutDir}
OutputBaseFilename=BloodWakeRecomp-Setup-{#AppVer}
; The unpacked game (2.0 GB), the work folder (toolkit, generated code, build: about 0.25 GB), margin.
ExtraDiskSpaceRequired=2700000000
UninstallDisplayName=Blood Wake Recomp
; Setup, the uninstaller and its Settings > Apps entry: our icon (docs/025/STYLE.md),
; drawn by tools/bw_icon.py from build_installer.py
#ifdef IconFile
SetupIconFile={#IconFile}
#endif
UninstallDisplayIcon={uninstallexe}
SetupLogging=yes
SetupMutex=BloodWakeRecompSetup
; The running game holds this mutex (docs/025/CONTRACTS.md 1, src/bw_host.c): Setup and the
; uninstaller ask for Blood Wake to be closed first. The launcher holds none, so the code
; also looks for any program running from the install folder (item 12).
AppMutex=BloodWakeRecomp.Running
CloseApplications=no
ShowLanguageDialog=no
VersionInfoDescription=Blood Wake Recomp Setup (unofficial)
VersionInfoProductName=Blood Wake Recomp

[Languages]
Name: "en"; MessagesFile: "compiler:Default.isl"

[Messages]
WelcomeLabel1=Welcome to Blood Wake Recomp
WelcomeLabel2=Blood Wake Recomp is an unofficial fan port that runs Blood Wake (Xbox, 2001) natively on Windows. Setup builds the game on this PC from your own copy.%n%nYou need:%n%n-  your copy of Blood Wake: a disc image or a folder of the disc's files%n-  about 3 GB of free space%n-  an internet connection%n-  about 15 minutes (longer on an older PC)%n%nNothing from the game is downloaded or sent anywhere.
ConfirmUninstall=Remove %1 now?
ClickNext=
; Inno's stock pages on the common path, without the title, subtitle and body repeating each other (item 106)
WizardSelectDir=Install folder
SelectDirDesc=Where should [name] go?
SelectDirLabel3=Setup installs [name] in this folder, for your Windows user only.
SelectDirBrowseLabel=Press Next, or Browse to choose another folder.
WizardSelectTasks=Shortcuts
SelectTasksDesc=Where do you want to start Blood Wake from?
SelectTasksLabel2=
WizardReady=Ready to install
ReadyLabel1=Setup is ready to install [name] and build the game.
ReadyLabel2a=Press Install to start, or Back to change something.
ReadyLabel2b=Press Install to start.
InstallingLabel=Setup is installing [name] and building the game on this PC.
ReadyMemoDir=Install folder:
ReadyMemoTasks=Shortcuts:
; one message for Setup already open and for Blood Wake running (Inno fills in %1)
SetupAppRunningError=%1 is already running.%n%nClose it, then press OK, or press Cancel to stop.
UninstallAppRunningError=%1 is already running.%n%nClose it, then press OK, or press Cancel to stop.
FinishedHeadingLabel=Blood Wake Recomp

[Tasks]
; Both ticked by default; either can be unticked (item 13). Silent: /TASKS="startmenuicon,desktopicon".
Name: "startmenuicon"; Description: "Create a &Start menu entry"
Name: "desktopicon"; Description: "Create a &desktop shortcut"

[Files]
Source: "disclaimer.txt"; Flags: dontcopy
Source: "setup_helper.py"; DestDir: "{app}\setup"; Flags: ignoreversion
Source: "{#SrcTree}\*"; DestDir: "{app}\source"; Flags: recursesubdirs createallsubdirs ignoreversion
; The private Python: python.org's full Windows package (a zip: nothing is registered,
; PATH is untouched). Kept or removed by the uninstaller's choice, not Inno's log.
Source: "{#PyZipUrl}"; DestDir: "{app}\python"; DestName: "python-{#PyVer}-amd64.zip"; \
  ExternalSize: {#PyUnpacked}; Hash: "{#PyZipSha256}"; \
  Flags: external download extractarchive recursesubdirs createallsubdirs ignoreversion uninsneveruninstall; \
  Check: NeedPython; AfterInstall: PythonUnpacked
Source: "{#CapUrl}"; DestDir: "{app}\deps"; DestName: "{#CapWhl}"; ExternalSize: {#CapBytes}; \
  Hash: "{#CapSha256}"; Flags: external download ignoreversion
Source: "{#PefUrl}"; DestDir: "{app}\deps"; DestName: "{#PefWhl}"; ExternalSize: {#PefBytes}; \
  Hash: "{#PefSha256}"; Flags: external download ignoreversion
Source: "{#CmakeUrl}"; DestDir: "{app}\deps"; DestName: "{#CmakeWhl}"; ExternalSize: {#CmakeBytes}; \
  Hash: "{#CmakeSha256}"; Flags: external download ignoreversion; Check: NeedPipCMake
Source: "{#VsUrl}"; DestDir: "{app}\deps"; DestName: "vs_BuildTools.exe"; ExternalSize: {#VsBytes}; \
  Hash: "{#VsSha256}"; Flags: external download ignoreversion; Check: WantVS

[UninstallDelete]
Type: files; Name: "{autoprograms}\Blood Wake Recomp.lnk"
Type: files; Name: "{autodesktop}\Blood Wake Recomp.lnk"
; What the game and the launcher write into the install folder while you play (item 11):
; left behind, they made the folder look like someone else's to the next Setup.
Type: files; Name: "{app}\play.log"
Type: files; Name: "{app}\xbox_kernel.log"
Type: files; Name: "{app}\icall_*.dump"
Type: files; Name: "{app}\bw_launch_next.bin"
Type: files; Name: "{app}\bw_relaunch.bin"
Type: files; Name: "{app}\*.dmp"
Type: files; Name: "{app}\kept_by_uninstall.txt"

[Run]
Filename: "{app}\python\pythonw.exe"; Parameters: "-E -s ""{app}\tools\launcher.pyw"""; \
  WorkingDir: "{app}"; Description: "Open the Blood Wake launcher now"; \
  Flags: postinstall nowait skipifsilent; Check: BuildSucceeded

[Code]
const
  XBE_SIZE = {#XbeSize};
  XBE_SHA256 = '{#XbeSha256}';
  RETAIL_LABEL = '{#RetailLabel}';
  TITLE_DIR = '4d530010';          { the game's own folder under UDATA and TDATA }
  LINK_NAME = 'Blood Wake Recomp.lnk';

var
  DisclaimerPage, DiscPage, WarnPage, DepsPage, SacPage: TWizardPage;
  gSacOn: Boolean;
  OwnCheck: TNewCheckBox;
  ImageRadio, FolderRadio: TNewRadioButton;
  DiscEdit: TNewEdit;
  DiscStatus, DiscMark, DepsText, VSNote: TNewStaticText;
  gDiscKind: Integer;
  gQuitNoConfirm: Boolean;
  gUnverifiedAccepted: Boolean;
  VSCheck: TNewCheckBox;
  gHaveVS, gHaveCMake, gVSPlanOnly, gStub: Boolean;
  gVSPath, gVSName, gVSVersion: String;
  gDisc, gDiscChecked, gDiscVerdict, gDiscEvaluated: String;
  gBuildOK, gBuildRan: Boolean;
  gBuildExit: Integer;
  gBuildMsg, gLastMsg, gFingerprint, gBuildReason, gBuildDetail, gCancelFile: String;
  gInBuild, gBuildCancelled: Boolean;
  gBase, gSpan: Integer;
  gVSInstalledNow: Boolean;
  { uninstall choices }
  uKeepSaves, uRemovePython, uRemoveGame, uRemoveShaders, uRemoveVS, uVSOurs: Boolean;

{ ── Command line ─────────────────────────────────────────────────────── }

function CmdParam(const Name: String; var Value: String): Boolean;
var
  I: Integer;
  P, Key: String;
begin
  Result := False;
  Value := '';
  Key := '/' + Uppercase(Name);
  for I := 1 to ParamCount do begin
    P := ParamStr(I);
    if Uppercase(P) = Key then begin
      Result := True;
      exit;
    end;
    if Pos(Key + '=', Uppercase(P)) = 1 then begin
      Value := Copy(P, Length(Key) + 2, MaxInt);
      Result := True;
      exit;
    end;
  end;
end;

function HasParam(const Name: String): Boolean;
var
  V: String;
begin
  Result := CmdParam(Name, V) and (V <> '0') and (Lowercase(V) <> 'no');
end;

{ A path as one command-line argument. A trailing backslash would escape the
  closing quote, so "E:\" becomes "E:\." (the same folder). }
function ArgQuote(S: String): String;
begin
  if (Length(S) > 0) and (S[Length(S)] = '\') then
    S := S + '.';
  Result := '"' + S + '"';
end;

{ ── Blood Wake running from the install folder (item 12) ────────────── }

{ The programs running from Dir (the launcher's pythonw.exe, the game, its
  helpers), as "name (PID n)" lines, or ''. WMI, so no tool is needed. }
function RunningFrom(const Dir: String): String;
var
  Locator, Service, Procs, P: Variant;
  I, N, Pid: Integer;
  D, Path, Name: String;
begin
  Result := '';
  if Dir = '' then
    exit;
  D := Lowercase(AddBackslash(ExpandFileName(Dir)));
  { (the mutex is checked at the end, even when the folder is new) }
  try
    Locator := CreateOleObject('WbemScripting.SWbemLocator');
    Service := Locator.ConnectServer('.', 'root\CIMV2');
    Procs := Service.ExecQuery('SELECT ProcessId, Name, ExecutablePath FROM Win32_Process');
    N := Procs.Count;
    for I := 0 to N - 1 do begin
      P := Procs.ItemIndex(I);
      if not VarIsNull(P.ExecutablePath) then begin
        Path := P.ExecutablePath;
        Name := P.Name;
        { The uninstaller's own first phase waits in the folder (unins000.exe): not Blood Wake. }
        if (Pos(D, Lowercase(Path)) = 1) and not WildcardMatch(Lowercase(Name), 'unins*.exe') then begin
          Pid := P.ProcessId;
          Result := Result + '  ' + Name + ' (PID ' + IntToStr(Pid) + ')' + #13#10;
        end;
      end;
    end;
  except
    Log('Checking for running programs failed: ' + GetExceptionMessage);
  end;
  { The game's own mutex (contract 1): a game started from elsewhere counts too.
    The uninstaller's own AppMutex check comes only after its options window. }
  if CheckForMutexes('BloodWakeRecomp.Running') and (Pos('bloodwake.exe', Lowercase(Result)) = 0) then
    Result := Result + '  Blood Wake (the game is running)' + #13#10;
end;

{ Waits for the player to close what runs from Dir. True when nothing does
  (or they chose to go on anyway); False to stop. Silent: logs and stops. }
function ClosedFirst(const Dir, What: String): Boolean;
var
  R: String;
  Silent: Boolean;
begin
  Result := True;
  if IsUninstaller() then Silent := UninstallSilent() else Silent := WizardSilent();
  R := RunningFrom(Dir);
  while R <> '' do begin
    Log('Running from ' + Dir + ':' + #13#10 + R);
    if Silent then begin
      Log('Stopped: close Blood Wake first (the launcher and the game), then ' + What + ' again.');
      Result := False;
      exit;
    end;
    if MsgBox('Close Blood Wake first: the launcher or the game is running from ' + Dir + '.' + #13#10#13#10 + R + #13#10 +
        'Then press Try again.', mbError, MB_RETRYCANCEL) <> IDRETRY then begin
      Log('Stopped: Blood Wake was still running.');
      Result := False;
      exit;
    end;
    R := RunningFrom(Dir);
  end;
end;

{ ── The disc: its main program against the retail fingerprint ───────── }

function U16(const S: AnsiString; Off: Integer): Integer;
begin
  Result := Ord(S[Off]) + Ord(S[Off + 1]) * 256;
end;

function U32(const S: AnsiString; Off: Integer): Int64;
var
  Hi: Int64;
begin
  Hi := Ord(S[Off + 3]);
  Result := Ord(S[Off]) + Ord(S[Off + 1]) * 256 + Ord(S[Off + 2]) * 65536 + Hi * 16777216;
end;

function ReadAt(F: TFileStream; Offset: Int64; Count: Integer; var Buf: AnsiString): Boolean;
begin
  Result := False;
  try
    if F.Seek(Offset, 0) <> Offset then
      exit;
    SetLength(Buf, Count);
    F.ReadBuffer(Buf, Count);
    Result := True;
  except
    Result := False;
  end;
end;

{ Finds default.xbe in an Xbox image's root folder (XDVDFS, as the toolkit's
  tools/xiso reads it) and hashes it. Layout: 'XISO' or 'full disc image'. }
function ImageXbe(const Path: String; var Sha, Layout, Magic: String; var Size: Int64): Boolean;
var
  F: TFileStream;
  Bases: array[0..4] of Int64;
  I, Off, NLen, L, R: Integer;
  Base, RootSec, RootSize, Sec, Chunk: Int64;
  Buf, Dir, Name: AnsiString;
begin
  Result := False;
  Bases[0] := 0; Bases[1] := $FD90; Bases[2] := $30600; Bases[3] := $FD90000; Bases[4] := $18300000;
  try
    F := TFileStream.Create(Path, fmOpenRead or fmShareDenyNone);
  except
    exit;
  end;
  try
    Base := -1;
    for I := 0 to 4 do
      if ReadAt(F, Bases[I] + 32 * 2048, 2048, Buf) and (Copy(Buf, 1, 20) = 'MICROSOFT*XBOX*MEDIA') then begin
        Base := Bases[I];
        break;
      end;
    if Base < 0 then
      exit;
    if Base = 0 then Layout := 'an XISO' else Layout := 'a full disc image';
    RootSec := U32(Buf, 21);
    RootSize := U32(Buf, 25);
    if (RootSize <= 0) or (RootSize > 4194304) then
      exit;
    if not ReadAt(F, Base + RootSec * 2048, RootSize, Dir) then
      exit;
    Chunk := 0;
    while Chunk < RootSize do begin
      Off := 0;
      while (Off + 14 <= 2048) and (Chunk + Off + 14 <= RootSize) do begin
        L := U16(Dir, Chunk + Off + 1);
        R := U16(Dir, Chunk + Off + 3);
        NLen := Ord(Dir[Chunk + Off + 14]);
        if ((L = $FFFF) and (R = $FFFF)) or (NLen = 0) then
          break;
        Name := Copy(Dir, Chunk + Off + 15, NLen);
        if Lowercase(Name) = 'default.xbe' then begin
          Sec := U32(Dir, Chunk + Off + 5);
          Size := U32(Dir, Chunk + Off + 9);
          if (Size <= 0) or (Size > 67108864) then
            exit;
          if not ReadAt(F, Base + Sec * 2048, Size, Buf) then
            exit;
          Sha := GetSHA256OfString(Buf);
          Magic := String(Copy(Buf, 1, 4));
          Result := True;
          exit;
        end;
        Off := Off + ((14 + NLen + 3) div 4) * 4;
      end;
      Chunk := Chunk + 2048;
    end;
  finally
    F.Free;
  end;
end;

const
  DISC_UNUSABLE = 0;      { not an Xbox image, or no default.xbe: nothing to build }
  DISC_VERIFIED = 1;      { its main program is the retail one Setup knows }
  DISC_UNVERIFIED = 2;    { an Xbox program Setup doesn't recognise: the warning page, item 2 }

function FileMagic(const Path: String): String;
var
  F: TFileStream;
  Buf: AnsiString;
begin
  Result := '';
  try
    F := TFileStream.Create(Path, fmOpenRead or fmShareDenyNone);
    try
      if ReadAt(F, 0, 4, Buf) then
        Result := String(Buf);
    finally
      F.Free;
    end;
  except
    Result := '';
  end;
end;

{ What the copy is: DISC_VERIFIED, DISC_UNVERIFIED or DISC_UNUSABLE, and the
  status line in plain words. Only the main program is checked here (a second
  or so); the build checks every other file while it copies them. }
function CheckDisc(const Path: String; var Msg: String): Integer;
var
  Sha, Layout, X, Magic: String;
  Size: Int64;
begin
  Result := DISC_UNUSABLE;
  Sha := '';
  Magic := '';
  Size := -1;
  if Trim(Path) = '' then begin
    Msg := 'Choose your copy of Blood Wake first.';
    exit;
  end;
  if DirExists(Path) then begin
    X := AddBackslash(Path) + 'default.xbe';
    if not FileExists(X) then begin
      Msg := 'This folder has no default.xbe. Choose the folder that holds your disc''s files (default.xbe is at its top).';
      exit;
    end;
    Layout := 'a folder of the disc''s files';
    try
      Sha := GetSHA256OfFile(X);
    except
      Msg := 'Setup couldn''t read ' + X + ': ' + GetExceptionMessage;
      exit;
    end;
    Magic := FileMagic(X);
    if not FileSize64(X, Size) then
      Size := -1;
  end else if FileExists(Path) then begin
    if not ImageXbe(Path, Sha, Layout, Magic, Size) then begin
      Msg := 'This isn''t an Xbox disc image Setup can read. Choose a full disc image or an XISO of your Blood Wake disc, or a folder of its files.';
      exit;
    end;
  end else begin
    Msg := 'Setup can''t find ' + Path + '.';
    exit;
  end;
  if Magic <> 'XBEH' then begin
    Msg := 'This copy''s default.xbe isn''t an Xbox program. Choose your Blood Wake disc image or a folder of its files.';
    exit;
  end;
  if (Lowercase(Sha) = XBE_SHA256) and (Size = XBE_SIZE) then begin
    Msg := 'Recognised: Blood Wake, ' + RETAIL_LABEL + ', ' + Layout + '. Setup checks every file as it copies them.';
    Result := DISC_VERIFIED;
  end else begin
    Msg := 'Not recognised: this may be another region, a modified copy or a damaged one. You can still go on; the next page explains.';
    Result := DISC_UNVERIFIED;
  end;
end;

{ ── Dependencies ─────────────────────────────────────────────────────── }

function VsWhere(const Prop: String): String;
var
  Exe: String;
  Code: Integer;
  Output: TExecOutput;
begin
  Result := '';
  Exe := ExpandConstant('{commonpf32}\Microsoft Visual Studio\Installer\vswhere.exe');
  if not FileExists(Exe) then
    exit;
  try
    if ExecAndCaptureOutput(Exe, '-products * -version [17.0,18.0) -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -latest -utf8 -format value -property ' + Prop,
        '', SW_HIDE, ewWaitUntilTerminated, Code, Output) and (Code = 0) and (GetArrayLength(Output.StdOut) > 0) then
      Result := Trim(Output.StdOut[0]);
  except
    Log('vswhere: ' + GetExceptionMessage);
  end;
end;

procedure DetectDeps();
begin
  gVSPath := '';
  gVSName := '';
  gVSVersion := '';
  if not HasParam('SIMULATENOVS') then begin
    gVSPath := VsWhere('installationPath');
    if gVSPath <> '' then begin
      gVSName := VsWhere('displayName');
      gVSVersion := VsWhere('installationVersion');
    end;
  end;
  gHaveVS := (gVSPath <> '') and DirExists(gVSPath);
  gHaveCMake := (gHaveVS and FileExists(gVSPath + '\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe'))
    or (FileSearch('cmake.exe', GetEnv('PATH')) <> '');
  if HasParam('SIMULATENOCMAKE') then
    gHaveCMake := False;
  Log(Format('Detected: Visual Studio 2022 C++ = %s [%s %s], CMake = %d', [gVSPath, gVSName, gVSVersion, Ord(gHaveCMake)]));
end;

{ Smart App Control (item 4, docs/RISKS.md R10): when it's on and enforcing,
  Windows blocks programs that aren't signed or known to Microsoft, so the
  game Setup builds here won't start. 1 = on; 2 = evaluation (doesn't block);
  0 = off. /SIMULATESAC (testing) reads "on". }
function SmartAppControlOn(): Boolean;
var
  V: Cardinal;
begin
  if HasParam('SIMULATESAC') then begin
    Result := True;
    exit;
  end;
  Result := RegQueryDWordValue(HKLM, 'SYSTEM\CurrentControlSet\Control\CI\Policy', 'VerifiedAndReputablePolicyState', V) and (V = 1);
end;

function WantVS(): Boolean;
begin
  Result := (not gHaveVS) and (VSCheck.Checked or gVSPlanOnly);
end;

function NeedPipCMake(): Boolean;
begin
  { The Build Tools Setup adds include CMake; otherwise pip's CMake when none is found. }
  Result := (not gHaveCMake) and not (WantVS() and not gVSPlanOnly);
end;

function NeedPython(): Boolean;
var
  S: AnsiString;
begin
  // WizardDirValue: the app constant isn't set yet while the pages are being filled
  Result := not (FileExists(AddBackslash(WizardDirValue) + 'python\python.exe')
    and LoadStringFromFile(AddBackslash(WizardDirValue) + 'python\.bw_python', S)
    and (Trim(S) = '{#PyVer} {#PyZipSha256}'));
end;

procedure PythonUnpacked();
begin
  { Python's manual (71 MB) isn't needed to build or play. }
  DelTree(ExpandConstant('{app}\python\Doc'), True, True, True);
  SaveStringToFile(ExpandConstant('{app}\python\.bw_python'), '{#PyVer} {#PyZipSha256}', False);
end;

function BuildSucceeded(): Boolean;
begin
  Result := gBuildOK;
end;

{ ── Pages ────────────────────────────────────────────────────────────── }

procedure OwnCheckClick(Sender: TObject);
begin
  WizardForm.NextButton.Enabled := OwnCheck.Checked;
end;

{ Next goes on only when the path in the box is the one that was checked and
  the copy is usable. While the box is empty or the copy can't be used, Next
  stays greyed out and the status line says what's needed. }
procedure UpdateDiscNext();
begin
  if WizardForm.CurPageID = DiscPage.ID then
    WizardForm.NextButton.Enabled := (gDiscChecked <> '') and (Trim(DiscEdit.Text) = gDiscChecked);
end;

{ The mark beside the path (docs/025/STYLE.md's status colours, for Windows'
  light or dark mode): a tick for a recognised copy, ! for one Setup doesn't
  recognise, a cross for one it can't use. Never colour alone: the status line
  says it in words. }
procedure ShowMark(Kind: Integer; Shown: Boolean);
begin
  DiscMark.Visible := Shown;
  if Kind = DISC_VERIFIED then begin
    DiscMark.Caption := #$2714;
    if IsDarkInstallMode() then DiscMark.Font.Color := $8ECF3F else DiscMark.Font.Color := $4E7F1B;
  end else if Kind = DISC_UNVERIFIED then begin
    DiscMark.Caption := '!';
    if IsDarkInstallMode() then DiscMark.Font.Color := $41B0F5 else DiscMark.Font.Color := $005B9A;
  end else begin
    DiscMark.Caption := #$2716;
    if IsDarkInstallMode() then DiscMark.Font.Color := $5F4AFF else DiscMark.Font.Color := $2F18C0;
  end;
end;

procedure ShowDiscResult();
var
  Msg: String;
begin
  gDisc := Trim(DiscEdit.Text);
  gDiscEvaluated := gDisc;
  gUnverifiedAccepted := False;          { a new copy needs its own answer }
  if gDisc = '' then begin
    gDiscChecked := '';
    gDiscKind := DISC_UNUSABLE;
    DiscStatus.Caption := 'Press Browse, or type or paste where your copy is.';
    ShowMark(DISC_UNUSABLE, False);
  end else begin
    if DirExists(gDisc) then
      FolderRadio.Checked := True        { the choice follows what was picked, for Browse }
    else if FileExists(gDisc) then
      ImageRadio.Checked := True;
    gDiscKind := CheckDisc(gDisc, Msg);
    if gDiscKind <> DISC_UNUSABLE then begin
      gDiscChecked := gDisc;
      gDiscVerdict := Msg;
      DiscStatus.Caption := Msg;
    end else begin
      gDiscChecked := '';
      DiscStatus.Caption := 'Not usable: ' + Msg;
    end;
    ShowMark(gDiscKind, True);
  end;
  UpdateDiscNext();
end;

{ The box changed (typed, pasted or browsed). A path that exists is checked at
  once (a folder: about a second; an image: a few seconds at most); one that
  doesn't exist yet just says so, so typing isn't slowed down. }
procedure DiscChanged(Sender: TObject);
var
  S: String;
begin
  S := Trim(DiscEdit.Text);
  if S = gDiscEvaluated then begin
    UpdateDiscNext();
    exit;
  end;
  if (S <> '') and not DirExists(S) and not FileExists(S) then begin
    gDiscEvaluated := '';
    gDiscChecked := '';
    gDiscKind := DISC_UNUSABLE;
    ShowMark(DISC_UNUSABLE, False);
    DiscStatus.Caption := 'Setup can''t find this yet. Type the full path, or press Browse.';
    UpdateDiscNext();
    exit;
  end;
  ShowDiscResult();
end;

procedure BrowseClick(Sender: TObject);
var
  S: String;
begin
  S := Trim(DiscEdit.Text);
  if ImageRadio.Checked then begin
    if GetOpenFileName('Choose your Blood Wake disc image', S, ExtractFileDir(S),
        'Xbox disc images (*.iso;*.xiso)|*.iso;*.xiso|All files (*.*)|*.*', 'iso') then
      DiscEdit.Text := S;      { OnChange checks it }
  end else begin
    if BrowseForFolder('Choose the folder with your Blood Wake disc''s files (the one with default.xbe in it):', S, False) then
      DiscEdit.Text := S;
  end;
  DiscChanged(nil);            { the same path again: OnChange doesn't fire }
end;

procedure FillDepsPage();
var
  T, NL: String;
begin
  NL := #13#10;
  T := 'What Setup found on this PC:' + NL + NL;
  T := T + '-  Python {#PyVer}, private to Blood Wake Recomp: ';
  if NeedPython() then
    T := T + 'from python.org (33 MB). It doesn''t change PATH or any other Python.' + NL
  else
    T := T + 'already in the install folder.' + NL;
  T := T + '-  Python packages capstone 5.0.9 and pefile 2024.8.26: from pypi.org (1.3 MB).' + NL;
  T := T + '-  Microsoft''s C++ compiler (Visual Studio 2022): ';
  if gHaveVS then
    T := T + 'found, ' + gVSName + ' ' + gVSVersion + '.' + NL
  else
    T := T + 'not found.' + NL;
  T := T + '-  CMake 3.20 or newer: ';
  if gHaveCMake then
    T := T + 'found.' + NL
  else if gHaveVS then
    T := T + 'not found; cmake 4.4.4 from pypi.org (42 MB).' + NL
  else
    T := T + 'comes with the Build Tools below.' + NL;
  T := T + '-  The xboxrecomp toolkit (commit f0f8a46): from GitHub, during the build.';
  DepsText.Caption := T;
  VSCheck.Visible := not gHaveVS;
  VSNote.Visible := not gHaveVS;
end;

procedure InitializeWizard();
var
  Memo: TNewMemo;
  Intro: TNewStaticText;
  Browse: TNewButton;
  Text: AnsiString;
  V: String;
begin
  gVSPlanOnly := HasParam('VSPLANONLY');
  gStub := HasParam('STUBBUILD');
  gBuildExit := 0;
  DetectDeps();

  { The disclaimer }
  DisclaimerPage := CreateCustomPage(wpWelcome, 'Before you start',
    'Setup goes on once you confirm you own the game.');
  ExtractTemporaryFile('disclaimer.txt');
  LoadStringFromFile(ExpandConstant('{tmp}\disclaimer.txt'), Text);
  Memo := TNewMemo.Create(DisclaimerPage);
  Memo.Parent := DisclaimerPage.Surface;
  Memo.Left := 0;
  Memo.Top := 0;
  Memo.Width := DisclaimerPage.SurfaceWidth;
  Memo.Height := DisclaimerPage.SurfaceHeight - ScaleY(30);
  Memo.ScrollBars := ssVertical;
  Memo.ReadOnly := True;
  Memo.WordWrap := True;
  V := String(Text);
  StringChangeEx(V, ' <!-- audit:allow off-limits -->', '', True);   { the public audit's marker }
  StringChangeEx(V, #13#10, #10, True);
  StringChangeEx(V, #10, #13#10, True);
  Memo.Text := V;
  OwnCheck := TNewCheckBox.Create(DisclaimerPage);
  OwnCheck.Parent := DisclaimerPage.Surface;
  OwnCheck.Left := 0;
  OwnCheck.Top := Memo.Top + Memo.Height + ScaleY(10);
  OwnCheck.Width := DisclaimerPage.SurfaceWidth;
  OwnCheck.Height := ScaleY(17);
  OwnCheck.Caption := 'I own Blood Wake and made this copy from my own disc';
  OwnCheck.OnClick := @OwnCheckClick;
  OwnCheck.Checked := HasParam('IOWNTHEGAME');

  { The disc }
  DiscPage := CreateCustomPage(DisclaimerPage.ID, 'Choose your disc',
    'Setup builds the game from your own copy.');
  Intro := TNewStaticText.Create(DiscPage);
  Intro.Parent := DiscPage.Surface;
  Intro.Width := DiscPage.SurfaceWidth;
  Intro.WordWrap := True;
  Intro.Caption := 'Choose the disc image you made from your Blood Wake disc (a full disc image or an XISO), or a folder of its files. ' +
    'Setup only reads it.';
  Intro.AdjustHeight;
  ImageRadio := TNewRadioButton.Create(DiscPage);
  ImageRadio.Parent := DiscPage.Surface;
  ImageRadio.Top := Intro.Top + Intro.Height + ScaleY(12);
  ImageRadio.Width := DiscPage.SurfaceWidth;
  ImageRadio.Height := ScaleY(17);
  ImageRadio.Caption := 'A disc image file (.iso)';
  ImageRadio.Checked := True;
  FolderRadio := TNewRadioButton.Create(DiscPage);
  FolderRadio.Parent := DiscPage.Surface;
  FolderRadio.Top := ImageRadio.Top + ImageRadio.Height + ScaleY(6);
  FolderRadio.Width := DiscPage.SurfaceWidth;
  FolderRadio.Height := ScaleY(17);
  FolderRadio.Caption := 'A folder with the disc''s files';
  DiscEdit := TNewEdit.Create(DiscPage);
  DiscEdit.Parent := DiscPage.Surface;
  DiscEdit.Top := FolderRadio.Top + FolderRadio.Height + ScaleY(12);
  DiscEdit.Width := DiscPage.SurfaceWidth - ScaleX(118);
  DiscMark := TNewStaticText.Create(DiscPage);
  DiscMark.Parent := DiscPage.Surface;
  DiscMark.AutoSize := False;
  DiscMark.Font.Name := 'Segoe UI Symbol';
  DiscMark.Font.Size := 13;
  DiscMark.Font.Style := [fsBold];
  DiscMark.Left := DiscEdit.Left + DiscEdit.Width + ScaleX(4);
  DiscMark.Top := DiscEdit.Top - ScaleY(4);
  DiscMark.Width := ScaleX(22);
  DiscMark.Height := ScaleY(26);
  DiscMark.Alignment := taCenter;
  DiscMark.StyleElements := [seClient, seBorder];   { keep its own colour under Setup's light or dark style }
  DiscMark.Visible := False;
  Browse := TNewButton.Create(DiscPage);
  Browse.Parent := DiscPage.Surface;
  Browse.Top := DiscEdit.Top - ScaleY(1);
  Browse.Left := DiscPage.SurfaceWidth - ScaleX(80);
  Browse.Width := ScaleX(80);
  Browse.Height := ScaleY(23);
  Browse.Caption := 'B&rowse...';
  Browse.OnClick := @BrowseClick;
  DiscStatus := TNewStaticText.Create(DiscPage);
  DiscStatus.Parent := DiscPage.Surface;
  DiscStatus.Top := DiscEdit.Top + DiscEdit.Height + ScaleY(14);
  DiscStatus.Width := DiscPage.SurfaceWidth;
  DiscStatus.AutoSize := False;
  DiscStatus.WordWrap := True;
  DiscStatus.Height := ScaleY(80);
  DiscStatus.Caption := '';
  { Silent installs use /DISC only (NextButtonClick stops them without it); the
    wizard offers the last install's disc. }
  if (not CmdParam('DISC', V) or (V = '')) and not WizardSilent() then
    V := GetPreviousData('Disc', '');
  DiscEdit.Text := V;
  if DirExists(V) then
    FolderRadio.Checked := True;
  ShowDiscResult();
  DiscEdit.OnChange := @DiscChanged;

  { The warning for a copy Setup doesn't recognise (item 2). Accept goes on,
    Decline stops Setup, Back picks another copy. }
  WarnPage := CreateCustomPage(DiscPage.ID, 'This copy isn''t recognised',
    'You can still build it. Read this first.');
  Memo := TNewMemo.Create(WarnPage);
  Memo.Parent := WarnPage.Surface;
  Memo.Left := 0;
  Memo.Top := 0;
  Memo.Width := WarnPage.SurfaceWidth;
  Memo.Height := WarnPage.SurfaceHeight;
  Memo.ScrollBars := ssVertical;
  Memo.ReadOnly := True;
  Memo.WordWrap := True;
  Memo.Text :=
    'Its main program isn''t the ' + RETAIL_LABEL + ' one Setup knows: it may be another region, a modified copy or a damaged one.' + #13#10#13#10 +
    'Ownership' + #13#10 +
    'You must own the game this copy came from. Blood Wake Recomp contains nothing from the game.' + #13#10#13#10 +
    'Safety' + #13#10 +
    'Setup turns the copy''s program into one that runs on this PC, so a tampered copy could act as malware. ' +
    'Use only a copy you made from your own disc.' + #13#10#13#10 +
    'The game' + #13#10 +
    'Another version may not build, may crash or may behave differently. Remastered''s fixes turn off where its code differs. ' +
    'Saved games may not carry over, and help with problems is limited.' + #13#10#13#10 +
    'Accept builds it, marked as an unverified copy. Decline stops Setup. Back lets you choose another copy.';

  { What's needed }
  DepsPage := CreateCustomPage(wpSelectDir, 'What Blood Wake needs',
    'Setup adds what''s missing, each from its official source, hash-checked.');
  DepsText := TNewStaticText.Create(DepsPage);
  DepsText.Parent := DepsPage.Surface;
  DepsText.Width := DepsPage.SurfaceWidth;
  DepsText.AutoSize := False;
  DepsText.WordWrap := True;
  DepsText.Height := ScaleY(132);
  VSCheck := TNewCheckBox.Create(DepsPage);
  VSCheck.Parent := DepsPage.Surface;
  VSCheck.Top := DepsText.Top + DepsText.Height + ScaleY(6);
  VSCheck.Width := DepsPage.SurfaceWidth;
  VSCheck.Height := ScaleY(17);
  VSCheck.Caption := 'Install Microsoft''s Build Tools for Visual Studio 2022 (C++ only), version {#VsVer}';
  VSCheck.Checked := HasParam('INSTALLVS') or gVSPlanOnly or not WizardSilent();
  VSNote := TNewStaticText.Create(DepsPage);
  VSNote.Parent := DepsPage.Surface;
  VSNote.Top := VSCheck.Top + VSCheck.Height + ScaleY(4);
  VSNote.Left := ScaleX(16);
  VSNote.Width := DepsPage.SurfaceWidth - ScaleX(16);
  VSNote.AutoSize := False;
  VSNote.WordWrap := True;
  VSNote.Height := ScaleY(48);
  VSNote.Caption := 'From microsoft.com, about 3 GB on drive C:. Windows asks for permission for this one step. Other programs can use them too, so the uninstaller keeps them unless you ask.';
  FillDepsPage();

  { Smart App Control (item 4): shown only when it's on }
  gSacOn := SmartAppControlOn();
  Log(Format('Smart App Control on: %d', [Ord(gSacOn)]));
  SacPage := CreateCustomPage(DepsPage.ID, 'Smart App Control will block the game',
    'It''s on on this PC.');
  Intro := TNewStaticText.Create(SacPage);
  Intro.Parent := SacPage.Surface;
  Intro.Width := SacPage.SurfaceWidth;
  Intro.WordWrap := True;
  Intro.Caption :=
    'The game is built on this PC, so it isn''t code-signed, and Smart App Control blocks programs that aren''t. ' +
    'To play, turn it off: Windows Security > App & browser control > Smart App Control settings > Off. ' +
    'It''s a setting for the whole PC (on some versions of Windows it can''t be turned back on without reinstalling), so it''s your choice.' + #13#10#13#10 +
    'Continue builds the game now; it starts once Smart App Control is off.';
  Intro.ShowAccelChar := False;    { "App & browser control": the & is text }
  Intro.AdjustHeight;
end;

function SacNote(): String;
begin
  Result := '';
  if gSacOn then
    Result := #13#10#13#10 + 'Smart App Control is on, so Windows will block the game until you turn it off: ' +
      'Windows Security > App & browser control > Smart App Control settings > Off.';
end;

{ Where to start the game, for the shortcuts that were actually made (item 13). }
function StartWhere(): String;
var
  S, D: Boolean;
begin
  S := WizardIsTaskSelected('startmenuicon');
  D := WizardIsTaskSelected('desktopicon');
  if S and D then
    Result := 'Start it from the Start menu or the desktop: "Blood Wake Recomp".'
  else if S then
    Result := 'Start it from the Start menu: "Blood Wake Recomp".'
  else if D then
    Result := 'Start it from the desktop: "Blood Wake Recomp".'
  else
    Result := 'You chose no shortcuts: start it with "Blood Wake.cmd" in ' + ExpandConstant('{app}') + ', or tick the box below to open the launcher now.';
end;

procedure CurPageChanged(CurPageID: Integer);
var
  V: String;
begin
  if CurPageID = DisclaimerPage.ID then begin
    { interactive only: silent installs reach NextButtonClick, which logs why it stops }
    if not WizardSilent() then
      WizardForm.NextButton.Enabled := OwnCheck.Checked;
  end else if CurPageID = DiscPage.ID then begin
    if not WizardSilent() then
      UpdateDiscNext();
  end;
  { The warning page's own buttons: Accept and Decline }
  if CurPageID = WarnPage.ID then begin
    WizardForm.NextButton.Caption := '&Accept';
    WizardForm.CancelButton.Caption := '&Decline';
  end else
    WizardForm.CancelButton.Caption := SetupMessage(msgButtonCancel);
  if CurPageID = SacPage.ID then
    WizardForm.NextButton.Caption := '&Continue';
  if CurPageID = DepsPage.ID then
    FillDepsPage()
  else if CurPageID = wpFinished then begin
    if gBuildOK then
      WizardForm.FinishedLabel.Caption := 'Blood Wake is built and ready.' + #13#10#13#10 + gFingerprint + #13#10#13#10 +
        StartWhere() + ' In the launcher, choose Original (as shipped) or Remastered, then press Play.' +
        SacNote()
    else
      WizardForm.FinishedLabel.Caption := 'The game wasn''t built:' + #13#10#13#10 + gBuildMsg + #13#10#13#10 +
        'Logs: ' + ExpandConstant('{app}\work\logs') + '. Run Setup again once it''s fixed, or remove Blood Wake Recomp in Settings > Apps.';
    { Inno sized the label for its own text: fit ours, and keep the launcher box below it }
    WizardForm.FinishedLabel.AdjustHeight;
    WizardForm.RunList.Top := WizardForm.FinishedLabel.Top + WizardForm.FinishedLabel.Height + ScaleY(12);
    V := WizardForm.FinishedLabel.Caption;
    StringChangeEx(V, #13#10, ' / ', True);
    Log('Finished page: ' + V);
  end;
end;

{ The install folder must be new, empty, or an earlier Blood Wake Recomp install:
  the uninstaller removes the folders Setup and the build put there (game, build,
  tools, work, python...), and must never meet someone else's folder of that name. }
{ What an earlier Blood Wake Recomp leaves in its folder after an uninstall:
  the game's runtime files (0.2.4's uninstaller didn't remove them), and the
  Python or the game's files when the player chose to keep them. }
function KnownLeftover(const Dir, Name: String; IsDir: Boolean): Boolean;
var
  N: String;
begin
  N := Lowercase(Name);
  if IsDir then
    Result := ((N = 'python') and FileExists(AddBackslash(Dir) + 'python\.bw_python'))
      or ((N = 'game') and DirExists(AddBackslash(Dir) + 'game\disc'))
  else
    Result := (N = 'play.log') or (N = 'xbox_kernel.log') or (N = 'bw_launch_next.bin') or (N = 'bw_relaunch.bin')
      or (N = 'kept_by_uninstall.txt') or WildcardMatch(N, 'icall_*.dump') or WildcardMatch(N, '*.dmp');
end;

function FolderUsable(const Dir: String): Boolean;
var
  F: TFindRec;
begin
  Result := True;
  if not DirExists(Dir) then
    exit;
  if FileExists(AddBackslash(Dir) + 'installer.ini') or FileExists(AddBackslash(Dir) + 'unins000.dat')
      or FileExists(AddBackslash(Dir) + 'kept_by_uninstall.txt') then
    exit;
  if FindFirst(AddBackslash(Dir) + '*', F) then
    try
      repeat
        if (F.Name <> '.') and (F.Name <> '..')
            and not KnownLeftover(Dir, F.Name, (F.Attributes and FILE_ATTRIBUTE_DIRECTORY) <> 0) then begin
          Log('The install folder holds something that isn''t Blood Wake Recomp''s: ' + F.Name);
          Result := False;
          exit;
        end;
      until not FindNext(F);
    finally
      FindClose(F);
    end;
end;

{ The folder Blood Wake Recomp is already installed in (its Settings > Apps
  entry), or '' when it isn't, or when that install's uninstaller is gone (the
  folder was deleted by hand: nothing is left to protect). }
function RegisteredDir(): String;
begin
  Result := WizardForm.PrevAppDir;
  if (Result <> '') and not FileExists(AddBackslash(Result) + 'unins000.exe') then begin
    Log('An earlier install is registered in ' + Result + ', but its uninstaller is gone: treated as not installed.');
    Result := '';
  end;
end;

function SameDir(const A, B: String): Boolean;
begin
  Result := Lowercase(RemoveBackslashUnlessRoot(ExpandFileName(A))) = Lowercase(RemoveBackslashUnlessRoot(ExpandFileName(B)));
end;

{ Item 98: there's one Settings > Apps entry and one set of shortcuts, so a
  second install in another folder would take over the first's, and
  uninstalling it would leave the first with neither. When it's already
  installed, Setup updates that install, or stops. }
function OneInstallOnly(): Boolean;
var
  R: String;
begin
  Result := True;
  R := RegisteredDir();
  if (R = '') or SameDir(WizardDirValue, R) then
    exit;
  if WizardSilent() then begin
    Log('Stopped at the folder page: Blood Wake Recomp is already installed in ' + R + ', and /DIR names another folder (' +
      WizardDirValue + '). Install into ' + R + ' to update it, or uninstall it first to move it.');
    Result := False;
    exit;
  end;
  Log('Already installed in ' + R + '; the folder page names ' + WizardDirValue + ': asking.');
  if TaskDialogMsgBox('Blood Wake Recomp is already installed',
      'It''s installed in ' + R + '. Setup can update it there.' + #13#10#13#10 +
      'To move it, uninstall it first (Settings > Apps): two installs would share one Apps entry and the same shortcuts.',
      mbConfirmation, MB_YESNO, ['Install it there instead' + #10 + 'Updates the install in ' + R, 'Cancel Setup'], 0) = IDYES then begin
    WizardForm.DirEdit.Text := R;
    Log('Installing into the existing folder instead: ' + R);
  end else begin
    Log('Cancelled: Blood Wake Recomp is already installed in ' + R + '.');
    gQuitNoConfirm := True;
    WizardForm.Close;
    Result := False;
  end;
end;

function NextButtonClick(CurPageID: Integer): Boolean;
var
  Msg: String;
begin
  Result := True;
  if CurPageID = wpSelectDir then begin
    if not OneInstallOnly() then begin
      Result := False;
      exit;
    end;
    if not FolderUsable(WizardDirValue) then begin
      Msg := 'Choose a new or empty folder: ' + WizardDirValue + ' already has other things in it, and Setup needs a folder of its own.';
      Log('Stopped at the folder page: ' + Msg);
      SuppressibleMsgBox(Msg, mbError, MB_OK, IDOK);
      Result := False;
    end;
  end else if CurPageID = DisclaimerPage.ID then begin
    if not OwnCheck.Checked then begin
      Log('Stopped: the disclaimer was not accepted (silent installs need /IOWNTHEGAME).');
      Result := False;
    end;
  end else if CurPageID = DiscPage.ID then begin
    { Only a path that was checked and passed goes on: an empty box never does
      (both '' would otherwise match). }
    gDisc := Trim(DiscEdit.Text);
    if gDisc <> gDiscEvaluated then
      ShowDiscResult();
    if (gDiscChecked = '') or (gDisc <> gDiscChecked) then begin
      if gDisc = '' then begin
        if WizardSilent() then
          Msg := 'No disc was given: silent installs need /DISC="<folder or disc image>".'
        else
          Msg := 'Choose your copy of Blood Wake first: press Browse, or type or paste where it is.';
      end else
        CheckDisc(gDisc, Msg);
      Log('Stopped at the disc page: ' + Msg);
      SuppressibleMsgBox(Msg, mbError, MB_OK, IDOK);
      Result := False;
    end else
      Log('Disc: ' + gDisc + ': ' + gDiscVerdict);
  end else if CurPageID = WarnPage.ID then begin
    if WizardSilent() and not HasParam('ACCEPTUNVERIFIED') then begin
      Log('Stopped at the warning page: Setup doesn''t recognise this copy (' + gDisc + '). Silent installs of an unverified copy need /ACCEPTUNVERIFIED as well as /IOWNTHEGAME.');
      Result := False;
    end else begin
      gUnverifiedAccepted := True;
      Log('The unverified-copy warning was accepted for ' + gDisc + ': the build goes on with --allow-unverified.');
    end;
  end else if CurPageID = SacPage.ID then begin
    Log('Smart App Control is on: the game will be blocked until it''s turned off (Windows Security > App & browser control > Smart App Control settings > Off). Going on, as chosen.');
  end else if CurPageID = DepsPage.ID then begin
    if (not gHaveVS) and (not WantVS()) then begin
      Msg := 'Blood Wake can''t be built without Microsoft''s C++ build tools. Tick the box to let Setup install them, or install Visual Studio 2022 (with "Desktop development with C++") yourself and run Setup again.';
      Log('Stopped at the dependencies page: ' + Msg);
      SuppressibleMsgBox(Msg, mbError, MB_OK, IDOK);
      Result := False;
    end;
  end;
end;

{ The warning page is only for a copy Setup doesn't recognise. }
function ShouldSkipPage(PageID: Integer): Boolean;
begin
  Result := ((PageID = WarnPage.ID) and (gDiscKind <> DISC_UNVERIFIED))
    or ((PageID = SacPage.ID) and not gSacOn)
    { item 106: with Visual Studio found there's nothing to decide; the Ready page lists the downloads }
    or ((PageID = DepsPage.ID) and gHaveVS);
end;

{ Decline on the warning page stops Setup at once: the answer was given. }
procedure CancelButtonClick(CurPageID: Integer; var Cancel, Confirm: Boolean);
begin
  if gQuitNoConfirm then
    Confirm := False                    { the player already chose "Cancel Setup" (item 98) }
  else if CurPageID = WarnPage.ID then begin
    Confirm := False;
    Log('The unverified-copy warning was declined: Setup stops, nothing was installed.');
  end;
end;

procedure RegisterPreviousData(PreviousDataKey: Integer);
begin
  SetPreviousData(PreviousDataKey, 'Disc', gDisc);
end;

function UpdateReadyMemo(Space, NewLine, MemoUserInfoInfo, MemoDirInfo, MemoTypeInfo,
  MemoComponentsInfo, MemoGroupInfo, MemoTasksInfo: String): String;
var
  S: String;
begin
  S := 'Your copy of Blood Wake:' + NewLine + Space + gDisc + NewLine;
  if gDiscKind = DISC_VERIFIED then
    S := S + Space + 'Recognised: Blood Wake, ' + RETAIL_LABEL + NewLine + NewLine
  else
    S := S + Space + 'Not recognised: built anyway, as an unverified copy' + NewLine + NewLine;
  S := S + MemoDirInfo + NewLine + NewLine;
  S := S + 'Setup downloads (official sources, hash-checked):' + NewLine;
  if NeedPython() then
    S := S + Space + 'Python {#PyVer} from python.org (33 MB)' + NewLine;
  S := S + Space + 'capstone 5.0.9 and pefile 2024.8.26 from pypi.org (1.3 MB)' + NewLine;
  if NeedPipCMake() then
    S := S + Space + 'CMake 4.4.4 from pypi.org (42 MB)' + NewLine;
  S := S + Space + 'the xboxrecomp toolkit from GitHub' + NewLine;
  if WantVS() then begin
    if gVSPlanOnly then
      S := S + Space + 'Microsoft''s Build Tools bootstrapper {#VsVer} (plan only: it won''t be run)' + NewLine
    else
      S := S + Space + 'Microsoft''s Build Tools for Visual Studio 2022 {#VsVer} (about 3 GB; asks for permission)' + NewLine;
  end;
  S := S + NewLine + 'Then it builds the game and prepares its graphics: about 15 minutes, longer on an older PC.' + NewLine +
    'You can use the PC meanwhile.' + NewLine;
  if MemoTasksInfo <> '' then
    S := S + NewLine + MemoTasksInfo + NewLine;
  Result := S;
end;

{ ── Install: Build Tools, Python packages, the build ─────────────────── }

function JsonValue(const Line, Key: String): String;
var
  P, I, N: Integer;
  C: Char;
  S: String;
begin
  Result := '';
  P := Pos('"' + Key + '": ', Line);
  if P = 0 then
    exit;
  I := P + Length(Key) + 4;
  N := Length(Line);
  S := '';
  if (I <= N) and (Line[I] = '"') then begin
    I := I + 1;
    while I <= N do begin
      C := Line[I];
      if C = '"' then
        break;
      if (C = '\') and (I < N) then begin
        I := I + 1;
        C := Line[I];
        if (C = 'n') or (C = 't') then
          S := S + ' '
        else if C = 'r' then
          S := S
        else if (C = 'u') and (I + 4 <= N) then begin
          S := S + Chr(StrToIntDef('$' + Copy(Line, I + 1, 4), 63));
          I := I + 4;
        end else
          S := S + C;
      end else
        S := S + C;
      I := I + 1;
    end;
  end else begin
    while (I <= N) and (Line[I] <> ',') and (Line[I] <> '}') do begin
      S := S + Line[I];
      I := I + 1;
    end;
    S := Trim(S);
  end;
  Result := S;
end;

function StepText(const Step: String): String;
begin
  if Step = 'deps' then Result := 'Setting up Python''s packages...'
  else if Step = 'identify' then Result := 'Looking at your copy of Blood Wake...'
  else if Step = 'toolkit' then Result := 'Getting the xboxrecomp toolkit...'
  else if Step = 'unpack' then Result := 'Copying the game''s files from your copy...'
  else if Step = 'fingerprint' then Result := 'Checking your copy...'
  else if Step = 'pipeline' then Result := 'Translating the game''s code (the longest step)...'
  else if Step = 'verify' then Result := 'Checking the translated code...'
  else if Step = 'build' then Result := 'Compiling the game...'
  else if Step = 'stage' then Result := 'Finishing...'
  else if Step = 'warmup' then Result := 'Preparing graphics...'
  else if Step = 'done' then Result := 'Done.'
  else Result := WizardForm.StatusLabel.Caption;
end;

procedure OnHelperLine(const S: String; const Error, FirstLine: Boolean);
var
  Step, Msg: String;
  Pct: Integer;
begin
  if Error then begin
    Log('helper output: ' + S);
    exit;
  end;
  Log('| ' + S);
  if (Length(S) = 0) or (S[1] <> '{') then
    exit;
  Step := JsonValue(S, 'step');
  Msg := JsonValue(S, 'msg');
  Pct := StrToIntDef(JsonValue(S, 'pct'), -1);
  if JsonValue(S, 'error') = 'true' then begin
    gBuildMsg := Msg;
    gBuildExit := StrToIntDef(JsonValue(S, 'exit'), 1);
    gBuildReason := JsonValue(S, 'reason');
    gBuildDetail := JsonValue(S, 'detail');
  end;
  if Step = 'log' then
    exit;
  if (Step = 'fingerprint') and (JsonValue(S, 'error') <> 'true') and (Pct >= 20) then begin
    if (JsonValue(S, 'verdict') = '') or (JsonValue(S, 'verdict') = 'verified') then
      gFingerprint := 'Your copy checks out: ' + Msg + '.'
    else
      gFingerprint := 'Your copy: ' + Msg + '.';
  end;
  if Msg <> '' then
    gLastMsg := Msg;
  WizardForm.StatusLabel.Caption := StepText(Step);
  WizardForm.FilenameLabel.Caption := Msg;
  if Pct >= 0 then
    WizardForm.ProgressGauge.Position := gBase + (Pct * gSpan) div 100;
end;

function RunHelper(const Params: String; var Code: Integer): Boolean;
var
  Py: String;
begin
  Py := ExpandConstant('{app}\python\python.exe');
  Log('Running: ' + Py + ' -I -u setup_helper.py ' + Params);
  try
    Result := ExecAndLogOutput(Py, '-I -u ' + ArgQuote(ExpandConstant('{app}\setup\setup_helper.py')) + ' ' + Params,
      ExpandConstant('{app}'), SW_HIDE, ewWaitUntilTerminated, Code, @OnHelperLine);
  except
    Log('Running the helper failed: ' + GetExceptionMessage);
    Result := False;
    Code := -1;
  end;
end;

{ The build without Exec's wait (item 10). Inno disables its window while Exec
  waits, so Cancel couldn't be pressed. The helper is started without waiting;
  it writes its progress lines (the first with its process id) to a file, and
  Setup reads them while it keeps its own window answering, until the helper
  ends. A message is passed through untouched, so its layout doesn't matter. }
type
  TMsgBuf = record
    A, B, C, D, E, F, G, H, I, J, K, L, M, N, O, P: Longint;
  end;

function PeekMessage(var Msg: TMsgBuf; Wnd: Longint; MinF, MaxF, Remove: Cardinal): Boolean;
  external 'PeekMessageW@user32 stdcall';
function TranslateMessage(var Msg: TMsgBuf): Boolean;
  external 'TranslateMessage@user32 stdcall';
function DispatchMessage(var Msg: TMsgBuf): Longint;
  external 'DispatchMessageW@user32 stdcall';
function OpenProcess(Access: Cardinal; Inherit: Boolean; Pid: Cardinal): THandle;
  external 'OpenProcess@kernel32 stdcall';
function WaitForSingleObject(H: THandle; Ms: Cardinal): Cardinal;
  external 'WaitForSingleObject@kernel32 stdcall';
function GetExitCodeProcess(H: THandle; var Code: Cardinal): Boolean;
  external 'GetExitCodeProcess@kernel32 stdcall';
function CloseHandle(H: THandle): Boolean;
  external 'CloseHandle@kernel32 stdcall';
function GetTickCount(): Cardinal;
  external 'GetTickCount@kernel32 stdcall';

procedure PumpMessages();
var
  M: TMsgBuf;
begin
  while PeekMessage(M, 0, 0, 0, 1) do begin       { PM_REMOVE }
    TranslateMessage(M);
    DispatchMessage(M);
  end;
end;

function RunHelperLive(const Params: String; var Code: Integer): Boolean;
var
  Py, Prog, S: String;
  Lines: TArrayOfString;
  Seen, N, I, R: Integer;
  Pid, C, T0: Cardinal;
  H: THandle;
  Ended: Boolean;
begin
  Result := False;
  Code := -1;
  Py := ExpandConstant('{app}\python\python.exe');
  Prog := ExpandConstant('{app}\setup\progress.jsonl');
  DeleteFile(Prog);
  Log('Running (live): ' + Py + ' -I -u setup_helper.py ' + Params);
  if not Exec(Py, '-I -u ' + ArgQuote(ExpandConstant('{app}\setup\setup_helper.py')) + ' ' + Params,
      ExpandConstant('{app}'), SW_HIDE, ewNoWait, R) then begin
    Log('The helper did not start: ' + SysErrorMessage(R));
    exit;
  end;
  Seen := 0;
  Pid := 0;
  H := 0;
  Ended := False;
  T0 := GetTickCount();
  repeat
    PumpMessages();
    Sleep(100);
    if (H <> 0) and (WaitForSingleObject(H, 0) = 0) then
      Ended := True;
    if LoadStringsFromFile(Prog, Lines) then begin
      N := GetArrayLength(Lines);
      { The newest line counts once it's whole (each is one JSON object, written at
        once): holding it back until the next one came hid a long step's line. }
      if (not Ended) and (N > 0) then begin
        S := Trim(Lines[N - 1]);
        if (S = '') or (S[Length(S)] <> '}') then
          N := N - 1;
      end;
      for I := Seen to N - 1 do begin
        S := Lines[I];
        if JsonValue(S, 'step') = 'pid' then begin
          Pid := StrToIntDef(JsonValue(S, 'pid'), 0);
          H := OpenProcess($00101000, False, Pid);   { SYNCHRONIZE, PROCESS_QUERY_LIMITED_INFORMATION }
          Log(Format('The helper is process %d', [Pid]));
        end else if JsonValue(S, 'step') <> 'exit' then
          OnHelperLine(S, False, I = 0);
      end;
      if N > Seen then
        Seen := N;
    end;
    if (Pid = 0) and (GetTickCount() - T0 > 120000) then begin
      Log('The helper wrote nothing for 2 minutes: giving up on it.');
      exit;
    end;
  until Ended;
  if GetExitCodeProcess(H, C) then
    Code := C;
  CloseHandle(H);
  Result := True;
end;

function InstallBuildTools(): Boolean;
var
  Boot, Target, Args, Ini: String;
  Code: Integer;
  Started: Boolean;
begin
  Result := False;
  Boot := ExpandConstant('{app}\deps\vs_BuildTools.exe');
  Target := ExpandConstant('{commonpf32}\Microsoft Visual Studio\2022\BuildTools');
  Args := '--passive --wait --norestart --nocache --installPath ' + ArgQuote(Target) +
    ' --add Microsoft.VisualStudio.Workload.VCTools --add Microsoft.VisualStudio.Component.VC.Tools.x86.x64' +
    ' --add Microsoft.VisualStudio.Component.VC.CMake.Project --includeRecommended';
  if gVSPlanOnly then begin
    SaveStringToFile(ExpandConstant('{app}\deps\vs_plan.txt'),
      'Plan only (/VSPLANONLY): Setup did not run this. It would run, with a Windows permission prompt:' + #13#10 +
      ArgQuote(Boot) + ' ' + Args + #13#10 +
      'The bootstrapper was downloaded from ' + '{#VsUrl}' + #13#10 +
      'and matched the pinned SHA-256 {#VsSha256} (Build Tools {#VsVer}).' + #13#10, False);
    Log('VS Build Tools: plan only, written to deps\vs_plan.txt: ' + Boot + ' ' + Args);
    Result := True;
    exit;
  end;
  WizardForm.StatusLabel.Caption := 'Installing Microsoft''s Build Tools for Visual Studio 2022...';
  WizardForm.FilenameLabel.Caption := 'Windows asks for permission. This takes 10 to 30 minutes and downloads about 2 GB from Microsoft.';
  Log('VS Build Tools: ' + Boot + ' ' + Args);
  if HasParam('SIMULATEUACDECLINE') then begin
    { testing: exactly what ShellExec reports when Windows' permission prompt is declined }
    Log('VS Build Tools: /SIMULATEUACDECLINE, not run; as if the permission prompt was declined (error 1223)');
    Started := False;
    Code := 1223;
  end else
    Started := ShellExec('', Boot, Args, '', SW_SHOWNORMAL, ewWaitUntilTerminated, Code);
  if not Started then begin
    if Code = 1223 then         { ERROR_CANCELLED: the prompt was declined }
      gBuildMsg := 'Microsoft''s build tools weren''t installed: Windows'' permission prompt was declined, so nothing was changed. ' +
        'To build Blood Wake, run Setup again and choose Yes when Windows asks, or install Visual Studio 2022 with "Desktop development with C++" yourself.'
    else
      gBuildMsg := 'Microsoft''s build tools couldn''t be installed (' + SysErrorMessage(Code) + '). Run Setup again, or install Visual Studio 2022 with "Desktop development with C++" yourself.';
    Log('VS Build Tools: ' + gBuildMsg);
    exit;
  end;
  Log(Format('VS Build Tools exit code %d', [Code]));
  if (Code <> 0) and (Code <> 3010) then begin
    gBuildMsg := Format('Microsoft''s Build Tools installer stopped with code %d. Run Setup again, or install Visual Studio 2022 with "Desktop development with C++" yourself.', [Code]);
    exit;
  end;
  Ini := ExpandConstant('{app}\installer.ini');
  SetIniString('vs', 'installed_by_setup', '1', Ini);
  SetIniString('vs', 'path', Target, Ini);
  SetIniString('vs', 'version', '{#VsVer}', Ini);
  gVSInstalledNow := True;
  DetectDeps();
  Result := gHaveVS;
  if not Result then
    gBuildMsg := 'Microsoft''s Build Tools were installed, but Setup can''t find their C++ compiler. Restart Windows and run Setup again.';
end;

procedure MakeShortcuts();
var
  Pyw, Params, Exe: String;
begin
  Pyw := ExpandConstant('{app}\python\pythonw.exe');
  Params := '-E -s ' + ArgQuote(ExpandConstant('{app}\tools\launcher.pyw'));
  Exe := ExpandConstant('{app}\build\Release\bloodwake.exe');
  try
    if WizardIsTaskSelected('startmenuicon') then
      CreateShellLink(ExpandConstant('{autoprograms}\' + LINK_NAME), 'Blood Wake (unofficial PC port): the launcher',
        Pyw, Params, ExpandConstant('{app}'), Exe, 0, SW_SHOWNORMAL)
    else
      DeleteFile(ExpandConstant('{autoprograms}\' + LINK_NAME));      { an earlier install's }
    if not WizardIsTaskSelected('desktopicon') then
      DeleteFile(ExpandConstant('{autodesktop}\' + LINK_NAME))
    else
      CreateShellLink(ExpandConstant('{autodesktop}\' + LINK_NAME), 'Blood Wake (unofficial PC port): the launcher',
        Pyw, Params, ExpandConstant('{app}'), Exe, 0, SW_SHOWNORMAL);
  except
    Log('Shortcut: ' + GetExceptionMessage);
  end;
end;

{ Cancel during the build (item 10). Inno greys Cancel out once its files are
  copied; Setup turns it back on for the build. Pressing it asks first, then
  writes the cancel file: setup_helper.py stops the build and every process it
  started (a job object), and the build ends as a failed one does. }
procedure BuildCancelClick(Sender: TObject);
begin
  if not gInBuild or gBuildCancelled then
    exit;
  if SuppressibleTaskDialogMsgBox('Stop building Blood Wake?',
      'The installed files stay: run Setup again later to build it, or remove Blood Wake Recomp in Settings > Apps.',
      mbConfirmation, MB_YESNO, ['Stop the build', 'Keep building'], 0, IDNO) = IDYES then begin
    gBuildCancelled := True;
    SaveStringToFile(gCancelFile, 'cancel' + #13#10, False);
    WizardForm.CancelButton.Enabled := False;
    WizardForm.StatusLabel.Caption := 'Stopping the build...';
    Log('Cancel pressed during the build: the helper was asked to stop it.');
  end;
end;

procedure RunBuild();
var
  Code: Integer;
  Params, V, Ini: String;
  Retry: Boolean;
  OldCancel: TNotifyEvent;
begin
  gBuildRan := True;
  gBuildOK := False;
  WizardForm.ProgressGauge.Min := 0;
  WizardForm.ProgressGauge.Max := 1000;
  WizardForm.ProgressGauge.Position := 0;
  Ini := ExpandConstant('{app}\installer.ini');
  SetIniString('setup', 'version', '{#AppVer}', Ini);
  SetIniString('setup', 'disc', gDisc, Ini);
  SetIniString('python', 'version', '{#PyVer}', Ini);

  if WantVS() then
    if not InstallBuildTools() then begin
      gBuildExit := 10;
      exit;
    end;

  { Python's packages: 0-2% }
  gBase := 0;
  gSpan := 20;
  WizardForm.StatusLabel.Caption := StepText('deps');
  WizardForm.FilenameLabel.Caption := '';
  if not RunHelper('deps --wheels ' + ArgQuote(ExpandConstant('{app}\deps')), Code) or (Code <> 0) then begin
    if gBuildMsg = '' then
      gBuildMsg := Format('Setting up Python''s packages failed (code %d). Log: %s', [Code, ExpandConstant('{app}\deps\pip.log')]);
    gBuildExit := 3;
    exit;
  end;

  { The build: 2-100%. A copy whose main program is the retail one but whose
    other files differ is only found by the build (the disc page checks the
    program alone): then Setup asks, and builds it again if the player accepts. }
  gCancelFile := ExpandConstant('{app}\setup\cancel.flag');
  DeleteFile(gCancelFile);
  gBuildCancelled := False;
  OldCancel := WizardForm.CancelButton.OnClick;
  WizardForm.CancelButton.OnClick := @BuildCancelClick;
  WizardForm.CancelButton.Enabled := True;
  gInBuild := True;
  repeat
    Retry := False;
    gBase := 20;
    gSpan := 980;
    gBuildMsg := '';
    gBuildReason := '';
    gBuildDetail := '';
    gBuildExit := 0;
    Params := 'build --source ' + ArgQuote(ExpandConstant('{app}\source')) + ' --cancel-file ' + ArgQuote(gCancelFile) +
      ' --progress-file ' + ArgQuote(ExpandConstant('{app}\setup\progress.jsonl'));
    if gStub then
      Params := Params + ' --stub';
    Params := Params + ' -- --disc ' + ArgQuote(gDisc) + ' --install-dir ' + ArgQuote(ExpandConstant('{app}'));
    if gUnverifiedAccepted or HasParam('ACCEPTUNVERIFIED') then
      Params := Params + ' --allow-unverified --accepted-in Setup';
    if CmdParam('JOBS', V) and (StrToIntDef(V, 0) > 0) then
      Params := Params + ' --jobs ' + V;
    if CmdParam('TOOLKIT', V) and (V <> '') then
      Params := Params + ' --toolkit ' + ArgQuote(V);
    if not RunHelperLive(Params, Code) then
      Code := -1;
    Log(Format('Build exit code %d', [Code]));
    if gBuildCancelled then begin
      Code := 8;
      gBuildExit := 8;
      gBuildMsg := 'You stopped the build.';
    end else if (Code = 2) and (gBuildReason = 'files differ') and not gUnverifiedAccepted and not WizardSilent() then begin
      if gBuildDetail = '' then
        gBuildDetail := 'some of its files differ from the retail disc';
      if SuppressibleTaskDialogMsgBox('Build this copy anyway?',
          'Its main program matches the retail one, but ' + gBuildDetail + '.' + #13#10#13#10 +
          'It may be a modified or damaged copy, which may crash or behave differently. You must own the game it came from. ' +
          'It''s marked as a copy whose files differ.',
          mbConfirmation, MB_YESNO, ['Build it anyway', 'Don''t build'], 0, IDNO) = IDYES then begin
        gUnverifiedAccepted := True;
        Log('The "files differ" warning was accepted: building again with --allow-unverified.');
        Retry := True;
      end else
        Log('The "files differ" warning was declined: no build.');
    end;
  until not Retry;
  gInBuild := False;
  WizardForm.CancelButton.Enabled := False;
  WizardForm.CancelButton.OnClick := OldCancel;
  DeleteFile(gCancelFile);
  if Code <> 0 then begin
    if gBuildExit = 0 then
      gBuildExit := Code;
    if gBuildMsg = '' then
      gBuildMsg := Format('The build stopped (code %d): %s', [Code, gLastMsg]);
    exit;
  end;
  if not FileExists(ExpandConstant('{app}\build\Release\bloodwake.exe')) then begin
    gBuildExit := 6;
    gBuildMsg := 'The build finished without bloodwake.exe.';
    exit;
  end;
  gBuildOK := True;
  if gFingerprint = '' then begin
    if gDiscKind = DISC_VERIFIED then
      gFingerprint := 'Your copy checks out: ' + gDiscVerdict
    else
      gFingerprint := 'Your copy isn''t a recognised one: it was built as you accepted.';
  end;
  V := 'setup_version={#AppVer} disc=' + ArgQuote(gDisc) + ' python_private=true';
  if gDiscKind = DISC_VERIFIED then
    V := V + ' disc_page=verified'
  else
    V := V + ' disc_page=unverified';
  if gUnverifiedAccepted or HasParam('ACCEPTUNVERIFIED') then
    V := V + ' unverified_accepted=true'
  else
    V := V + ' unverified_accepted=false';
  if gSacOn then
    V := V + ' smart_app_control=on'
  else
    V := V + ' smart_app_control=off';
  if gVSInstalledNow then
    V := V + ' vs_buildtools_installed_by_setup=true vs_buildtools_path=' + ArgQuote(GetIniString('vs', 'path', '', Ini))
  else
    V := V + ' vs_buildtools_installed_by_setup=false';
  RunHelper('record --install-json ' + ArgQuote(ExpandConstant('{app}\install.json')) + ' ' + V, Code);
  MakeShortcuts();
  WizardForm.ProgressGauge.Position := 1000;
end;

{ Updating a folder the launcher or the game is running from would leave
  locked files behind (item 12). }
function PrepareToInstall(var NeedsRestart: Boolean): String;
begin
  Result := '';
  if not ClosedFirst(WizardDirValue, 'Setup') then
    Result := 'Blood Wake is still running from ' + WizardDirValue + '. Close the game and the launcher, then run Setup again.';
end;

procedure CurStepChanged(CurStep: TSetupStep);
begin
  if CurStep = ssPostInstall then begin
    DeleteFile(ExpandConstant('{app}\kept_by_uninstall.txt'));    { ours again }
    RunBuild();
  end;
end;

function GetCustomSetupExitCode(): Integer;
begin
  Result := 0;
  if gBuildRan and not gBuildOK then begin
    if gBuildExit = 10 then
      Result := 110
    else if (gBuildExit >= 2) and (gBuildExit <= 8) then
      Result := 100 + gBuildExit
    else
      Result := 109;
  end;
end;

{ ── Uninstall ────────────────────────────────────────────────────────── }

function AddCheck(Form: TSetupForm; const Caption: String; Checked: Boolean; var Y: Integer): TNewCheckBox;
begin
  Result := TNewCheckBox.Create(Form);
  Result.Parent := Form;
  Result.Left := ScaleX(16);
  Result.Top := Y;
  Result.Width := Form.ClientWidth - ScaleX(32);
  Result.Height := ScaleY(17);
  Result.Caption := Caption;
  Result.Checked := Checked;
  Y := Y + ScaleY(24);
end;

function InitializeUninstall(): Boolean;
var
  Form: TSetupForm;
  Intro, Warn: TNewStaticText;
  CSaves, CPython, CGame, CShaders, CVS: TNewCheckBox;
  OK, Cancel: TNewButton;
  Y, W: Integer;
begin
  Result := True;
  if not ClosedFirst(ExpandConstant('{app}'), 'the uninstall') then begin
    Result := False;
    exit;
  end;
  uVSOurs := GetIniString('vs', 'installed_by_setup', '0', ExpandConstant('{app}\installer.ini')) = '1';
  uKeepSaves := not HasParam('REMOVESAVES');
  uRemovePython := not HasParam('KEEPPYTHON');
  uRemoveGame := not HasParam('KEEPGAME');
  uRemoveShaders := not HasParam('KEEPSHADERCACHE');
  uRemoveVS := uVSOurs and HasParam('REMOVEVS');
  if UninstallSilent() then
    exit;

  Form := CreateCustomForm(ScaleX(480), ScaleY(340), False, True);
  try
    Form.Caption := 'Uninstall Blood Wake Recomp';
    Intro := TNewStaticText.Create(Form);
    Intro.Parent := Form;
    Intro.Left := ScaleX(16);
    Intro.Top := ScaleY(14);
    Intro.Width := Form.ClientWidth - ScaleX(32);
    Intro.WordWrap := True;
    Intro.Caption := 'The game, its files and its shortcuts are removed. Also remove:';
    Intro.AdjustHeight;
    Y := Intro.Top + Intro.Height + ScaleY(14);
    CSaves := AddCheck(Form, 'My saved games and settings', not uKeepSaves, Y);
    CPython := AddCheck(Form, 'The private Python (in the install folder)', uRemovePython, Y);
    CGame := AddCheck(Form, 'The game''s files copied from my disc (about 2 GB)', uRemoveGame, Y);
    CShaders := AddCheck(Form, 'The shader cache (rebuilt when you play)', uRemoveShaders, Y);
    CVS := nil;
    if uVSOurs then begin
      CVS := AddCheck(Form, 'Microsoft''s Build Tools for Visual Studio 2022', uRemoveVS, Y);
      Warn := TNewStaticText.Create(Form);
      Warn.Parent := Form;
      Warn.Left := ScaleX(34);
      Warn.Top := Y - ScaleY(4);
      Warn.Width := Form.ClientWidth - ScaleX(50);
      Warn.WordWrap := True;
      Warn.Caption := 'Setup installed these for you. Other programs may use them too.';
      Warn.AdjustHeight;
      Y := Warn.Top + Warn.Height + ScaleY(8);
    end;
    OK := TNewButton.Create(Form);
    OK.Parent := Form;
    OK.Caption := '&Uninstall';
    OK.ModalResult := mrOk;
    OK.Default := True;
    Cancel := TNewButton.Create(Form);
    Cancel.Parent := Form;
    Cancel.Caption := 'Cancel';
    Cancel.ModalResult := mrCancel;
    Cancel.Cancel := True;
    W := Form.CalculateButtonWidth([OK.Caption, Cancel.Caption]);
    OK.Width := W;
    Cancel.Width := W;
    OK.Height := ScaleY(23);
    Cancel.Height := ScaleY(23);
    Form.ClientHeight := Y + ScaleY(23 + 24);
    OK.Top := Form.ClientHeight - ScaleY(23 + 12);
    Cancel.Top := OK.Top;
    Cancel.Left := Form.ClientWidth - W - ScaleX(12);
    OK.Left := Cancel.Left - W - ScaleX(6);
    Form.ActiveControl := OK;
    if Form.ShowModal() <> mrOk then begin
      Result := False;
      exit;
    end;
    uKeepSaves := not CSaves.Checked;
    uRemovePython := CPython.Checked;
    uRemoveGame := CGame.Checked;
    uRemoveShaders := CShaders.Checked;
    if CVS <> nil then
      uRemoveVS := CVS.Checked;
  finally
    Form.Free();
  end;
end;

function OtherTitles(const Dir: String): Boolean;
var
  F: TFindRec;
begin
  Result := False;
  if FindFirst(Dir + '\*', F) then
    try
      repeat
        if (F.Name <> '.') and (F.Name <> '..') and (Lowercase(F.Name) <> TITLE_DIR) then begin
          Result := True;
          exit;
        end;
      until not FindNext(F);
    finally
      FindClose(F);
    end;
end;

procedure Gone(const Path: String; IsDir: Boolean);
begin
  if IsDir then begin
    if DirExists(Path) then
      Log(Format('Remove %s: %d', [Path, Ord(DelTree(Path, True, True, True))]));
  end else if FileExists(Path) then
    Log(Format('Remove %s: %d', [Path, Ord(DeleteFile(Path))]));
end;

{ Where the game keeps saves and settings: %LOCALAPPDATA%, or /USERDATAROOT=<dir>
  (testing: an uninstall that must not touch this PC's real saves). }
function UserDataBase(): String;
begin
  if not CmdParam('USERDATAROOT', Result) or (Result = '') then
    Result := ExpandConstant('{localappdata}');
end;

procedure RemoveUserData();
var
  Root: String;
begin
  Root := UserDataBase() + '\xboxrecomp';
  if uRemoveShaders then
    Gone(Root + '\shadercache', True);
  if uKeepSaves then
    exit;
  { Blood Wake's own parts of the toolkit's save folder }
  Gone(Root + '\UDATA\' + TITLE_DIR, True);
  Gone(Root + '\TDATA\' + TITLE_DIR, True);
  Gone(Root + '\editions', True);
  Gone(UserDataBase() + '\BloodWake', True);
  { The rest (the emulated hard disk, caches) is shared by any game built with
    xboxrecomp: removed only when no other game has saves in it. }
  if uRemoveShaders and not OtherTitles(Root + '\UDATA') and not OtherTitles(Root + '\TDATA') then
    Gone(Root, True)
  else
    Log('Kept the shared parts of ' + Root);
end;

procedure RemoveBuildTools();
var
  Path, Inst, Args: String;
  Code: Integer;
begin
  Path := GetIniString('vs', 'path', '', ExpandConstant('{app}\installer.ini'));
  Inst := ExpandConstant('{commonpf32}\Microsoft Visual Studio\Installer\setup.exe');
  if (Path = '') or not FileExists(Inst) then begin
    Log('Build Tools: nothing to remove (' + Path + ')');
    exit;
  end;
  Args := 'uninstall --installPath ' + ArgQuote(Path) + ' --norestart';
  if UninstallSilent() then Args := Args + ' --quiet' else Args := Args + ' --passive';
  if HasParam('VSPLANONLY') then begin
    Log('Build Tools: plan only, would run: ' + Inst + ' ' + Args);
    exit;
  end;
  Log('Build Tools: ' + Inst + ' ' + Args);
  if ShellExec('', Inst, Args, '', SW_SHOWNORMAL, ewWaitUntilTerminated, Code) then
    Log(Format('Build Tools uninstall exit code %d', [Code]))
  else
    Log('Build Tools uninstall did not start: ' + SysErrorMessage(Code));
end;

procedure CurUninstallStepChanged(CurUninstallStep: TUninstallStep);
var
  App: String;
begin
  if CurUninstallStep = usUninstall then begin
    App := ExpandConstant('{app}');
    if uRemoveVS then
      RemoveBuildTools();
    { What the build wrote (Inno's log has only what Setup itself copied) }
    Gone(App + '\build', True);
    Gone(App + '\work', True);
    Gone(App + '\tools', True);
    Gone(App + '\Blood Wake.cmd', False);
    Gone(App + '\play.cmd', False);
    Gone(App + '\install.json', False);
    Gone(App + '\deps', True);
    Gone(App + '\setup', True);
    if uRemoveGame then
      Gone(App + '\game', True);
    if uRemovePython then
      Gone(App + '\python', True);
    RemoveUserData();
    Gone(App + '\installer.ini', False);
  end else if CurUninstallStep = usPostUninstall then begin
    App := ExpandConstant('{app}');
    { Kept on purpose (/KEEPPYTHON, /KEEPGAME or the boxes): say so in the folder,
      so the next Setup knows it's ours and installs into it again (item 11). }
    if DirExists(App + '\python') or DirExists(App + '\game') then begin
      SaveStringToFile(App + '\kept_by_uninstall.txt',
        'The Blood Wake Recomp uninstaller kept these here on purpose: ' +
        'the private Python (python) and/or the game''s files from your disc (game).' + #13#10 +
        'Setup can install into this folder again and reuses them. If you don''t need them, delete this folder.' + #13#10, False);
      Log('Kept on purpose; marker written: ' + App + '\kept_by_uninstall.txt');
    end;
    RemoveDir(App);
  end;
end;
