@echo off
rem Downloads the latest ArrayLab build and puts it in the system VST3 folder.
rem Double-click it. Close Pyramix first: Windows locks a plugin that is in use.
setlocal
set "DEST=%~1"
if not "%DEST%"=="" goto install

rem Writing to Program Files needs administrator rights: ask for them once.
net session >nul 2>&1
if errorlevel 1 (
    powershell -NoProfile -Command "Start-Process -FilePath '%~f0' -Verb RunAs"
    exit /b
)
set "DEST=%CommonProgramFiles%\VST3"

:install
powershell -NoProfile -ExecutionPolicy Bypass -Command "$ErrorActionPreference = 'Stop'; $ProgressPreference = 'SilentlyContinue'; $zip = Join-Path $env:TEMP 'ArrayLab-VST3-Windows.zip'; $dest = $env:DEST; New-Item -ItemType Directory -Force -Path $dest | Out-Null; Write-Host 'Downloading the latest ArrayLab...'; Invoke-WebRequest 'https://github.com/oreadesrec-gif/arraylab-tech/releases/download/latest/ArrayLab-VST3-Windows.zip' -OutFile $zip; foreach ($old in 'ArrayLab.vst3', 'ArrayLab Probe.vst3') { $p = Join-Path $dest $old; if (Test-Path $p) { Remove-Item $p -Recurse -Force } }; Expand-Archive $zip -DestinationPath $dest -Force; Remove-Item $zip; Write-Host ('ArrayLab installed in ' + $dest)"
set "RESULT=%errorlevel%"
if not "%RESULT%"=="0" echo Installation failed. Close Pyramix and try again.
if "%~1"=="" pause
exit /b %RESULT%
