Unicode true

!include "LogicLib.nsh"
!include "WinMessages.nsh"

!ifndef PAK_EXE
!define PAK_EXE "pak.exe"
!endif

!ifndef OUT_FILE
!define OUT_FILE "pak-windows-setup.exe"
!endif

!ifndef PRODUCT_VERSION
!define PRODUCT_VERSION "dev"
!endif

!ifndef ARCH
!define ARCH "windows"
!endif

Name "pak"
OutFile "${OUT_FILE}"
InstallDir "$LOCALAPPDATA\Programs\pak"
RequestExecutionLevel user
SetCompressor /SOLID lzma

VIProductVersion "0.0.0.0"
VIAddVersionKey "ProductName" "pak"
VIAddVersionKey "CompanyName" "xaviersupreme"
VIAddVersionKey "FileDescription" "pak ${ARCH} installer"
VIAddVersionKey "FileVersion" "${PRODUCT_VERSION}"
VIAddVersionKey "ProductVersion" "${PRODUCT_VERSION}"

Page directory
Page instfiles

UninstPage uninstConfirm
UninstPage instfiles

Function AddToUserPath
    InitPluginsDir
    FileOpen $0 "$PLUGINSDIR\pak_path.ps1" w
    FileWrite $0 "$$dir = [IO.Path]::GetFullPath('$INSTDIR')$\r$\n"
    FileWrite $0 "$$path = [Environment]::GetEnvironmentVariable('Path', 'User')$\r$\n"
    FileWrite $0 "$$parts = @()$\r$\n"
    FileWrite $0 "if ($$path) { $$parts = $$path -split ';' | Where-Object { $$_ -ne '' } }$\r$\n"
    FileWrite $0 "if ($$parts -notcontains $$dir) { $$parts += $$dir; [Environment]::SetEnvironmentVariable('Path', ($$parts -join ';'), 'User') }$\r$\n"
    FileClose $0
    ExecWait 'powershell.exe -NoProfile -ExecutionPolicy Bypass -File "$PLUGINSDIR\pak_path.ps1"'
    SendMessage ${HWND_BROADCAST} ${WM_SETTINGCHANGE} 0 "STR:Environment" /TIMEOUT=5000
FunctionEnd

Function un.RemoveFromUserPath
    InitPluginsDir
    FileOpen $0 "$PLUGINSDIR\pak_path.ps1" w
    FileWrite $0 "$$dir = [IO.Path]::GetFullPath('$INSTDIR')$\r$\n"
    FileWrite $0 "$$path = [Environment]::GetEnvironmentVariable('Path', 'User')$\r$\n"
    FileWrite $0 "if ($$path) { $$parts = $$path -split ';' | Where-Object { $$_ -ne '' -and $$_ -ne $$dir }; [Environment]::SetEnvironmentVariable('Path', ($$parts -join ';'), 'User') }$\r$\n"
    FileClose $0
    ExecWait 'powershell.exe -NoProfile -ExecutionPolicy Bypass -File "$PLUGINSDIR\pak_path.ps1"'
    SendMessage ${HWND_BROADCAST} ${WM_SETTINGCHANGE} 0 "STR:Environment" /TIMEOUT=5000
FunctionEnd

Section "pak" SecInstall
    SetOutPath "$INSTDIR"
    File /oname=pak.exe "${PAK_EXE}"
    WriteUninstaller "$INSTDIR\uninstall.exe"

    WriteRegStr HKCU "Software\Microsoft\Windows\CurrentVersion\Uninstall\pak" "DisplayName" "pak"
    WriteRegStr HKCU "Software\Microsoft\Windows\CurrentVersion\Uninstall\pak" "DisplayVersion" "${PRODUCT_VERSION}"
    WriteRegStr HKCU "Software\Microsoft\Windows\CurrentVersion\Uninstall\pak" "Publisher" "xaviersupreme"
    WriteRegStr HKCU "Software\Microsoft\Windows\CurrentVersion\Uninstall\pak" "InstallLocation" "$INSTDIR"
    WriteRegStr HKCU "Software\Microsoft\Windows\CurrentVersion\Uninstall\pak" "UninstallString" "$INSTDIR\uninstall.exe"
    WriteRegDWORD HKCU "Software\Microsoft\Windows\CurrentVersion\Uninstall\pak" "NoModify" 1
    WriteRegDWORD HKCU "Software\Microsoft\Windows\CurrentVersion\Uninstall\pak" "NoRepair" 1

    Call AddToUserPath
SectionEnd

Section "Uninstall"
    Call un.RemoveFromUserPath
    Delete "$INSTDIR\pak.exe"
    Delete "$INSTDIR\uninstall.exe"
    RMDir "$INSTDIR"
    DeleteRegKey HKCU "Software\Microsoft\Windows\CurrentVersion\Uninstall\pak"
SectionEnd
