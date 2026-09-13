; ============================================================================
;  PerformLive.iss -- the PerformLive beta installer (Inno Setup 6.7).
;
;  Built by package_release.ps1, which passes every value that changes per
;  build: /DAppVersion /DStageDir /DIconFile /DWizardImage /DWizardSmall
;  /DLastDay /DLicenceFile. Written against the studio's Installer & Packaging
;  Standard; the P-numbers below are its requirements.
;
;  Unsigned: this is a test build (P31 is deliberately open for the beta).
; ============================================================================
#ifndef AppVersion
  #error Build this with package_release.ps1, which supplies the version and paths
#endif

#define AppName   "PerformLive"
#define AppLabel  "PERFORMLIVE BETA (Testing)"
#define Publisher "Amanorsac Studio"

[Setup]
; Windows recognises later builds as the same app by this id. Never change it.
AppId={{6E1B2C7A-4F2D-4B8E-9A61-5C3D7E0F9A42}
AppName={#AppName}
AppVersion={#AppVersion}
AppVerName={#AppLabel} {#AppVersion}
AppPublisher={#Publisher}
AppPublisherURL=https://amanorsac.studio
AppSupportURL=https://amanorsac.studio
VersionInfoCompany={#Publisher}
VersionInfoProductName={#AppName}
VersionInfoVersion={#AppVersion}
VersionInfoDescription={#AppLabel} installer

; P13: the application folder is changeable.
; P22: Program Files needs an administrator. "Install for me only" needs none,
; and Inno's own dialog asks which before anything else happens.
DefaultDirName={autopf}\Amanorsac Studio\{#AppName}
DisableDirPage=no
DisableProgramGroupPage=yes
DisableWelcomePage=no
PrivilegesRequired=admin
PrivilegesRequiredOverridesAllowed=dialog
UsePreviousAppDir=yes

; P17-P19: near-black ground, the lockup on the first page, the app's own icon.
WizardStyle=modern dark
WizardBackColor=#07070F
WizardImageFile={#WizardImage}
WizardSmallImageFile={#WizardSmall}
SetupIconFile={#IconFile}
UninstallDisplayIcon={app}\PerformLive.exe
UninstallDisplayName={#AppName}

; P20: the licence page.
LicenseFile={#LicenceFile}

; P1, P28: one file, exact name.
OutputBaseFilename={#AppName}-{#AppVersion}-Windows
Compression=lzma2/max
SolidCompression=yes
ArchitecturesAllowed=x64compatible
ArchitecturesInstallIn64BitMode=x64compatible
MinVersion=10.0

[Languages]
Name: "english"; MessagesFile: "compiler:Default.isl"

[Messages]
; British English, matching the studio's documents (B59).
WizardLicense=Licence Agreement
LicenseLabel=Please read the following licence before continuing.
LicenseLabel3=Please read the following licence. You must accept it before continuing with the installation.
LicenseAccepted=I &accept the licence agreement
LicenseNotAccepted=I &do not accept the licence agreement
WelcomeLabel1=Welcome to {#AppLabel}
WelcomeLabel2=This installs {#AppName} {#AppVersion} from {#Publisher}: a stage instrument for running stems, loops, pads and a click live.%n%nThis is a free test build. There is no licence key and no account, and it never connects to the internet. You can use it until {#LastDay}.%n%nNo sounds are included. The app opens empty.

[Types]
Name: "full"; Description: "{#AppName}"; Flags: iscustom

[Components]
; P5, P12: a standalone ships a standalone, and nothing else.
Name: "app"; Description: "PerformLive application (standalone)"; Types: full; Flags: fixed

[Tasks]
Name: "desktopicon"; Description: "Create a desktop shortcut"; Flags: unchecked

[Files]
Source: "{#StageDir}\PerformLive.exe"; DestDir: "{app}";       Components: app; Flags: ignoreversion
Source: "{#StageDir}\logo.png";        DestDir: "{app}";       Components: app; Flags: ignoreversion
Source: "{#StageDir}\README.txt";      DestDir: "{app}";       Components: app; Flags: ignoreversion
Source: "{#StageDir}\fonts\*";         DestDir: "{app}\fonts"; Components: app; Flags: ignoreversion
Source: "{#StageDir}\art\*";           DestDir: "{app}\art";   Components: app; Flags: ignoreversion

[Icons]
Name: "{autoprograms}\{#AppName}"; Filename: "{app}\PerformLive.exe"
Name: "{autodesktop}\{#AppName}";  Filename: "{app}\PerformLive.exe"; Tasks: desktopicon

[Run]
Filename: "{app}\PerformLive.exe"; Description: "Open {#AppName} now"; Flags: nowait postinstall skipifsilent

[Code]
{ The two folders the app itself writes to (File & Data Conventions 1.1, 1.2).
  The installer creates neither and uninstalling removes neither (P25). }
function UserContentDir: String;
begin
  Result := ExpandConstant('{userdocs}\Amanorsac Studio\PerformLive');
end;

function MachineStateDir: String;
begin
  Result := ExpandConstant('{localappdata}\Amanorsac Studio\PerformLive');
end;

{ P11: every full path, readable, before anything is written. }
function UpdateReadyMemo(Space, NewLine, MemoUserInfoInfo, MemoDirInfo, MemoTypeInfo,
  MemoComponentsInfo, MemoGroupInfo, MemoTasksInfo: String): String;
begin
  Result :=
    'PerformLive application' + NewLine +
    Space + ExpandConstant('{app}') + NewLine + NewLine +
    'Your projects and library (created when you first open the app; kept if you uninstall)' + NewLine +
    Space + UserContentDir + NewLine + NewLine +
    'Settings and logs (created when you first open the app; kept if you uninstall)' + NewLine +
    Space + MachineStateDir;
  if MemoTasksInfo <> '' then
    Result := Result + NewLine + NewLine + MemoTasksInfo;
end;

var
  InstalledPaths: TNewMemo;

{ P15: the last page lists what went where, in a box the paths can be copied from. }
procedure CurPageChanged(CurPageID: Integer);
begin
  if CurPageID <> wpFinished then
    exit;

  WizardForm.FinishedLabel.Caption :=
    'PerformLive is installed. You can use this beta until {#LastDay}.' + #13#10 + #13#10 +
    'It opens empty, so bring your own stems, loops and pads. To sell your own, open the STORE page in the app.';
  WizardForm.AdjustLabelHeight(WizardForm.FinishedLabel);

  if InstalledPaths = nil then
  begin
    InstalledPaths := TNewMemo.Create(WizardForm);
    InstalledPaths.Parent := WizardForm.FinishedPage;
    InstalledPaths.ReadOnly := True;
    InstalledPaths.ScrollBars := ssVertical;
    InstalledPaths.WordWrap := False;
  end;

  InstalledPaths.Left   := WizardForm.FinishedLabel.Left;
  InstalledPaths.Width  := WizardForm.FinishedLabel.Width;
  InstalledPaths.Top    := WizardForm.FinishedLabel.Top + WizardForm.FinishedLabel.Height + ScaleY(12);
  InstalledPaths.Height := ScaleY(64);
  InstalledPaths.Lines.Text :=
    'Application:  ' + ExpandConstant('{app}') + #13#10 +
    'Projects:     ' + UserContentDir + #13#10 +
    'Settings:     ' + MachineStateDir;

  WizardForm.RunList.Top := InstalledPaths.Top + InstalledPaths.Height + ScaleY(12);
end;
