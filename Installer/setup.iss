; 机械革命监控（MechrevoMonitorTray）安装包脚本 — Inno Setup 6.5+
; 编译：ISCC.exe setup.iss

#define MyAppName "机械革命监控"
#define MyAppNameEn "MechrevoMonitorTray"
#define MyAppVersion "3.2.4"
#define MyAppExeName "MechrevoMonitorTray.exe"
#define MyAppPublisher "MechrevoMonitorTray"

[Setup]
AppId={{7A4B2C8E-1D3F-4E6A-B5C8-90D1E2F3A4B6}
AppName={#MyAppName}
AppVersion={#MyAppVersion}
AppVerName={#MyAppName} {#MyAppVersion}
AppPublisher={#MyAppPublisher}
DefaultDirName={autopf}\{#MyAppNameEn}
DefaultGroupName={#MyAppName}
UninstallDisplayName={#MyAppName} {#MyAppVersion}
OutputDir=Output
OutputBaseFilename=MechrevoMonitorTray-Setup-{#MyAppVersion}
Compression=lzma2/max
SolidCompression=yes
WizardStyle=modern
ArchitecturesInstallIn64BitMode=x64compatible
PrivilegesRequired=admin
SetupIconFile=..\MonitoringApp.Tray\app.ico
UninstallDisplayIcon={app}\{#MyAppExeName}
CloseApplications=no
ChangesAssociations=no

[Languages]
Name: "chinesesimplified"; MessagesFile: "compiler:Languages\ChineseSimplified.isl"

[Tasks]
Name: "desktopicon"; Description: "创建桌面快捷方式(&D)"; GroupDescription: "附加任务："; Flags: unchecked
Name: "autostart"; Description: "开机自动运行(&S)"; GroupDescription: "附加任务："

[Files]
Source: "..\MonitoringApp.Tray\bin\Release\net8.0-windows\win-x64\publish\MechrevoMonitorTray.exe"; DestDir: "{app}"; Flags: ignoreversion
; 说明文档取仓库内 README（CI checkout 里没有仓库外的使用说明.txt），安装后显示为 使用说明.txt
Source: "..\README.md"; DestDir: "{app}"; DestName: "使用说明.txt"; Flags: ignoreversion

[Icons]
Name: "{group}\{#MyAppName}"; Filename: "{app}\{#MyAppExeName}"; Comment: "CPU 功耗 / 风扇转速托盘监控"
Name: "{group}\卸载 {#MyAppName}"; Filename: "{uninstallexe}"
Name: "{autodesktop}\{#MyAppName}"; Filename: "{app}\{#MyAppExeName}"; Tasks: desktopicon

[Run]
; 开机自启用计划任务（最高权限）而非 HKCU Run：程序 requireAdministrator，
; HKCU Run 登录自启是非提权运行，读不到 MSR，功耗会一直显示 "--"
Filename: "schtasks"; Parameters: "/Create /TN ""MechrevoMonitorTray"" /TR ""\""{app}\{#MyAppExeName}""\"""" /SC ONLOGON /RL HIGHEST /F"; Tasks: autostart; Flags: runhidden
Filename: "{app}\{#MyAppExeName}"; Description: "立即运行 {#MyAppName}"; Flags: nowait postinstall skipifsilent

[UninstallRun]
Filename: "{cmd}"; Parameters: "/C taskkill /f /im {#MyAppExeName} >nul 2>&1"; Flags: runhidden; RunOnceId: "KillApp"
Filename: "schtasks"; Parameters: "/Delete /TN ""MechrevoMonitorTray"" /F"; Flags: runhidden; RunOnceId: "DelTask"
; 清理 3.1.0 及更早版本的 HKCU Run 自启项
Filename: "{cmd}"; Parameters: "/C reg delete ""HKCU\Software\Microsoft\Windows\CurrentVersion\Run"" /v MechrevoMonitorTray /f >nul 2>&1"; Flags: runhidden; RunOnceId: "DelLegacyRun"

[Code]
// 安装前停掉正在运行的监控进程（最多重试 5 次），避免文件占用导致安装失败
function PrepareToInstall(var NeedsRestart: Boolean): String;
var
  ResultCode: Integer;
  I: Integer;
begin
  Result := '';
  for I := 1 to 5 do
  begin
    Exec(ExpandConstant('{cmd}'), '/C taskkill /f /im {#MyAppExeName} >nul 2>&1',
         '', SW_HIDE, ewWaitUntilTerminated, ResultCode);
    Sleep(600);
    // tasklist|find：找到进程返回 0，没找到返回 1
    Exec(ExpandConstant('{cmd}'),
         '/C tasklist /fi "IMAGENAME eq {#MyAppExeName}" | find /i "{#MyAppExeName}" >nul 2>&1',
         '', SW_HIDE, ewWaitUntilTerminated, ResultCode);
    if ResultCode = 1 then
      Exit;   // 进程已退出，可以继续安装
  end;
  Result := 'MechrevoMonitorTray.exe 正在运行且无法结束，请手动退出后重试。';
end;
