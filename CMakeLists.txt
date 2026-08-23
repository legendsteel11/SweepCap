cmake_minimum_required(VERSION 3.25)
project(SweepCap VERSION 0.1.0 LANGUAGES CXX)

if(NOT MSVC)
    message(FATAL_ERROR "SweepCap builds with MSVC only.")
endif()

enable_language(RC)

option(SWEEPCAP_ASAN "Enable AddressSanitizer in debug builds" ON)

set(SWEEPCAP_ASAN_ACTIVE OFF)
if(SWEEPCAP_ASAN AND CMAKE_BUILD_TYPE STREQUAL "Debug")
    set(SWEEPCAP_ASAN_ACTIVE ON)
endif()

# CRT linkage.
#
# Release uses the static CRT so the shipped executable is a single file with
# no Visual C++ redistributable to install.
#
# Only ASan-enabled debug builds switch to the dynamic CRT. This toolset ships
# the ASan runtime as clang_rt.asan_dynamic with no static library; mixing it
# with /MTd links fine and then misbehaves silently at runtime. Debug builds
# are never shipped, so a DLL dependency there does not matter.
if(SWEEPCAP_ASAN_ACTIVE)
    set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreadedDebugDLL")
else()
    set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
endif()

# ASan is incompatible with /RTC1, which CMake adds to debug builds by default.
string(REPLACE "/RTC1" "" CMAKE_C_FLAGS_DEBUG "${CMAKE_C_FLAGS_DEBUG}")
string(REPLACE "/RTC1" "" CMAKE_CXX_FLAGS_DEBUG "${CMAKE_CXX_FLAGS_DEBUG}")

add_executable(sweepcap WIN32
    src/main.cpp
    src/hook.cpp
    src/app.cpp
    src/capture.cpp
    src/coords.cpp
    src/log.cpp
    src/output.cpp
    src/overlay.cpp
    src/session.cpp
    src/settings.cpp
    src/tray.cpp
    src/windowpick.cpp
    src/winsnap.cpp
    res/sweepcap.rc
)

set_target_properties(sweepcap PROPERTIES OUTPUT_NAME "SweepCap")

# The resource script pulls in files the build system cannot see by reading it,
# so replacing the icon left the old one compiled in and the build reported
# nothing to do.
set_source_files_properties(res/sweepcap.rc PROPERTIES
    OBJECT_DEPENDS
    "${CMAKE_CURRENT_SOURCE_DIR}/res/sweepcap.ico;${CMAKE_CURRENT_SOURCE_DIR}/res/sweepcap.manifest;${CMAKE_CURRENT_SOURCE_DIR}/res/resource.h;${CMAKE_CURRENT_SOURCE_DIR}/src/app_name.h")

target_compile_features(sweepcap PRIVATE cxx_std_20)

target_include_directories(sweepcap PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/src
    ${CMAKE_CURRENT_SOURCE_DIR}/third_party
)

target_compile_definitions(sweepcap PRIVATE
    UNICODE
    _UNICODE
    WIN32_LEAN_AND_MEAN
    NOMINMAX
    STRICT
    WINVER=0x0A00
    _WIN32_WINNT=0x0A00
)

target_compile_options(sweepcap PRIVATE
    /W4 /WX
    /permissive-
    /utf-8
    /Zc:__cplusplus
    /EHsc
    /MP
    $<$<CONFIG:Release>:/Zi>
)

# res/sweepcap.rc embeds the manifest. Letting the linker add one as well
# produces CVT1100 (duplicate resource).
target_link_options(sweepcap PRIVATE /MANIFEST:NO)

target_link_options(sweepcap PRIVATE
    $<$<CONFIG:Release>:/DEBUG>
    $<$<CONFIG:Release>:/OPT:REF>
    $<$<CONFIG:Release>:/OPT:ICF>
)

# All inbox libraries: the project builds without a package manager.
target_link_libraries(sweepcap PRIVATE
    user32      # windows, messages, monitors
    shell32     # Shell_NotifyIcon, SHGetKnownFolderPath
    shcore      # GetDpiForMonitor
    dwmapi      # DWMWA_EXTENDED_FRAME_BOUNDS
    ole32       # CoTaskMemFree
    comctl32    # LoadIconMetric
    windowscodecs  # WIC PNG encoding
)

if(SWEEPCAP_ASAN_ACTIVE)
    target_compile_options(sweepcap PRIVATE /fsanitize=address)
    target_link_options(sweepcap PRIVATE /INCREMENTAL:NO)

    # Copy the ASan runtime next to the executable.
    #
    # It is a static import, so a missing DLL keeps the process from starting at
    # all and leaves only a loader error dialog. Which of the two variants the
    # linker picks depends on the toolset revision, so copy both.
    file(TO_CMAKE_PATH "$ENV{VCToolsInstallDir}" SWEEPCAP_VCTOOLS)
    set(SWEEPCAP_ASAN_DLLS "")
    foreach(dll_name clang_rt.asan_dynamic-x86_64.dll clang_rt.asan_dbg_dynamic-x86_64.dll)
        unset(dll_path CACHE)
        find_file(dll_path NAMES ${dll_name}
                  HINTS "${SWEEPCAP_VCTOOLS}/bin/Hostx64/x64"
                  NO_DEFAULT_PATH)
        if(dll_path)
            list(APPEND SWEEPCAP_ASAN_DLLS "${dll_path}")
        endif()
    endforeach()
    unset(dll_path CACHE)

    if(SWEEPCAP_ASAN_DLLS)
        add_custom_command(TARGET sweepcap POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E copy_if_different
                    ${SWEEPCAP_ASAN_DLLS} "$<TARGET_FILE_DIR:sweepcap>")
        message(STATUS "ASan runtime: ${SWEEPCAP_ASAN_DLLS}")
    else()
        message(WARNING
            "ASan runtime DLL not found; the debug build will not start.\n"
            "  Build through vcvarsall, or turn it off with -DSWEEPCAP_ASAN=OFF.")
    endif()
endif()

# The icon is the output of tools/make_icon.ps1.
if(NOT EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/res/sweepcap.ico")
    message(FATAL_ERROR
        "res/sweepcap.ico is missing. Generate it with:\n"
        "  powershell -NoProfile -ExecutionPolicy Bypass -File tools/make_icon.ps1 res/sweepcap.ico")
endif()
