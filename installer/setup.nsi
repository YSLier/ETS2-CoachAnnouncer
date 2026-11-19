# -*- coding: utf-8 -*-
# Stage the full manual archive's three payload directories under PAYLOAD.
Target amd64-unicode
!include "MUI2.nsh"
!include "LogicLib.nsh"
!include "FileFunc.nsh"
!include "StrFunc.nsh"
!include "WordFunc.nsh"
${StrRep}
${StrStr}

!ifndef PAYLOAD
!error "Set PAYLOAD to the extracted full installation archive"
!endif
!ifndef OUTPUT
!define OUTPUT "CoachAnnouncer-0.7.3-Setup-Win64.exe"
!endif

Name "CoachAnnouncer 0.7.3"
OutFile "${OUTPUT}"
InstallDir "$PROGRAMFILES32\Steam\steamapps\common\Euro Truck Simulator 2"
RequestExecutionLevel highest
SetCompressor /SOLID lzma
BrandingText "CoachAnnouncer 0.7.3"

VIProductVersion "0.7.3.0"
VIAddVersionKey /LANG=2052 "ProductName" "CoachAnnouncer"
VIAddVersionKey /LANG=2052 "FileDescription" "CoachAnnouncer 0.7.3 离线安装程序"
VIAddVersionKey /LANG=2052 "FileVersion" "0.7.3"
VIAddVersionKey /LANG=2052 "ProductVersion" "0.7.3"
VIAddVersionKey /LANG=2052 "LegalCopyright" "CoachAnnouncer contributors"

!define MUI_WELCOMEPAGE_TITLE "安装 CoachAnnouncer 0.7.3"
!define MUI_WELCOMEPAGE_TEXT "安装前请完全退出 ETS2。$\r$\n$\r$\n安装程序会查找 Steam 游戏目录，并安装报站插件、四语语音及站点坐标。缺少 SPF Framework 时会一并安装。"
!insertmacro MUI_PAGE_WELCOME
!define MUI_DIRECTORYPAGE_TEXT_TOP "选择包含 bin\win_x64\eurotrucks2.exe 的 ETS2 游戏目录。"
!define MUI_PAGE_CUSTOMFUNCTION_LEAVE VerifyDirectory
!insertmacro MUI_PAGE_DIRECTORY
!insertmacro MUI_PAGE_INSTFILES
!define MUI_FINISHPAGE_TEXT "安装完成。再次启动 ETS2，在 SPF 插件管理中启用 CoachAnnouncerPanel。默认面板快捷键为左 Ctrl + 左 Shift + F8。"
!insertmacro MUI_PAGE_FINISH
!insertmacro MUI_LANGUAGE "SimpChinese"

Var SteamPath
Var BackupDir

!macro AcceptGameFolder PATH
    IfFileExists "${PATH}\bin\win_x64\eurotrucks2.exe" 0 +3
    StrCpy $INSTDIR "${PATH}"
    Return
!macroend

Function .onInit
    SetShellVarContext current
    SetRegView 64
    ReadRegStr $0 HKLM "SOFTWARE\Microsoft\Windows\CurrentVersion\Uninstall\Steam App 227300" "InstallLocation"
    !insertmacro AcceptGameFolder "$0"
    SetRegView 32
    ReadRegStr $0 HKLM "SOFTWARE\Microsoft\Windows\CurrentVersion\Uninstall\Steam App 227300" "InstallLocation"
    !insertmacro AcceptGameFolder "$0"
    ReadRegStr $SteamPath HKCU "Software\Valve\Steam" "SteamPath"
    ${If} $SteamPath == ""
        ReadRegStr $SteamPath HKLM "Software\Valve\Steam" "InstallPath"
    ${EndIf}
    ${If} $SteamPath == ""
        StrCpy $SteamPath "$PROGRAMFILES32\Steam"
    ${EndIf}
    ${StrRep} $SteamPath $SteamPath "/" "\"
    !insertmacro AcceptGameFolder "$SteamPath\steamapps\common\Euro Truck Simulator 2"

    ClearErrors
    FileOpen $1 "$SteamPath\steamapps\libraryfolders.vdf" r
    IfErrors steam_done
    steam_read:
        FileRead $1 $2
        IfErrors steam_close
        ${WordFind} "$2" '$\"path$\"' "E+1}" $3
        IfErrors steam_read
        ${WordFind} "$3" '$\"' "E+2" $4
        IfErrors steam_read
        ${StrRep} $4 $4 "\\" "\"
        StrCpy $0 "$4\steamapps\common\Euro Truck Simulator 2"
        IfFileExists "$0\bin\win_x64\eurotrucks2.exe" 0 steam_read
        StrCpy $INSTDIR "$0"
    steam_close:
        FileClose $1
    steam_done:
    ClearErrors
FunctionEnd

Function VerifyDirectory
    IfFileExists "$INSTDIR\bin\win_x64\eurotrucks2.exe" valid
    MessageBox MB_OK|MB_ICONEXCLAMATION "这个目录里找不到 ETS2。请从 Steam 的「浏览本地文件」打开游戏目录，再选择该目录。"
    Abort
    valid:
FunctionEnd

!macro CheckCopy
    IfErrors install_failed
!macroend

Section "CoachAnnouncer"
    IfFileExists "$INSTDIR\bin\win_x64\eurotrucks2.exe" +3
    SetErrorLevel 2
    Quit

    nsExec::ExecToStack '"$SYSDIR\tasklist.exe" /FI "IMAGENAME eq eurotrucks2.exe" /NH /FO CSV'
    Pop $0
    Pop $1
    ${If} $0 == 0
        ${StrStr} $2 $1 "eurotrucks2.exe"
        ${If} $2 != ""
            MessageBox MB_OK|MB_ICONEXCLAMATION "请完全退出 Euro Truck Simulator 2，再运行安装程序。" /SD IDOK
            SetErrorLevel 3
            Quit
        ${EndIf}
    ${EndIf}

    ClearErrors
    SetOutPath "$LOCALAPPDATA\CoachAnnouncer"
    SetOverwrite off
    File "${PAYLOAD}/02-LocalAppData/CoachAnnouncer/settings.ini"
    File "${PAYLOAD}/02-LocalAppData/CoachAnnouncer/station-map-ets2-1.62.tsv"
    SetOverwrite on
    File "${PAYLOAD}/NOTICE-STATION-MAP.txt"
    File "${PAYLOAD}/TRUCKDASH-LICENSE.txt"
    !insertmacro CheckCopy
    SetOutPath "$LOCALAPPDATA\CoachAnnouncer\audio"
    File /r "${PAYLOAD}/02-LocalAppData/CoachAnnouncer/audio/*"
    !insertmacro CheckCopy

    IfFileExists "$INSTDIR\bin\win_x64\plugins\spf-framework.dll" spf_ready
    SetOutPath "$INSTDIR\bin\win_x64\plugins"
    File /r "${PAYLOAD}/03-SPF-1.2.7/bin/win_x64/plugins/*"
    !insertmacro CheckCopy
    spf_ready:

    ${GetTime} "" "L" $0 $1 $2 $3 $4 $5 $6
    StrCpy $BackupDir "$LOCALAPPDATA\CoachAnnouncer\backup\$2-$1-$0-$4$5$6"
    IfFileExists "$INSTDIR\bin\win_x64\plugins\CoachAnnouncer.dll" 0 no_main_backup
    CreateDirectory "$BackupDir"
    CopyFiles /SILENT "$INSTDIR\bin\win_x64\plugins\CoachAnnouncer.dll" "$BackupDir\CoachAnnouncer.dll"
    !insertmacro CheckCopy
    no_main_backup:
    IfFileExists "$INSTDIR\bin\win_x64\plugins\spfPlugins\CoachAnnouncerPanel\CoachAnnouncerPanel.dll" 0 no_panel_backup
    CreateDirectory "$BackupDir"
    CopyFiles /SILENT "$INSTDIR\bin\win_x64\plugins\spfPlugins\CoachAnnouncerPanel\CoachAnnouncerPanel.dll" "$BackupDir\CoachAnnouncerPanel.dll"
    !insertmacro CheckCopy
    no_panel_backup:

    SetOutPath "$INSTDIR\bin\win_x64\plugins"
    File "${PAYLOAD}/01-Game/bin/win_x64/plugins/CoachAnnouncer.dll"
    IfErrors restore_plugin
    SetOutPath "$INSTDIR\bin\win_x64\plugins\spfPlugins\CoachAnnouncerPanel"
    File /r "${PAYLOAD}/01-Game/bin/win_x64/plugins/spfPlugins/CoachAnnouncerPanel/*"
    IfErrors restore_plugin
    Goto install_done
    restore_plugin:
    IfFileExists "$BackupDir\CoachAnnouncer.dll" 0 +3
    CopyFiles /SILENT "$BackupDir\CoachAnnouncer.dll" "$INSTDIR\bin\win_x64\plugins\CoachAnnouncer.dll"
    Goto restore_panel
    Delete "$INSTDIR\bin\win_x64\plugins\CoachAnnouncer.dll"
    restore_panel:
    IfFileExists "$BackupDir\CoachAnnouncerPanel.dll" 0 +3
    CopyFiles /SILENT "$BackupDir\CoachAnnouncerPanel.dll" "$INSTDIR\bin\win_x64\plugins\spfPlugins\CoachAnnouncerPanel\CoachAnnouncerPanel.dll"
    Goto install_failed
    Delete "$INSTDIR\bin\win_x64\plugins\spfPlugins\CoachAnnouncerPanel\CoachAnnouncerPanel.dll"
    install_failed:
    MessageBox MB_OK|MB_ICONSTOP "无法完成安装。确认 ETS2 已退出、目录可写。旧 DLL 的备份位于 %LOCALAPPDATA%\CoachAnnouncer\backup。" /SD IDOK
    SetErrorLevel 4
    Quit
    install_done:
SectionEnd
