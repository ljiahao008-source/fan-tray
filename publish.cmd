@echo off
setlocal
REM 一键发布 MechrevoMonitorTray 单文件 exe（需 .NET 8 SDK）。
REM 可选代码签名：对外分发前建议购买 OV/EV 代码签名证书，然后设置以下环境变量再运行本脚本：
REM   set SIGN_PFX=C:\path\to\cert.pfx
REM   set SIGN_PFX_PWD=证书密码
REM   set SIGN_TIMESTAMP=http://timestamp.digicert.com
REM 个人自用可自签（无第三方信任，仅提供"文件未被篡改"的校验依据）：
REM   powershell -c "New-SelfSignedCertificate -Type CodeSigningCert -Subject 'CN=MechrevoMonitor' -CertStoreLocation Cert:\CurrentUser\My"
REM   （取其指纹后）set SIGN_PFX=Cert:\CurrentUser\My^<指纹^> 不被 signtool 直接支持，自签请用 /sha1 指纹方式，见下方 SIGN_SHA1
REM   set SIGN_SHA1=证书指纹（去掉空格）

dotnet publish MonitoringApp.Tray\MonitoringApp.Tray.csproj -c Release -r win-x64 -p:PublishSingleFile=true --self-contained true
if errorlevel 1 exit /b 1

set "OUT=%~dp0MonitoringApp.Tray\bin\Release\net8.0-windows\win-x64\publish\MechrevoMonitorTray.exe"

if "%SIGN_PFX%"=="" if "%SIGN_SHA1%"=="" (
  echo [提示] 未设置 SIGN_PFX / SIGN_SHA1，跳过签名。对外分发前请签名 exe，否则接收方无法验证文件未被篡改。
  exit /b 0
)

if "%SIGN_TIMESTAMP%"=="" set "SIGN_TIMESTAMP=http://timestamp.digicert.com"

if not "%SIGN_SHA1%"=="" (
  signtool sign /fd SHA256 /sha1 "%SIGN_SHA1%" /tr "%SIGN_TIMESTAMP%" /td SHA256 "%OUT%" || exit /b 1
) else (
  signtool sign /fd SHA256 /f "%SIGN_PFX%" /p "%SIGN_PFX_PWD%" /tr "%SIGN_TIMESTAMP%" /td SHA256 "%OUT%" || exit /b 1
)

signtool verify /pa /v "%OUT%"
echo [完成] 已签名：%OUT%
