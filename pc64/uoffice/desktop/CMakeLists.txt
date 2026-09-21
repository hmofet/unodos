# UnoWord, UnoCalc and UnoShow as native Windows / macOS / Linux apps.
# See README.md.  Configure from this directory:
#
#   cmake -S . -B build -DCMAKE_BUILD_TYPE=Release && cmake --build build
#   cd build && cpack          # installers: NSIS+ZIP / DMG / DEB+RPM+TGZ
#
# The apps and every library under them are compiled from the SAME files
# pc64/build.sh links into APPS\UO*.UNO - nothing here is a copy.  Under
# them sits one small backend per OS (uodesk_plat.h) on that OS's own API:
# nothing third-party is linked at all.
cmake_minimum_required(VERSION 3.16)
project(UnoOffice VERSION 0.2.0 LANGUAGES C
        DESCRIPTION "UnoWord, UnoCalc and UnoShow - an Office 97-style suite"
        HOMEPAGE_URL "https://github.com/hmofet/unooffice")
if(WIN32)
    enable_language(RC)
endif()
if(APPLE)
    enable_language(OBJC)
endif()

set(ROOT ${CMAKE_CURRENT_SOURCE_DIR}/../../..)
get_filename_component(ROOT ${ROOT} ABSOLUTE)
set(PC64 ${ROOT}/pc64)
set(UO   ${PC64}/uoffice)
set(PKG  ${CMAKE_CURRENT_SOURCE_DIR}/packaging)

option(UODESK_SANITIZE  "build with ASan + UBSan (development)" OFF)

# ---- the OS layer -------------------------------------------------------------
if(WIN32)
    set(UODESK_PLAT_SRC plat_win32.c)
    set(UODESK_OS_LIBS user32 gdi32 comdlg32 shell32)
elseif(APPLE)
    set(UODESK_PLAT_SRC plat_cocoa.m)
    set_source_files_properties(plat_cocoa.m PROPERTIES COMPILE_OPTIONS "-fobjc-arc")
    set(UODESK_OS_LIBS "-framework Cocoa")
else()
    find_package(X11 REQUIRED)
    set(UODESK_PLAT_SRC plat_x11.c)
    set(UODESK_OS_LIBS X11::X11)
    # the desktop portal's file picker; without libdbus, zenity/kdialog only
    find_package(PkgConfig QUIET)
    if(PKG_CONFIG_FOUND)
        pkg_check_modules(DBUS QUIET IMPORTED_TARGET dbus-1)
    endif()
    if(DBUS_FOUND)
        list(APPEND UODESK_OS_LIBS PkgConfig::DBUS)
        set(UODESK_HAVE_DBUS 1)
    else()
        message(STATUS "libdbus-1 not found: the file picker falls back to zenity/kdialog")
    endif()
endif()

# fb.c's 8x8 fallback font, generated exactly as pc64/build.sh generates it
find_package(Python3 REQUIRED COMPONENTS Interpreter)
add_custom_command(
    OUTPUT  ${PC64}/build/font_data.h
    COMMAND ${Python3_EXECUTABLE} ${PC64}/mkfont_c.py
    DEPENDS ${PC64}/mkfont_c.py ${ROOT}/amiga/gen_data.i
    COMMENT "generating the 8x8 fallback font")
add_custom_target(uodesk_font DEPENDS ${PC64}/build/font_data.h)

# ---- where things install ------------------------------------------------------
# Windows and the portable archives: everything in one folder, fonts beside
# the .exe.  macOS: fonts inside each bundle.  Linux packages: the FHS -
# binaries in bin/, fonts in share/unooffice/fonts (uodesk_fs.c looks in both
# places, relative to the executable).
set(CMAKE_INSTALL_DOCDIR share/doc/unooffice)   # not PROJECT_NAME's capitals
include(GNUInstallDirs)
if(WIN32)
    set(UODESK_BIN .)
    set(UODESK_FONTS fonts)
    set(UODESK_DOC .)
elseif(APPLE)
    set(UODESK_BIN .)
    set(UODESK_DOC .)
else()
    set(UODESK_BIN ${CMAKE_INSTALL_BINDIR})
    set(UODESK_FONTS ${CMAKE_INSTALL_DATADIR}/unooffice/fonts)
    set(UODESK_DOC ${CMAKE_INSTALL_DOCDIR})
endif()

# ---- the platform under every app: fb, TTF text, unoui ---------------------
file(GLOB THEMES ${ROOT}/unoui/themes/*.c)
add_library(uodesk_platform STATIC
    ${PC64}/fb.c
    ${PC64}/pc64_font.c
    ${ROOT}/unoui/unoui.c
    ${ROOT}/unoui/unoui_input.c
    ${ROOT}/unoui/unoui_anim.c
    ${ROOT}/unoui/unoui_wmanim.c
    ${THEMES}
    uodesk_fs.c
    uodesk_plat_common.c
    ${UODESK_PLAT_SRC})
add_dependencies(uodesk_platform uodesk_font)

# the framebuffer ceiling sizes a static array; a desktop can be a 4K monitor
set(UODESK_DEFS UNO_PC64 UNO_UUI UNO_COLOR=1 FB_MAX_W=3840 FB_MAX_H=2400)
set(UODESK_INC
    ${CMAKE_CURRENT_SOURCE_DIR}/compat   # ahead of pc64/: see compat/
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${PC64} ${UO} ${ROOT}/unoui ${ROOT}/unodoc ${ROOT}/unomedia)

if(MSVC)
    set(UODESK_WARN /W3)
else()
    set(UODESK_WARN -Wall -Wno-unused-parameter -Wno-unused-function
                    -Wno-sign-compare -Wno-missing-field-initializers
                    -Wno-misleading-indentation -Wno-comment)
endif()

function(uodesk_flags tgt)
    target_compile_definitions(${tgt} PRIVATE ${UODESK_DEFS})
    target_include_directories(${tgt} PRIVATE ${UODESK_INC})
    target_compile_options(${tgt} PRIVATE ${UODESK_WARN})
    set_property(TARGET ${tgt} PROPERTY C_STANDARD 99)
    if(UODESK_SANITIZE AND NOT MSVC)
        target_compile_options(${tgt} PRIVATE -fsanitize=address,undefined -fno-omit-frame-pointer)
        target_link_options(${tgt} PRIVATE -fsanitize=address,undefined)
    endif()
endfunction()
uodesk_flags(uodesk_platform)
target_link_libraries(uodesk_platform PUBLIC ${UODESK_OS_LIBS})
if(UODESK_HAVE_DBUS)
    target_compile_definitions(uodesk_platform PRIVATE UODESK_HAVE_DBUS)
endif()
if(NOT WIN32)
    target_link_libraries(uodesk_platform PUBLIC m)
endif()

set(CHROME uochrome uoicons uodlg uobars uofile)
file(GLOB FONTS ${PC64}/fonts/*.ttf)

# add_uoapp(<target> <display name> <app .c> <what it is> <uoffice files> <unodoc files>)
# The per-app file lists mirror pc64/build.sh's [3d2]..[3d4] steps.
function(add_uoapp tgt display app what lane doc)
    string(TOLOWER "${display}" lower)
    set(src ${CMAKE_CURRENT_SOURCE_DIR}/uodesk.c ${PC64}/apps/${app}.c
            ${ROOT}/unomedia/unomedia.c ${ROOT}/unomedia/um_inflate.c)
    foreach(b ${CHROME} ${lane})
        list(APPEND src ${UO}/${b}.c)
    endforeach()
    foreach(b ${doc})
        list(APPEND src ${ROOT}/unodoc/${b}.c)
    endforeach()

    if(APPLE)
        set(icns ${PKG}/icons/${lower}.icns)
        set_source_files_properties(${icns} PROPERTIES MACOSX_PACKAGE_LOCATION Resources)
        add_executable(${tgt} MACOSX_BUNDLE ${src} ${icns})
        set_target_properties(${tgt} PROPERTIES
            OUTPUT_NAME "${display}"
            MACOSX_BUNDLE_BUNDLE_NAME "${display}"
            MACOSX_BUNDLE_GUI_IDENTIFIER "org.unodos.${display}"
            MACOSX_BUNDLE_ICON_FILE "${lower}.icns"
            MACOSX_BUNDLE_INFO_STRING "${display} ${PROJECT_VERSION}"
            MACOSX_BUNDLE_COPYRIGHT "UnoDOS project, MPL-2.0"
            MACOSX_BUNDLE_BUNDLE_VERSION ${PROJECT_VERSION}
            MACOSX_BUNDLE_SHORT_VERSION_STRING ${PROJECT_VERSION})
    elseif(WIN32)
        # the icon and the version resource Explorer shows in Properties
        set(UO_ICON ${PKG}/icons/${lower}.ico)
        set(UO_DISPLAY ${display})
        set(UO_WHAT "${what}")
        string(REPLACE "." "," UO_VERCOMMA "${PROJECT_VERSION}.0")
        configure_file(${PKG}/app.rc.in ${CMAKE_CURRENT_BINARY_DIR}/${tgt}.rc @ONLY)
        add_executable(${tgt} WIN32 ${src} ${CMAKE_CURRENT_BINARY_DIR}/${tgt}.rc)
        set_target_properties(${tgt} PROPERTIES OUTPUT_NAME "${display}")
    else()
        add_executable(${tgt} ${src})
        set_target_properties(${tgt} PROPERTIES OUTPUT_NAME "${lower}")
    endif()
    uodesk_flags(${tgt})
    target_compile_definitions(${tgt} PRIVATE UNO_APP_SYM=uno_app_main)
    target_link_libraries(${tgt} PRIVATE uodesk_platform)
    if(WIN32)
        target_link_libraries(${tgt} PRIVATE shell32)
    endif()

    # the fonts ride beside the binary in the build tree (inside the bundle
    # on a Mac), so a build runs without being installed
    if(APPLE)
        set(fontdir $<TARGET_FILE_DIR:${tgt}>/../Resources/fonts)
    else()
        set(fontdir $<TARGET_FILE_DIR:${tgt}>/fonts)
    endif()
    add_custom_command(TARGET ${tgt} POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E make_directory ${fontdir}
        COMMAND ${CMAKE_COMMAND} -E copy ${FONTS} ${fontdir})

    install(TARGETS ${tgt} BUNDLE DESTINATION ${UODESK_BIN} RUNTIME DESTINATION ${UODESK_BIN})
    if(APPLE)
        # Ad-hoc sign the INSTALLED bundle: the fonts and icon were added
        # after the linker signed the binary, and an Apple-silicon Mac calls
        # a bundle whose seal does not match "damaged" - worse than the
        # "unidentified developer" an unsigned-but-consistent one gets.
        install(CODE "execute_process(COMMAND codesign --force --deep --sign - \"\$ENV{DESTDIR}\${CMAKE_INSTALL_PREFIX}/${display}.app\")")
    elseif(NOT WIN32)
        set(UO_DISPLAY ${display})
        set(UO_EXEC ${lower})
        set(UO_WHAT "${what}")
        configure_file(${PKG}/app.desktop.in
                       ${CMAKE_CURRENT_BINARY_DIR}/org.unodos.${display}.desktop @ONLY)
        install(FILES ${CMAKE_CURRENT_BINARY_DIR}/org.unodos.${display}.desktop
                DESTINATION ${CMAKE_INSTALL_DATADIR}/applications)
        foreach(sz 16 32 48 64 128 256 512)
            install(FILES ${PKG}/icons/${lower}-${sz}.png
                    DESTINATION ${CMAKE_INSTALL_DATADIR}/icons/hicolor/${sz}x${sz}/apps
                    RENAME org.unodos.${display}.png)
        endforeach()
    endif()
endfunction()

add_uoapp(uoword UnoWord uoword "Word processor" "uow_doc;uow_layout"
    "unodoc;ud_cfb;ud_doc;ud_docw;ud_zip;ud_xml;ud_docx;ud_ooxz;ud_docxw")
add_uoapp(uocalc UnoCalc uocalc "Spreadsheet" "uxl_sheet;uxl_calc;uxl_numfmt"
    "unodoc;ud_cfb;ud_xls;ud_xlsw;ud_ptg;ud_ptgc;ud_zip;ud_xml;ud_xlsx;ud_ooxz;ud_xlsxw")
add_uoapp(uoshow UnoShow uoshow "Presentations" "uos_geom;uos_model;uos_render"
    "unodoc;ud_cfb;ud_ppt;ud_pptw;ud_escher;ud_zip;ud_xml;ud_pptx;ud_ooxz;ud_pptxw")

if(NOT APPLE)
    install(FILES ${FONTS} DESTINATION ${UODESK_FONTS})
endif()
install(FILES ${ROOT}/LICENSE DESTINATION ${UODESK_DOC} RENAME LICENSE.txt)
install(FILES ${PC64}/fonts/README.md DESTINATION ${UODESK_DOC} RENAME FONTS.txt)
install(FILES ${CMAKE_CURRENT_SOURCE_DIR}/README.md DESTINATION ${UODESK_DOC} RENAME README.txt)

# the icon renderer (packaging/mkicons.c) - a developer tool, not shipped
add_executable(uodesk_mkicons EXCLUDE_FROM_ALL
    ${PKG}/mkicons.c ${PC64}/pc64_icons.c ${PC64}/pc64_qoi.c)
uodesk_flags(uodesk_mkicons)
find_package(ZLIB QUIET)
if(ZLIB_FOUND)
    target_link_libraries(uodesk_mkicons PRIVATE uodesk_platform ZLIB::ZLIB)
endif()

# ---- packages (cpack) -----------------------------------------------------------
include(packaging/CPackConfig.cmake)
