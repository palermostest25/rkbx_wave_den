# Rekordbox Wave, in C++ with Qt 6. Built beside the Python app until it does everything that one does;
# then releases switch to it. See app/README.md.
cmake_minimum_required(VERSION 3.21)
project(RekordboxWave VERSION 3.0.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_AUTOMOC ON)
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
)
target_include_directories(wavecore PUBLIC src)
target_link_libraries(wavecore PUBLIC Qt6::Core Qt6::Gui)

enable_testing()
add_executable(core_tests tests/core_tests.cpp)
target_link_libraries(core_tests PRIVATE wavecore Qt6::Test)
target_compile_definitions(core_tests PRIVATE WAVE_TEST_DATA="${CMAKE_CURRENT_SOURCE_DIR}/tests/data")
add_test(NAME core_tests COMMAND core_tests)
