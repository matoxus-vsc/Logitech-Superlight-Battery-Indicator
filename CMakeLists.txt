cmake_minimum_required(VERSION 3.20)
cmake_policy(SET CMP0091 NEW)

project(SuperlightBattery LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")

add_executable(SuperlightBattery WIN32
    src/main.cpp
    src/config.cpp
    src/autostart.cpp
    src/hidpp_protocol.cpp
    src/hid_device.cpp
    src/battery_poller.cpp
    src/renderer.cpp
    src/taskbar_dock.cpp
    src/tray_icon.cpp
    src/widget_window.cpp
    src/app.rc
    src/app.manifest
)

target_compile_definitions(SuperlightBattery PRIVATE
    UNICODE
    _UNICODE
    WIN32_LEAN_AND_MEAN
    NOMINMAX
)

if(MSVC)
    target_compile_options(SuperlightBattery PRIVATE
        /W4
        /permissive-
        /utf-8
        $<$<CONFIG:Release>:/O2 /Os /GL /Gy /Gw /Zc:inline>
    )
    target_link_options(SuperlightBattery PRIVATE
        $<$<CONFIG:Release>:/LTCG /OPT:REF /OPT:ICF>
    )
endif()

target_link_libraries(SuperlightBattery PRIVATE
    hid
    setupapi
    d2d1
    dwrite
    shell32
    advapi32
)

add_executable(hidpp_tests
    tests/hidpp_tests.cpp
    src/hidpp_protocol.cpp
    src/config.cpp
    src/taskbar_dock.cpp
)

target_link_libraries(hidpp_tests PRIVATE
    shell32
    advapi32
)

target_compile_definitions(hidpp_tests PRIVATE
    UNICODE
    _UNICODE
    WIN32_LEAN_AND_MEAN
    NOMINMAX
)

if(MSVC)
    target_compile_options(hidpp_tests PRIVATE
        /W4
        /permissive-
        /utf-8
    )
endif()

enable_testing()
add_test(NAME hidpp_tests COMMAND hidpp_tests)
