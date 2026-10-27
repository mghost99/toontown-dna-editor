; Inno Setup script for the DNA Editor (Windows).  Built by installer\build_installer.ps1.
#ifndef AppVersion
  #define AppVersion "0.0.3"
#endif
#ifndef StageDir
  #define StageDir "..\build-win\stage"
#endif

[Setup]
AppId={{3E9A41F7-58C2-4D0B-A6E3-9B27D4C81F05}
AppName=DNA Editor
AppVersion={#AppVersion}
AppPublisher=mghost99
DefaultDirName={autopf}\DNA Editor
DefaultGroupName=DNA Editor
PrivilegesRequiredOverridesAllowed=dialog commandline
OutputDir=..\build-win
OutputBaseFilename=DNA-Editor-Setup-v{#AppVersion}
SetupIconFile=..\assets\branding\dna-editor.ico
WizardImageFile=..\assets\branding\installer_side.bmp
WizardSmallImageFile=..\assets\branding\installer_small.bmp
UninstallDisplayIcon={app}\bin\dna-editor.exe
Compression=lzma2/max
SolidCompression=yes
ArchitecturesAllowed=x64compatible
ArchitecturesInstallIn64BitMode=x64compatible
WizardStyle=modern

[Tasks]
Name: "desktopicon"; Description: "Create a &desktop shortcut"; GroupDescription: "Shortcuts:"

[Files]
Source: "{#StageDir}\*"; DestDir: "{app}"; Flags: recursesubdirs ignoreversion

[Icons]
Name: "{group}\DNA Editor"; Filename: "{app}\bin\dna-editor.exe"; WorkingDir: "{app}\bin"; IconFilename: "{app}\bin\dna-editor.ico"
Name: "{group}\Uninstall DNA Editor"; Filename: "{uninstallexe}"
Name: "{autodesktop}\DNA Editor"; Filename: "{app}\bin\dna-editor.exe"; WorkingDir: "{app}\bin"; IconFilename: "{app}\bin\dna-editor.ico"; Tasks: desktopicon

[Run]
Filename: "{app}\bin\dna-editor.exe"; Description: "Launch DNA Editor"; Flags: nowait postinstall skipifsilent

[Code]
var ResPage: TInputDirWizardPage;

function IsRes(const P: String): Boolean;
begin
  Result := (P <> '') and (DirExists(AddBackslash(P) + 'phase_4') or DirExists(AddBackslash(P) + 'phase_8'));
end;

function ExistingConfigRes(): String;
var Lines: TArrayOfString; I: Integer; F: String;
begin
  Result := '';
  F := ExpandConstant('{userappdata}\DnaEditor\config.ini');
  if LoadStringsFromFile(F, Lines) then
    for I := 0 to GetArrayLength(Lines) - 1 do
      if Pos('resources=', Lines[I]) = 1 then Result := Copy(Lines[I], 11, Length(Lines[I]));
end;

function DetectRes(): String;
var C: array of String; I: Integer;
begin
  Result := ExistingConfigRes();
  if IsRes(Result) then exit;
  SetArrayLength(C, 6);
  Result := '';
  for I := 0 to GetArrayLength(C) - 1 do
    if IsRes(C[I]) then begin Result := C[I]; exit; end;
end;

procedure InitializeWizard();
begin
  ResPage := CreateInputDirPage(wpSelectDir, 'Game resources folder',
    'Where is your game''s resources folder?',
    'The editor loads models, textures and DNA files from the game''s "resources" folder (it contains phase_4, phase_8, ...).' + #13#10 +
    'It was auto-detected if found; pick another folder with Browse. You can change it later in File > Set Resources Folder...',
    False, '');
  ResPage.Add('Resources folder:');
  ResPage.Values[0] := ExpandConstant('{param:RES|' + DetectRes() + '}');
end;

function NextButtonClick(CurPageID: Integer): Boolean;
begin
  Result := True;
  if (CurPageID = ResPage.ID) and not IsRes(ResPage.Values[0]) then
    Result := MsgBox('That folder has no phase_4 or phase_8 inside. Use it anyway?', mbConfirmation, MB_YESNO) = IDYES;
end;

procedure CurStepChanged(CurStep: TSetupStep);
var Dir: String;
begin
  if CurStep = ssPostInstall then begin
    Dir := ExpandConstant('{userappdata}\DnaEditor');
    ForceDirectories(Dir);
    SaveStringToFile(Dir + '\config.ini', '[editor]' + #13#10 + 'resources=' + ResPage.Values[0] + #13#10, False);
  end;
end;
