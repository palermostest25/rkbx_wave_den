# Rekordbox Wave, in C++ with Qt 6. Built beside the Python app until it does everything that one does;
# then releases switch to it. See app/README.md.
cmake_minimum_required(VERSION 3.21)
project(RekordboxWave VERSION 3.0.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_AUTOMOC ON)
set(CMAKE_AUTORCC ON)
if(VCPKG_TARGET_TRIPLET MATCHES "windows-wave")
  set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded")  # the static CRT, as the static Qt is built with
endif()
if(MSVC)
  add_compile_options(/W4 /permissive- /utf-8)
  add_compile_definitions(_USE_MATH_DEFINES NOMINMAX)
else()
  add_compile_options(-Wall -Wextra -Wpedantic)
endif()

find_package(Qt6 6.4 REQUIRED COMPONENTS Core Gui Widgets Test)

# The core: Rekordbox's files and engine state, and the waveform pictures. No widgets.
add_library(wavecore STATIC
  src/core/anlz.cpp
  src/core/waveform.cpp
  src/core/timeline.cpp
  src/core/bridge.cpp
  src/core/settings.cpp
  src/core/library.cpp
)
target_include_directories(wavecore PUBLIC src)
target_link_libraries(wavecore PUBLIC Qt6::Core Qt6::Gui)

# SQLCipher, for Rekordbox's encrypted libraries: vcpkg's (Windows, macOS releases), else pkg-config's (apt).
find_package(unofficial-sqlcipher CONFIG QUIET)
if(unofficial-sqlcipher_FOUND)
  target_link_libraries(wavecore PUBLIC unofficial::sqlcipher::sqlcipher)
else()
  find_package(PkgConfig REQUIRED)
  pkg_check_modules(SQLCIPHER REQUIRED IMPORTED_TARGET sqlcipher)
  target_link_libraries(wavecore PUBLIC PkgConfig::SQLCIPHER)
endif()
target_compile_definitions(wavecore PUBLIC SQLITE_HAS_CODEC)

enable_testing()
add_executable(core_tests tests/core_tests.cpp)
target_link_libraries(core_tests PRIVATE wavecore Qt6::Test)
target_compile_definitions(core_tests PRIVATE WAVE_TEST_DATA="${CMAKE_CURRENT_SOURCE_DIR}/tests/data")
add_test(NAME core_tests COMMAND core_tests)
