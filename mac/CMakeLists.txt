# UnoDOS/Mac - two applications from one source.
#
# Build with the Retro68 toolchain (see build.sh):
#   cmake .. -DCMAKE_TOOLCHAIN_FILE=<Retro68-build>/toolchain/m68k-apple-macos/cmake/retro68.toolchain.cmake
#   cmake --build .
#
# Produces:
#   UnoDOS7        - System 7, Color QuickDraw (Mac II / LC / Quadra)
#   UnoDOSClassic  - System 1-6, 1-bit QuickDraw (Mac Plus / SE / Classic)
cmake_minimum_required(VERSION 3.5)
project(UnoDOSMac C)

# Color build (System 7+, Color QuickDraw). Target 68020 (Mac II family).
add_application(UnoDOS7 unodos.c)
target_compile_definitions(UnoDOS7 PRIVATE UNO_COLOR=1)
target_compile_options(UnoDOS7 PRIVATE -ffunction-sections)
set_target_properties(UnoDOS7 PROPERTIES LINK_FLAGS "-Wl,-gc-sections -Wl,--mac-strip-macsbug")

# Mono build (System 1-6, classic QuickDraw). Target 68000 for the Plus.
add_application(UnoDOSClassic unodos.c)
target_compile_definitions(UnoDOSClassic PRIVATE UNO_COLOR=0)
target_compile_options(UnoDOSClassic PRIVATE -ffunction-sections -m68000)
set_target_properties(UnoDOSClassic PROPERTIES LINK_FLAGS "-Wl,-gc-sections -Wl,--mac-strip-macsbug")
