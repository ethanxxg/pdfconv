; ---------------------------------------------------------------------------
; DocForge 文档工坊 -- Inno Setup 安装包脚本
;
; 前置条件: 先跑 app\build_exe_onedir.bat 生成 dist\DocForge\
; 编译:     "C:\Program Files (x86)\Inno Setup 6\ISCC.exe" app\DocForge.iss
;           (或直接双击 app\build_installer.bat，会自动找 / 装 Inno Setup)
; 产物:     dist\installer\DocForge-1.0.7-setup.exe
;
; 安装方式: PrivilegesRequired=lowest -> 默认按当前用户装到
;           %LOCALAPPDATA%\Programs\DocForge，全程无 UAC 弹窗；
;           用户在命令行加 /ALLUSERS 仍可装到 Program Files。
; ---------------------------------------------------------------------------

#define AppName      "DocForge 文档工坊"
#define AppShortName "DocForge"
#define AppVersion   "1.0.8"
#define AppPublisher "DocForge"
#define AppExeName   "DocForge.exe"
#define SrcDir       "..\dist\DocForge"

[Setup]
; AppId 必须永远不变，否则新版本会被当成另一个软件重复安装
AppId={{BD773EEB-A038-4D3A-A9FA-13FBD17FD0A4}
AppName={#AppName}
AppVersion={#AppVersion}
AppVerName={#AppName} {#AppVersion}
AppPublisher={#AppPublisher}
VersionInfoVersion={#AppVersion}
VersionInfoCompany={#AppPublisher}
VersionInfoDescription={#AppName} Setup
VersionInfoProductName={#AppName}
VersionInfoProductVersion={#AppVersion}

DefaultDirName={autopf}\{#AppShortName}
DefaultGroupName={#AppName}
DisableProgramGroupPage=yes
DisableDirPage=no

; 图标 / 卸载项
SetupIconFile=DocForge.ico
UninstallDisplayIcon={app}\{#AppExeName}
UninstallDisplayName={#AppName}

; 输出
OutputDir=..\dist\installer
OutputBaseFilename=DocForge-{#AppVersion}-setup

; 压缩
Compression=lzma2/max
SolidCompression=yes
LZMANumBlockThreads=4

; 外观
WizardStyle=modern
WizardSizePercent=110

; 权限：默认按用户安装（不弹 UAC），允许用 /ALLUSERS 覆盖
PrivilegesRequired=lowest
PrivilegesRequiredOverridesAllowed=commandline dialog

; 平台：PyQt6 / PyMuPDF 只提供 x64 轮子，Win10 以下不支持
ArchitecturesAllowed=x64compatible
ArchitecturesInstallIn64BitMode=x64compatible
MinVersion=10.0

; 覆盖安装时若 DocForge 正在运行，提示用户关闭（而不是报文件占用）
CloseApplications=yes
RestartApplications=no
AllowNoIcons=yes
SetupLogging=yes

[Languages]
; 第一个为默认语言
Name: "cn"; MessagesFile: "ChineseSimplified.isl"
Name: "en"; MessagesFile: "compiler:Default.isl"

[Tasks]
Name: "desktopicon"; Description: "{cm:CreateDesktopIcon}"; GroupDescription: "{cm:AdditionalIcons}"; Flags: unchecked

[Files]
; 整个 onedir 目录（DocForge.exe + _internal\）原样铺进安装目录
Source: "{#SrcDir}\*"; DestDir: "{app}"; Flags: ignoreversion recursesubdirs createallsubdirs

[Icons]
Name: "{group}\{#AppName}"; Filename: "{app}\{#AppExeName}"
Name: "{group}\{cm:UninstallProgram,{#AppName}}"; Filename: "{uninstallexe}"
Name: "{autodesktop}\{#AppName}"; Filename: "{app}\{#AppExeName}"; Tasks: desktopicon

[Run]
Filename: "{app}\{#AppExeName}"; Description: "{cm:LaunchProgram,{#StringChange(AppName, '&', '&&')}}"; Flags: nowait postinstall skipifsilent

[UninstallDelete]
; 卸载时清掉运行期可能残留的日志/缓存（不影响用户自己的输出文件）
Type: files; Name: "{app}\*.log"

; 注意: 这里刻意不做"源文件是否存在"的 [Code] 检查。
; 一是相对路径 ..\dist\DocForge 在运行时无法解析；二是用 {src} 会展开成
; 编译机的绝对路径，安装包拿到别人机器上必然 FileExists=False 而拒绝安装。
; 该检查也多余: [Files] 的 Source 匹配不到任何文件时 ISCC 编译期就报
; "No files found matching ..." 并失败。
