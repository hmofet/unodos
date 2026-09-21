# The installers, one set per OS (included from ../CMakeLists.txt):
#
#   Windows  NSIS installer (Start-menu entries, uninstaller, Add/Remove
#            Programs) + a portable ZIP
#   macOS    a drag-to-Applications DMG holding the three .app bundles
#   Linux    .deb + .rpm (FHS layout, .desktop entries, hicolor icons) + TGZ
#
# Build them with `cpack` in the build directory, or `cpack -G <gen>`.

set(CPACK_PACKAGE_NAME "UnoOffice")
set(CPACK_PACKAGE_VENDOR "UnoDOS project")
set(CPACK_PACKAGE_VERSION ${PROJECT_VERSION})
set(CPACK_PACKAGE_DESCRIPTION_SUMMARY "${PROJECT_DESCRIPTION}")
set(CPACK_PACKAGE_DESCRIPTION
"UnoOffice is the Office 97-style suite from UnoDOS - UnoWord (word
processor), UnoCalc (spreadsheet) and UnoShow (presentations) - as native
desktop applications. It reads and writes .doc, .xls and .ppt files as well
as their OOXML counterparts.")
set(CPACK_PACKAGE_HOMEPAGE_URL "${PROJECT_HOMEPAGE_URL}")
set(CPACK_PACKAGE_INSTALL_DIRECTORY "UnoOffice")
set(CPACK_RESOURCE_FILE_LICENSE "${ROOT}/LICENSE")
set(CPACK_STRIP_FILES ON)

if(WIN32)
    set(UODESK_PLAT windows-x64)
elseif(APPLE)
    if(CMAKE_OSX_ARCHITECTURES MATCHES "arm64" AND CMAKE_OSX_ARCHITECTURES MATCHES "x86_64")
        set(UODESK_PLAT macos-universal)
    else()
        set(UODESK_PLAT macos-${CMAKE_SYSTEM_PROCESSOR})
    endif()
else()
    set(UODESK_PLAT linux-${CMAKE_SYSTEM_PROCESSOR})
endif()
set(CPACK_PACKAGE_FILE_NAME "UnoOffice-${PROJECT_VERSION}-${UODESK_PLAT}")

# ---- Windows: NSIS ------------------------------------------------------------
set(CPACK_NSIS_DISPLAY_NAME "UnoOffice")
# also the Start-menu folder name, so no version in it (Add/Remove Programs
# shows the version in its own column)
set(CPACK_NSIS_PACKAGE_NAME "UnoOffice")
set(CPACK_NSIS_INSTALL_ROOT "$PROGRAMFILES64")
set(CPACK_NSIS_MUI_ICON   "${PKG}/icons/unoword.ico")
set(CPACK_NSIS_MUI_UNIICON "${PKG}/icons/unoword.ico")
set(CPACK_NSIS_INSTALLED_ICON_NAME "UnoWord.exe")
set(CPACK_NSIS_URL_INFO_ABOUT "${PROJECT_HOMEPAGE_URL}")
set(CPACK_NSIS_HELP_LINK "${PROJECT_HOMEPAGE_URL}")
set(CPACK_NSIS_ENABLE_UNINSTALL_BEFORE_INSTALL ON)
set(CPACK_NSIS_EXECUTABLES_DIRECTORY ".")
# Start menu: UnoOffice\UnoWord, UnoCalc, UnoShow (exe name;label pairs)
set(CPACK_PACKAGE_EXECUTABLES "UnoWord;UnoWord;UnoCalc;UnoCalc;UnoShow;UnoShow")
set(CPACK_NSIS_MENU_LINKS "README.txt;Read me")

# ---- macOS: DMG -------------------------------------------------------------
set(CPACK_DMG_VOLUME_NAME "UnoOffice")
set(CPACK_DMG_FORMAT UDZO)
# the MPL is not an end-user agreement to click through
set(CPACK_DMG_SLA_USE_RESOURCE_FILE_LICENSE OFF)

# ---- Linux: DEB + RPM ----------------------------------------------------------
set(CPACK_DEBIAN_PACKAGE_NAME "unooffice")
set(CPACK_DEBIAN_FILE_NAME DEB-DEFAULT)
set(CPACK_DEBIAN_PACKAGE_MAINTAINER "UnoDOS project <hmofet@users.noreply.github.com>")
set(CPACK_DEBIAN_PACKAGE_SECTION "editors")
set(CPACK_DEBIAN_PACKAGE_SHLIBDEPS ON)
# SDL is linked in, and it dlopen()s the display libraries at run time, so
# the linker cannot see them: X11 is required, the rest widen what works.
set(CPACK_DEBIAN_PACKAGE_DEPENDS "libx11-6, libxext6")
set(CPACK_DEBIAN_PACKAGE_RECOMMENDS
    "libxcursor1, libxi6, libxrandr2, libxss1, libxkbcommon0, libwayland-client0, libwayland-cursor0, libwayland-egl1, libdecor-0-0")

set(CPACK_RPM_PACKAGE_NAME "unooffice")
set(CPACK_RPM_FILE_NAME RPM-DEFAULT)
set(CPACK_RPM_PACKAGE_LICENSE "MPL-2.0")
set(CPACK_RPM_PACKAGE_GROUP "Applications/Productivity")
set(CPACK_RPM_PACKAGE_URL "${PROJECT_HOMEPAGE_URL}")
set(CPACK_RPM_PACKAGE_REQUIRES "libX11, libXext")
set(CPACK_RPM_PACKAGE_SUGGESTS "libXcursor, libXi, libXrandr, libXScrnSaver, libxkbcommon, libwayland-client, libwayland-cursor, libdecor")
# directories the system owns, which the package must not claim
set(CPACK_RPM_EXCLUDE_FROM_AUTO_FILELIST_ADDITION
    /usr/share/applications
    /usr/share/icons
    /usr/share/icons/hicolor
    /usr/share/icons/hicolor/16x16 /usr/share/icons/hicolor/16x16/apps
    /usr/share/icons/hicolor/32x32 /usr/share/icons/hicolor/32x32/apps
    /usr/share/icons/hicolor/48x48 /usr/share/icons/hicolor/48x48/apps
    /usr/share/icons/hicolor/64x64 /usr/share/icons/hicolor/64x64/apps
    /usr/share/icons/hicolor/128x128 /usr/share/icons/hicolor/128x128/apps
    /usr/share/icons/hicolor/256x256 /usr/share/icons/hicolor/256x256/apps
    /usr/share/icons/hicolor/512x512 /usr/share/icons/hicolor/512x512/apps
    /usr/share/doc)

if(WIN32)
    set(CPACK_GENERATOR "NSIS;ZIP")
elseif(APPLE)
    set(CPACK_GENERATOR "DragNDrop")
else()
    set(CPACK_GENERATOR "DEB;RPM;TGZ")
    set(CPACK_PACKAGING_INSTALL_PREFIX "/usr")
endif()

include(CPack)
