cmake_minimum_required(VERSION 3.10)

# Single source of truth for the version: ../VERSION
file(STRINGS "${CMAKE_CURRENT_SOURCE_DIR}/../VERSION" FWTOOL_VERSION LIMIT_COUNT 1)
string(STRIP "${FWTOOL_VERSION}" FWTOOL_VERSION)
project(fwtool VERSION ${FWTOOL_VERSION} LANGUAGES C)

option(FWTOOL_WERROR "Treat compiler warnings as errors" OFF)

set(CMAKE_C_STANDARD 99)
set(CMAKE_C_STANDARD_REQUIRED ON)

add_executable(fwtool
    src/fwtool.c
    src/flash.c
    src/serial_win.c
    src/serial_posix.c
)
target_compile_definitions(fwtool PRIVATE FWTOOL_VERSION="${FWTOOL_VERSION}")

if(WIN32)
    target_link_libraries(fwtool advapi32)
    target_compile_definitions(fwtool PRIVATE _CRT_SECURE_NO_WARNINGS __USE_MINGW_ANSI_STDIO=1)
endif()

if(MSVC)
    target_compile_options(fwtool PRIVATE /W3 $<$<BOOL:${FWTOOL_WERROR}>:/WX>)
else()
    target_compile_options(fwtool PRIVATE -Wall -Wextra $<$<BOOL:${FWTOOL_WERROR}>:-Werror>)
endif()
