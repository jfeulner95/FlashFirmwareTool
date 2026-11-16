cmake_minimum_required(VERSION 3.10)
project(fwtool VERSION 0.1.0 LANGUAGES C)

set(CMAKE_C_STANDARD 99)
set(CMAKE_C_STANDARD_REQUIRED ON)

add_executable(fwtool
    src/fwtool.c
    src/flash.c
    src/serial_win.c
    src/serial_posix.c
)

if(WIN32)
    target_link_libraries(fwtool advapi32)
    target_compile_definitions(fwtool PRIVATE _CRT_SECURE_NO_WARNINGS __USE_MINGW_ANSI_STDIO=1)
endif()

if(MSVC)
    target_compile_options(fwtool PRIVATE /W3)
else()
    target_compile_options(fwtool PRIVATE -Wall -Wextra)
endif()
