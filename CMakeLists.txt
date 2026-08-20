cmake_minimum_required(VERSION 3.25)
project(SweepCap VERSION 0.1.0 LANGUAGES CXX)

if(NOT MSVC)
    message(FATAL_ERROR "SweepCap은 MSVC로만 빌드한다. (CLAUDE.md '절대 제약')")
endif()

enable_language(RC)

option(SWEEPCAP_ASAN "디버그 빌드에 AddressSanitizer를 넣는다" ON)

set(SWEEPCAP_ASAN_ACTIVE OFF)
if(SWEEPCAP_ASAN AND CMAKE_BUILD_TYPE STREQUAL "Debug")
    set(SWEEPCAP_ASAN_ACTIVE ON)
endif()

# CRT 연결 방식.
#
# Release는 정적 CRT(/MT)다. "단일 exe로 배포한다. 별도 런타임 설치를 요구하지 않는다"를
# 만족시키려면 VC++ 재배포 패키지 의존이 없어야 한다.
#
# ASan을 켠 Debug만 동적 CRT(/MDd)로 간다. 이 툴셋(14.44)은 ASan 런타임을
# clang_rt.asan_dynamic만 제공하고 정적 라이브러리가 없다. /MTd와 섞으면
# 링크는 되는데 실행 중에 조용히 어긋난다(로그가 한 줄도 안 남고 트레이가 죽었다).
# 디버그 빌드는 배포하지 않으므로 DLL 의존이 생겨도 상관없다.
if(SWEEPCAP_ASAN_ACTIVE)
    set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreadedDebugDLL")
else()
    set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
endif()

# ASan은 /RTC1과 같이 못 쓴다. CMake의 Debug 기본 플래그에서 빼 둔다.
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
    src/tray.cpp
    res/sweepcap.rc
)

set_target_properties(sweepcap PROPERTIES OUTPUT_NAME "SweepCap")

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

# 매니페스트는 res/sweepcap.rc가 넣는다. 링커가 또 넣으면 CVT1100(중복 리소스)이 난다.
target_link_options(sweepcap PRIVATE /MANIFEST:NO)

target_link_options(sweepcap PRIVATE
    $<$<CONFIG:Release>:/DEBUG>
    $<$<CONFIG:Release>:/OPT:REF>
    $<$<CONFIG:Release>:/OPT:ICF>
)

# 전부 인박스 라이브러리다. 패키지 관리자 없이 빌드된다.
target_link_libraries(sweepcap PRIVATE
    user32      # 창, 메시지, 모니터
    shell32     # Shell_NotifyIcon, SHGetKnownFolderPath
    shcore      # GetDpiForMonitor
    dwmapi      # DWMWA_EXTENDED_FRAME_BOUNDS
    ole32       # CoTaskMemFree
    comctl32    # LoadIconMetric
    windowscodecs  # WIC PNG 인코딩
)

if(SWEEPCAP_ASAN_ACTIVE)
    target_compile_options(sweepcap PRIVATE /fsanitize=address)
    target_link_options(sweepcap PRIVATE /INCREMENTAL:NO)

    # ASan 런타임 DLL을 exe 옆에 둔다.
    # 정적 임포트라서 없으면 프로세스가 아예 안 뜨고 로더 오류 대화상자만 나온다.
    # /MDd라도 링커가 무엇을 고를지는 툴셋 판에 따라 갈리므로 두 벌 다 복사한다.
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
        message(STATUS "ASan 런타임 복사: ${SWEEPCAP_ASAN_DLLS}")
    else()
        message(WARNING
            "ASan 런타임 DLL을 찾지 못했다. 디버그 빌드가 실행되지 않는다.\n"
            "  vcvarsall을 거쳐 빌드했는지 확인하거나, -DSWEEPCAP_ASAN=OFF로 끈다.")
    endif()
endif()

# 아이콘은 tools/make_icon.ps1의 출력이다. 없으면 빌드 전에 만들라고 알려 준다.
if(NOT EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/res/sweepcap.ico")
    message(FATAL_ERROR
        "res/sweepcap.ico가 없다. tools/make_icon.ps1로 생성한다:\n"
        "  powershell -NoProfile -ExecutionPolicy Bypass -File tools/make_icon.ps1 res/sweepcap.ico")
endif()
