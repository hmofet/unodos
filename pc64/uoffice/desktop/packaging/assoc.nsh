; assoc.nsh - file associations for the NSIS installer (CPackConfig.cmake
; includes this into the install and uninstall sections).
;
; Windows 10 and 11 do not let an installer MAKE itself the default for .doc
; - only the user can, from "Open with" or Settings > Default apps - so this
; registers everything those two places read, and nothing more:
;
;   a ProgID per app          UnoOffice.UnoWord.Document: its icon and the
;                             "open" command, "%1" being the file
;   OpenWithProgids           lists UnoWord under .doc's "Open with", without
;                             taking .doc away from whatever already has it
;   Capabilities + Registered-
;   Applications              the app's entry in Settings > Default apps
;
; SHCTX is HKLM when the installer runs for all users (the Program Files
; install) and HKCU otherwise, the same choice the Start-menu entries make.
!ifndef UO_ASSOC_NSH
!define UO_ASSOC_NSH

!macro UO_ASSOC_APP APP DESC
  WriteRegStr SHCTX "Software\Classes\UnoOffice.${APP}.Document" "" "${DESC}"
  WriteRegStr SHCTX "Software\Classes\UnoOffice.${APP}.Document\DefaultIcon" "" "$INSTDIR\${APP}.exe,0"
  WriteRegStr SHCTX "Software\Classes\UnoOffice.${APP}.Document\shell\open\command" "" '"$INSTDIR\${APP}.exe" "%1"'
  WriteRegStr SHCTX "Software\Classes\Applications\${APP}.exe\shell\open\command" "" '"$INSTDIR\${APP}.exe" "%1"'
  WriteRegStr SHCTX "Software\UnoOffice\${APP}\Capabilities" "ApplicationName" "${APP}"
  WriteRegStr SHCTX "Software\UnoOffice\${APP}\Capabilities" "ApplicationDescription" "${DESC} editor from UnoOffice"
  WriteRegStr SHCTX "Software\RegisteredApplications" "UnoOffice ${APP}" "Software\UnoOffice\${APP}\Capabilities"
!macroend

!macro UO_ASSOC_EXT APP EXT
  WriteRegStr SHCTX "Software\Classes\${EXT}\OpenWithProgids" "UnoOffice.${APP}.Document" ""
  WriteRegStr SHCTX "Software\Classes\Applications\${APP}.exe\SupportedTypes" "${EXT}" ""
  WriteRegStr SHCTX "Software\UnoOffice\${APP}\Capabilities\FileAssociations" "${EXT}" "UnoOffice.${APP}.Document"
!macroend

!macro UO_UNASSOC_APP APP
  DeleteRegKey SHCTX "Software\Classes\UnoOffice.${APP}.Document"
  DeleteRegKey SHCTX "Software\Classes\Applications\${APP}.exe"
  DeleteRegKey SHCTX "Software\UnoOffice\${APP}"
  DeleteRegValue SHCTX "Software\RegisteredApplications" "UnoOffice ${APP}"
!macroend

!macro UO_UNASSOC_EXT APP EXT
  DeleteRegValue SHCTX "Software\Classes\${EXT}\OpenWithProgids" "UnoOffice.${APP}.Document"
!macroend

; Explorer caches associations: tell it they changed (SHCNE_ASSOCCHANGED)
!macro UO_ASSOC_CHANGED
  System::Call 'shell32::SHChangeNotify(i 0x08000000, i 0, p 0, p 0)'
!macroend

!macro UO_ASSOC_ALL
  !insertmacro UO_ASSOC_APP UnoWord "Word Document"
  !insertmacro UO_ASSOC_EXT UnoWord ".doc"
  !insertmacro UO_ASSOC_EXT UnoWord ".docx"
  !insertmacro UO_ASSOC_APP UnoCalc "Excel Workbook"
  !insertmacro UO_ASSOC_EXT UnoCalc ".xls"
  !insertmacro UO_ASSOC_EXT UnoCalc ".xlsx"
  !insertmacro UO_ASSOC_APP UnoShow "PowerPoint Presentation"
  !insertmacro UO_ASSOC_EXT UnoShow ".ppt"
  !insertmacro UO_ASSOC_EXT UnoShow ".pptx"
  !insertmacro UO_ASSOC_CHANGED
!macroend

!macro UO_UNASSOC_ALL
  !insertmacro UO_UNASSOC_EXT UnoWord ".doc"
  !insertmacro UO_UNASSOC_EXT UnoWord ".docx"
  !insertmacro UO_UNASSOC_APP UnoWord
  !insertmacro UO_UNASSOC_EXT UnoCalc ".xls"
  !insertmacro UO_UNASSOC_EXT UnoCalc ".xlsx"
  !insertmacro UO_UNASSOC_APP UnoCalc
  !insertmacro UO_UNASSOC_EXT UnoShow ".ppt"
  !insertmacro UO_UNASSOC_EXT UnoShow ".pptx"
  !insertmacro UO_UNASSOC_APP UnoShow
  DeleteRegKey /ifempty SHCTX "Software\UnoOffice"
  !insertmacro UO_ASSOC_CHANGED
!macroend

!endif
