cmake_minimum_required(VERSION 3.20)
project(SimpleWallpaperChanger LANGUAGES CXX RC)

if(NOT WIN32)
    message(FATAL_ERROR "Simple Wallpaper Changer requires Windows.")
endif()

include(CTest)

add_executable(simple-wallpaper-changer WIN32 src/main.cpp src/tray_icon.rc)
target_compile_features(simple-wallpaper-changer PRIVATE cxx_std_17)
target_compile_definitions(simple-wallpaper-changer PRIVATE UNICODE _UNICODE WIN32_LEAN_AND_MEAN NOMINMAX)
target_link_libraries(simple-wallpaper-changer PRIVATE ole32 windowscodecs shlwapi user32 shell32 uuid comctl32 advapi32 comdlg32)

if(BUILD_TESTING)
    add_executable(swc-core-tests tests/core_tests.cpp)
    target_compile_features(swc-core-tests PRIVATE cxx_std_17)
    add_test(NAME swc-core-tests COMMAND swc-core-tests)
endif()
