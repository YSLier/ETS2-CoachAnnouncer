cmake_minimum_required(VERSION 3.21)
project(CoachAnnouncer VERSION 0.7.3 LANGUAGES CXX)
include(CTest)
find_package(Threads REQUIRED)

if(NOT CMAKE_SIZEOF_VOID_P EQUAL 8)
    message(FATAL_ERROR "Build CoachAnnouncer for x64.")
endif()

# The native Linux module is for the SDK simulation, not a release target.
if(NOT WIN32 AND NOT (CMAKE_SYSTEM_NAME STREQUAL "Linux" AND BUILD_TESTING))
    message(FATAL_ERROR "The release DLL targets Windows x64.")
endif()

if(MSVC)
    set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
endif()

add_library(CoachAnnouncer SHARED src/plugin.cpp src/capture.cpp src/journal.cpp src/route.cpp src/automatic.cpp src/station_locations.cpp src/announcer.cpp src/audio_clip.cpp src/panel_status.cpp src/settings.cpp)
target_include_directories(CoachAnnouncer SYSTEM PRIVATE third_party/scs_sdk/include)
target_compile_features(CoachAnnouncer PRIVATE cxx_std_20)
target_compile_definitions(CoachAnnouncer PRIVATE COACH_PROBE_VERSION="${PROJECT_VERSION}")
target_link_libraries(CoachAnnouncer PRIVATE Threads::Threads)
set_target_properties(CoachAnnouncer PROPERTIES
    PREFIX ""
    CXX_EXTENSIONS OFF
    CXX_VISIBILITY_PRESET hidden
    VISIBILITY_INLINES_HIDDEN ON
)

if(WIN32)
    target_sources(CoachAnnouncer PRIVATE src/CoachAnnouncer.def)
    target_link_libraries(CoachAnnouncer PRIVATE shell32 ole32 uuid user32 winmm)
    if(MINGW)
        target_link_options(CoachAnnouncer PRIVATE -static)
    endif()
endif()

if(MSVC)
    target_compile_options(CoachAnnouncer PRIVATE /W4 /permissive- /utf-8)
else()
    target_compile_options(CoachAnnouncer PRIVATE -Wall -Wextra -Wpedantic)
endif()

option(COACH_BUILD_SPF_PANEL "Build the separate SPF in-game panel" ON)
if(COACH_BUILD_SPF_PANEL)
    add_library(CoachAnnouncerPanel SHARED panel/plugin.cpp)
    target_include_directories(CoachAnnouncerPanel PRIVATE src)
    target_include_directories(CoachAnnouncerPanel SYSTEM PRIVATE third_party/spf)
    target_compile_features(CoachAnnouncerPanel PRIVATE cxx_std_20)
    target_compile_definitions(CoachAnnouncerPanel PRIVATE COACH_PANEL_VERSION="${PROJECT_VERSION}")
    target_link_libraries(CoachAnnouncerPanel PRIVATE ${CMAKE_DL_LIBS})
    set_target_properties(CoachAnnouncerPanel PROPERTIES
        PREFIX "" CXX_EXTENSIONS OFF CXX_VISIBILITY_PRESET hidden)
    if(MSVC)
        target_compile_options(CoachAnnouncerPanel PRIVATE /W4 /permissive- /utf-8)
    else()
        target_compile_options(CoachAnnouncerPanel PRIVATE -Wall -Wextra -Wpedantic)
    endif()
    if(MINGW)
        target_link_options(CoachAnnouncerPanel PRIVATE -static)
    endif()
endif()

if(BUILD_TESTING)
    find_package(Python3 REQUIRED COMPONENTS Interpreter)
    add_executable(sdk_host tests/sdk_host.cpp)
    target_include_directories(sdk_host SYSTEM PRIVATE third_party/scs_sdk/include)
    target_compile_features(sdk_host PRIVATE cxx_std_20)
    if(MSVC)
        target_compile_options(sdk_host PRIVATE /utf-8)
    endif()
    if(MINGW)
        target_link_options(sdk_host PRIVATE -static -municode)
    endif()
    target_link_libraries(sdk_host PRIVATE Threads::Threads ${CMAKE_DL_LIBS})
    add_test(NAME sdk_callback_simulation
        COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/tests/verify.py"
            "$<TARGET_FILE:sdk_host>" "$<TARGET_FILE:CoachAnnouncer>")
    add_executable(route_host tests/route_host.cpp src/capture.cpp src/route.cpp)
    target_include_directories(route_host PRIVATE src)
    target_include_directories(route_host SYSTEM PRIVATE third_party/scs_sdk/include)
    target_compile_features(route_host PRIVATE cxx_std_20)
    if(MINGW)
        target_link_options(route_host PRIVATE -static)
    endif()
    add_test(NAME announcement_route COMMAND route_host)
    add_executable(audio_host tests/audio_host.cpp src/audio_clip.cpp)
    target_include_directories(audio_host PRIVATE src)
    target_include_directories(audio_host SYSTEM PRIVATE third_party/scs_sdk/include)
    target_compile_features(audio_host PRIVATE cxx_std_20)
    add_test(NAME announcement_audio
        COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/tests/audio_fixture.py"
                "$<TARGET_FILE:audio_host>" "${CMAKE_CURRENT_SOURCE_DIR}/audio")
    add_executable(automatic_host tests/automatic_host.cpp src/capture.cpp src/route.cpp src/automatic.cpp src/station_locations.cpp)
    target_include_directories(automatic_host PRIVATE src)
    target_include_directories(automatic_host SYSTEM PRIVATE third_party/scs_sdk/include)
    target_compile_features(automatic_host PRIVATE cxx_std_20)
    add_test(NAME automatic_trigger
        COMMAND "${Python3_EXECUTABLE}" -c
            "import subprocess, sys, tempfile; d = tempfile.TemporaryDirectory(); sys.exit(subprocess.call([sys.argv[1], d.name]))"
            "$<TARGET_FILE:automatic_host>")

    if(COACH_BUILD_SPF_PANEL AND CMAKE_SYSTEM_NAME STREQUAL "Linux")
        add_executable(panel_host tests/panel_host.cpp)
        target_include_directories(panel_host PRIVATE src)
        target_include_directories(panel_host SYSTEM PRIVATE third_party/spf third_party/scs_sdk/include)
        target_compile_features(panel_host PRIVATE cxx_std_20)
        target_link_libraries(panel_host PRIVATE Threads::Threads ${CMAKE_DL_LIBS})
        add_test(NAME spf_panel_bridge
            COMMAND "${Python3_EXECUTABLE}" -c
                "import os, subprocess, sys, tempfile; d = tempfile.TemporaryDirectory(); sys.exit(subprocess.call(sys.argv[1:], env=dict(os.environ, COACH_PROBE_TEST_DIR=os.path.join(d.name, 'logs'))))"
                "$<TARGET_FILE:panel_host>" "$<TARGET_FILE:CoachAnnouncer>" "$<TARGET_FILE:CoachAnnouncerPanel>")
    endif()
endif()
