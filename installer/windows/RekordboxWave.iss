; Rekordbox Wave for Windows: installs for the signed-in user (no administrator needed), with Start menu and
; desktop shortcuts and an uninstaller. Rekordbox itself is AlphaTheta's: when the version Wave supports is
; missing, Wave downloads AlphaTheta's own installer on first start, checks its signature and runs it.
; Built by CI: iscc /DAppVersion=x.y.z installer\windows\RekordboxWave.iss, with the C++ app in dist\.

#ifndef AppVersion
  #define AppVersion "0.0.0"
#endif
#define RekordboxVersion "7.2.19"
; AlphaTheta's own installer for it, hosted on this project's pre-release (see the mirror-rekordbox CI job) and
; checked here against its SHA-256 before it runs. It is signed by AlphaTheta Corporation.
#define RekordboxInstaller "Install_rekordbox_x64_7_2_19.exe"
#define RekordboxUrl "https://github.com/palermostest25/rkbx_wave_den/releases/download/rekordbox-7.2.19/Install_rekordbox_x64_7_2_19.exe"
#define RekordboxSha256 "290bfaa2bbab6d4b650d9e8982b5a8eeaaa86ad168eb8dc3f3d506c11310f862"

[Setup]
AppId={{7E3B5C2A-9F1D-4C8E-A6B2-3D5F7A9C1E40}
AppName=Rekordbox Wave
AppVersion={#AppVersion}
AppVerName=Rekordbox Wave {#AppVersion}
AppPublisher=Rekordbox Wave contributors
AppPublisherURL=https://github.com/palermostest25/rkbx_wave_den
AppSupportURL=https://github.com/palermostest25/rkbx_wave_den/issues
AppUpdatesURL=https://github.com/palermostest25/rkbx_wave_den/releases
DefaultDirName={localappdata}\Programs\Rekordbox Wave
DefaultGroupName=Rekordbox Wave
DisableProgramGroupPage=yes
DisableDirPage=auto
PrivilegesRequired=lowest
ArchitecturesAllowed=x64compatible
ArchitecturesInstallIn64BitMode=x64compatible
MinVersion=10.0
OutputDir=..\..\dist
OutputBaseFilename=Rekordbox-Wave-Setup-Windows-x64
SetupIconFile=..\..\rb_waveform_core\data\icon.ico
UninstallDisplayIcon={app}\Rekordbox Wave.exe
UninstallDisplayName=Rekordbox Wave
WizardStyle=modern
Compression=lzma2/max
SolidCompression=yes
CloseApplications=force
RestartApplications=no
VersionInfoVersion={#AppVersion}
VersionInfoProductName=Rekordbox Wave

[Messages]
WelcomeLabel2=This installs Rekordbox Wave {#AppVersion}, the XDJ-AZ style deck screen and touch browser for Rekordbox.%n%nWave works with Rekordbox {#RekordboxVersion}. If it isn't on this computer, this installer downloads AlphaTheta's own Rekordbox {#RekordboxVersion} installer (about 660 MB), checks it and runs it for you.

[Tasks]
Name: "desktopicon"; Description: "Put Rekordbox Wave on the desktop"
Name: "rekordbox"; Description: "Download and install Rekordbox {#RekordboxVersion} (needed; about 660 MB; Windows asks for permission)"; Check: NeedsRekordbox
Name: "startup"; Description: "Start Rekordbox Wave when I sign in"; Flags: unchecked

[Files]
Source: "..\..\dist\Rekordbox Wave.exe"; DestDir: "{app}"; Flags: ignoreversion

[Icons]
Name: "{autoprograms}\Rekordbox Wave"; Filename: "{app}\Rekordbox Wave.exe"
Name: "{autodesktop}\Rekordbox Wave"; Filename: "{app}\Rekordbox Wave.exe"; Tasks: desktopicon
Name: "{userstartup}\Rekordbox Wave"; Filename: "{app}\Rekordbox Wave.exe"; Tasks: startup

[Run]
; Wave offers Rekordbox setup itself if Rekordbox is still missing (a download that failed, an installer cancelled).
Filename: "{app}\Rekordbox Wave.exe"; Parameters: "--setup"; Description: "Start Rekordbox Wave"; Flags: nowait postinstall skipifsilent

[UninstallDelete]
; What an update leaves beside Wave.
Type: files; Name: "{app}\Rekordbox Wave.old.exe"

[Code]
var
  DownloadPage: TDownloadWizardPage;
  RekordboxReady: Boolean;

function NeedsRekordbox(): Boolean;
begin
  Result := not FileExists(ExpandConstant('{commonpf64}\rekordbox\rekordbox {#RekordboxVersion}\rekordbox.exe'));
end;

procedure InitializeWizard();
begin
  DownloadPage := CreateDownloadPage('Downloading Rekordbox {#RekordboxVersion}',
    'AlphaTheta''s own installer, checked before it runs.', nil);
end;

// The download happens before anything is installed, so the DJ sees its progress and can skip it.
function NextButtonClick(CurPageID: Integer): Boolean;
begin
  Result := True;
  if (CurPageID = wpReady) and WizardIsTaskSelected('rekordbox') then begin
    DownloadPage.Clear;
    DownloadPage.Add('{#RekordboxUrl}', '{#RekordboxInstaller}', '{#RekordboxSha256}');
    DownloadPage.Show;
    try
      try
        DownloadPage.Download;
        RekordboxReady := True;
      except
        if not DownloadPage.AbortedByUser then
          SuppressibleMsgBox('Rekordbox {#RekordboxVersion} could not be downloaded: ' + GetExceptionMessage +
            #13#10#13#10'Wave installs anyway, and offers Rekordbox again when it starts.', mbInformation, MB_OK, IDOK);
      end;
    finally
      DownloadPage.Hide;
    end;
  end;
end;

// Wave is in place: now AlphaTheta's installer, with Windows' permission (Rekordbox installs for every user).
procedure CurStepChanged(CurStep: TSetupStep);
var
  ResultCode: Integer;
begin
  if (CurStep = ssPostInstall) and RekordboxReady then begin
    WizardForm.StatusLabel.Caption := 'Installing Rekordbox {#RekordboxVersion}: answer its installer...';
    if not ShellExec('runas', ExpandConstant('{tmp}\{#RekordboxInstaller}'), '', ExpandConstant('{tmp}'), SW_SHOWNORMAL,
                     ewWaitUntilTerminated, ResultCode) then
      SuppressibleMsgBox('Rekordbox''s installer did not start (permission refused?). Wave offers it again when it starts.',
        mbInformation, MB_OK, IDOK);
  end;
end;
