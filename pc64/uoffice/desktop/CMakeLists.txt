# UnoWord, UnoCalc and UnoShow as native Windows / macOS / Linux apps.
# See README.md.  Configure from this directory:
#
#   cmake -S . -B build -DCMAKE_BUILD_TYPE=Release && cmake --build build
#
# The apps and every library under them are compiled from the SAME files
# pc64/build.sh links into APPS\UO*.UNO - nothing here is a copy.
cmake_minimum_required(VERSION 3.16)
project(UnoOffice VERSION 0.1.0 LANGUAGES C)

set(ROOT ${CMAKE_CURRENT_SOURCE_DIR}/../../..)
get_filename_component(ROOT ${ROOT} ABSOLUTE)
set(PC64 ${ROOT}/pc64)
set(UO   ${PC64}/uoffice)

option(UODESK_STATIC_SDL "link SDL2 statically (self-contained binaries)" OFF)
option(UODESK_SANITIZE  "build with ASan + UBSan (development)" OFF)

find_package(SDL2 CONFIG QUIET)
if(NOT SDL2_FOUND)
    find_package(PkgConfig REQUIRED)
    pkg_check_modules(SDL2 REQUIRED IMPORTED_TARGET sdl2)
    add_library(SDL2::SDL2 ALIAS PkgConfig::SDL2)
endif()
if(UODESK_STATIC_SDL AND TARGET SDL2::SDL2-static)
    set(UODESK_SDL SDL2::SDL2-static)
else()
    set(UODESK_SDL SDL2::SDL2)
endif()
# SDL2main supplies WinMain on Windows; elsewhere it is an empty shim
if(TARGET SDL2::SDL2main)
    set(UODESK_SDLMAIN SDL2::SDL2main)
endif()

# fb.c's 8x8 fallback font, generated exactly as pc64/build.sh generates it
find_package(Python3 REQUIRED COMPONENTS Interpreter)
add_custom_command(
    OUTPUT  ${PC64}/build/font_data.h
    COMMAND ${Python3_EXECUTABLE} ${PC64}/mkfont_c.py
    DEPENDS ${PC64}/mkfont_c.py ${ROOT}/amiga/gen_data.i
    COMMENT "generating the 8x8 fallback font")
add_custom_target(uodesk_font DEPENDS ${PC64}/build/font_data.h)

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
    uodesk_fs.c)
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
target_link_libraries(uodesk_platform PUBLIC ${UODESK_SDL})
if(NOT WIN32)
    target_link_libraries(uodesk_platform PUBLIC m)
endif()

set(CHROME uochrome uoicons uodlg uobars uofile)

# add_uoapp(<target> <display name> <app .c> <uoffice files> <unodoc files>)
# The per-app file lists mirror pc64/build.sh's [3d2]..[3d4] steps.
function(add_uoapp tgt display app lane doc)
    set(src ${CMAKE_CURRENT_SOURCE_DIR}/uodesk.c ${PC64}/apps/${app}.c
            ${ROOT}/unomedia/unomedia.c ${ROOT}/unomedia/um_inflate.c)
    foreach(b ${CHROME} ${lane})
        list(APPEND src ${UO}/${b}.c)
    endforeach()
    foreach(b ${doc})
        list(APPEND src ${ROOT}/unodoc/${b}.c)
    endforeach()
    if(APPLE)
        add_executable(${tgt} MACOSX_BUNDLE ${src})
        set_target_properties(${tgt} PROPERTIES
            OUTPUT_NAME "${display}"
            MACOSX_BUNDLE_BUNDLE_NAME "${display}"
            MACOSX_BUNDLE_GUI_IDENTIFIER "org.unodos.${tgt}"
            MACOSX_BUNDLE_BUNDLE_VERSION ${PROJECT_VERSION}
            MACOSX_BUNDLE_SHORT_VERSION_STRING ${PROJECT_VERSION})
    elseif(WIN32)
        add_executable(${tgt} WIN32 ${src})
        set_target_properties(${tgt} PROPERTIES OUTPUT_NAME "${display}")
    else()
        add_executable(${tgt} ${src})
        string(TOLOWER "${display}" lower)
        set_target_properties(${tgt} PROPERTIES OUTPUT_NAME "${lower}")
    endif()
    uodesk_flags(${tgt})
    target_compile_definitions(${tgt} PRIVATE UNO_APP_SYM=uno_app_main)
    target_link_libraries(${tgt} PRIVATE ${UODESK_SDLMAIN} uodesk_platform)
    if(WIN32)
        target_link_libraries(${tgt} PRIVATE shell32)
    endif()

    # the fonts ride beside the binary (inside the bundle on a Mac)
    file(GLOB FONTS ${PC64}/fonts/*.ttf)
    if(APPLE)
        set(fontdir $<TARGET_FILE_DIR:${tgt}>/../Resources/fonts)
    else()
        set(fontdir $<TARGET_FILE_DIR:${tgt}>/fonts)
    endif()
    add_custom_command(TARGET ${tgt} POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E make_directory ${fontdir}
        COMMAND ${CMAKE_COMMAND} -E copy ${FONTS} ${fontdir})
    install(TARGETS ${tgt} BUNDLE DESTINATION . RUNTIME DESTINATION .)
endfunction()

add_uoapp(uoword UnoWord uoword "uow_doc;uow_layout"
    "unodoc;ud_cfb;ud_doc;ud_docw;ud_zip;ud_xml;ud_docx;ud_ooxz;ud_docxw")
add_uoapp(uocalc UnoCalc uocalc "uxl_sheet;uxl_calc;uxl_numfmt"
    "unodoc;ud_cfb;ud_xls;ud_xlsw;ud_ptg;ud_ptgc;ud_zip;ud_xml;ud_xlsx;ud_ooxz;ud_xlsxw")
add_uoapp(uoshow UnoShow uoshow "uos_geom;uos_model;uos_render"
    "unodoc;ud_cfb;ud_ppt;ud_pptw;ud_escher;ud_zip;ud_xml;ud_pptx;ud_ooxz;ud_pptxw")

if(NOT APPLE)
    install(DIRECTORY ${PC64}/fonts/ DESTINATION fonts FILES_MATCHING PATTERN "*.ttf")
endif()
