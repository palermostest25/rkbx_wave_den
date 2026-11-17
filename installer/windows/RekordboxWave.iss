; Rekordbox Wave for Windows: installs for the signed-in user (no administrator needed), with Start menu and
; desktop shortcuts and an uninstaller. Rekordbox itself is AlphaTheta's: when the version Wave supports is
; missing, Wave downloads AlphaTheta's own installer on first start, checks its signature and runs it.
; Built by CI: iscc /DAppVersion=x.y.z installer\windows\RekordboxWave.iss (after scripts/build_app.py).

#ifndef AppVersion
  #define AppVersion "0.0.0"
#endif
#define RekordboxVersion "7.2.19"

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
WelcomeLabel2=This installs Rekordbox Wave {#AppVersion}, the XDJ-AZ style deck screen and touch browser for Rekordbox.%n%nWave works with Rekordbox {#RekordboxVersion}. If it isn't on this computer, Wave can download AlphaTheta's own installer and install it for you when it first starts.

[Tasks]
Name: "desktopicon"; Description: "Put Rekordbox Wave on the desktop"
Name: "rekordbox"; Description: "Install Rekordbox {#RekordboxVersion} from rekordbox.com when Wave starts (needed; Windows asks for permission)"; Check: NeedsRekordbox
Name: "startup"; Description: "Start Rekordbox Wave when I sign in"; Flags: unchecked

[Files]
Source: "..\..\dist\Rekordbox Wave.exe"; DestDir: "{app}"; Flags: ignoreversion

[Icons]
Name: "{autoprograms}\Rekordbox Wave"; Filename: "{app}\Rekordbox Wave.exe"
Name: "{autodesktop}\Rekordbox Wave"; Filename: "{app}\Rekordbox Wave.exe"; Tasks: desktopicon
Name: "{userstartup}\Rekordbox Wave"; Filename: "{app}\Rekordbox Wave.exe"; Tasks: startup

[Run]
Filename: "{app}\Rekordbox Wave.exe"; Parameters: "--install-rekordbox"; Description: "Start Rekordbox Wave"; Flags: nowait postinstall skipifsilent; Tasks: rekordbox
Filename: "{app}\Rekordbox Wave.exe"; Parameters: "--setup"; Description: "Start Rekordbox Wave"; Flags: nowait postinstall skipifsilent; Tasks: not rekordbox

[UninstallDelete]
; What an update leaves beside Wave.
Type: files; Name: "{app}\Rekordbox Wave.old.exe"

[Code]
function NeedsRekordbox(): Boolean;
begin
  Result := not FileExists(ExpandConstant('{commonpf64}\rekordbox\rekordbox {#RekordboxVersion}\rekordbox.exe'));
end;
