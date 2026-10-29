cmake_minimum_required(VERSION 3.20)

project(TibiaExtended LANGUAGES CXX)

option(TIBIA_PUBLIC_RELEASE "Build the hardened public distribution" ON)

if(NOT MSVC)
    message(FATAL_ERROR "TibiaExtended requires the 32-bit MSVC toolchain")
endif()
if(NOT CMAKE_SIZEOF_VOID_P EQUAL 4)
    message(FATAL_ERROR "TibiaExtended must be configured for Win32/x86")
endif()

add_library(ddraw SHARED
    src/chat_input.cpp
    src/ddraw.def
    src/dllmain.cpp
    src/enhanced_dat.cpp
    src/extdx9.cpp
    src/extogl.cpp
    src/hook.cpp
    src/integrity_check.cpp
    src/mount_system.cpp
    src/network_redirect.cpp
    src/outfit_limit.cpp
    src/ping_overlay.cpp
    src/sprites.cpp
    src/store_window.cpp
    src/timer.cpp
    src/weather_system.cpp
)

target_compile_features(ddraw PRIVATE cxx_std_17)
target_compile_definitions(ddraw PRIVATE
    WIN32
    _WINDOWS
    _USRDLL
    _CRT_SECURE_NO_WARNINGS
    __INCLUDE_860_VERSION__
    __CONFIG__
    __MAGIC_EFFECTS_U16__
    __DISTANCE_SHOOT_U16__
    __PLAYER_HEALTH_U32__
    __PLAYER_MANA_U32__
    TIBIA_PUBLIC_RELEASE=$<IF:$<CONFIG:Debug>,0,$<BOOL:${TIBIA_PUBLIC_RELEASE}>>
)
target_compile_options(ddraw PRIVATE
    /W4
    /WX
    /sdl
    /permissive-
    /utf-8
    $<$<CONFIG:Debug>:/Od>
    $<$<CONFIG:Debug>:/Zi>
    $<$<CONFIG:Release>:/O2>
    $<$<CONFIG:Release>:/Ob2>
    $<$<CONFIG:Release>:/GL>
    $<$<CONFIG:Release>:/Gy>
    $<$<CONFIG:Release>:/Gw>
    $<$<CONFIG:Release>:/GS>
    $<$<NOT:$<CONFIG:Debug>>:/GR->
)
target_link_options(ddraw PRIVATE
    $<$<CONFIG:Debug>:/DEBUG:FULL>
    $<$<CONFIG:Release>:/LTCG>
    $<$<CONFIG:Release>:/OPT:REF>
    $<$<CONFIG:Release>:/OPT:ICF>
    $<$<CONFIG:Release>:/INCREMENTAL:NO>
    $<$<CONFIG:Release>:/DYNAMICBASE>
    $<$<CONFIG:Release>:/NXCOMPAT>
    $<$<CONFIG:Release>:/RELEASE>
)
target_link_libraries(ddraw PRIVATE bcrypt opengl32 ws2_32)

add_custom_command(TARGET ddraw POST_BUILD
    COMMAND powershell -NoProfile -ExecutionPolicy Bypass
        -File "${CMAKE_CURRENT_SOURCE_DIR}/scripts/stamp_integrity.ps1"
        -DllPath "$<TARGET_FILE:ddraw>"
        -Enabled "$<IF:$<AND:$<BOOL:${TIBIA_PUBLIC_RELEASE}>,$<NOT:$<CONFIG:Debug>>>,1,0>"
    VERBATIM
)

set_property(TARGET ddraw PROPERTY MSVC_RUNTIME_LIBRARY
    "MultiThreaded$<$<CONFIG:Debug>:Debug>")
set_target_properties(ddraw PROPERTIES OUTPUT_NAME ddraw)

# The custom ping state machine lives in PingOverlay::Detail inside
# ping_overlay.cpp. PING_OVERLAY_TRACKER_TEST compiles that file down to just
# those pure functions, with no hook, client memory or timer dependency, so the
# handshake can be exercised without running Tibia.
enable_testing()

add_executable(ping_tracker_tests
    src/ping_overlay.cpp
    tests/ping_tracker_tests.cpp
)
target_compile_features(ping_tracker_tests PRIVATE cxx_std_17)
target_compile_definitions(ping_tracker_tests PRIVATE
    WIN32
    _CRT_SECURE_NO_WARNINGS
    PING_OVERLAY_TRACKER_TEST
)
target_compile_options(ping_tracker_tests PRIVATE /W4 /WX /permissive- /utf-8)
set_property(TARGET ping_tracker_tests PROPERTY MSVC_RUNTIME_LIBRARY
    "MultiThreaded$<$<CONFIG:Debug>:Debug>")

add_test(NAME ping_tracker_tests COMMAND ping_tracker_tests)

# Exercise the real outfit/mount readers with native calls replaced by a
# synthetic packet buffer. Rendering/hook stubs abort if accidentally reached.
add_executable(mount_protocol_tests tests/mount_protocol_tests.cpp)
target_compile_features(mount_protocol_tests PRIVATE cxx_std_17)
target_compile_definitions(mount_protocol_tests PRIVATE
    WIN32
    __CONFIG__
    TIBIA_PUBLIC_RELEASE=1
    _CRT_SECURE_NO_WARNINGS
)
target_compile_options(mount_protocol_tests PRIVATE /W4 /WX /permissive- /utf-8)
set_property(TARGET mount_protocol_tests PROPERTY MSVC_RUNTIME_LIBRARY
    "MultiThreaded$<$<CONFIG:Debug>:Debug>")
add_test(NAME mount_protocol_tests COMMAND mount_protocol_tests)
