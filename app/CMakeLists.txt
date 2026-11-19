# Rekordbox Wave, in C++ with Qt 6. Built beside the Python app until it does everything that one does;
# then releases switch to it. See app/README.md.
cmake_minimum_required(VERSION 3.21)
project(RekordboxWave VERSION 3.0.12 LANGUAGES CXX)

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

find_package(Qt6 6.4 REQUIRED COMPONENTS Core Gui Widgets Network Concurrent Test)

# The core: Rekordbox's files and engine state, and the waveform pictures. No widgets.
add_library(wavecore STATIC
  src/core/anlz.cpp
  src/core/waveform.cpp
  src/core/timeline.cpp
  src/core/bridge.cpp
  src/core/settings.cpp
  src/core/library.cpp
  src/core/streamed.cpp
)
target_include_directories(wavecore PUBLIC src)
target_link_libraries(wavecore PUBLIC Qt6::Core Qt6::Gui)

# SQLCipher, for Rekordbox's encrypted libraries: its amalgamation, built in, with the platform's own
# crypto (CommonCrypto on macOS, CNG on Windows; OpenSSL on Linux, where Wave is only tested).
enable_language(C)
add_library(sqlcipher STATIC third_party/sqlcipher/sqlite3.c third_party/sqlcipher/crypto_cng.c)
target_include_directories(sqlcipher PUBLIC third_party/sqlcipher)
target_compile_definitions(sqlcipher PUBLIC SQLITE_HAS_CODEC SQLITE_TEMP_STORE=2 SQLITE_THREADSAFE=1
  SQLITE_OMIT_LOAD_EXTENSION SQLITE_OMIT_DEPRECATED SQLITE_DEFAULT_MEMSTATUS=0)
if(MSVC)
  target_compile_options(sqlcipher PRIVATE /W0)
else()
  target_compile_options(sqlcipher PRIVATE -w)
endif()
if(WIN32)
  target_compile_definitions(sqlcipher PRIVATE SQLCIPHER_CRYPTO_CUSTOM)
  target_link_libraries(sqlcipher PRIVATE bcrypt)
elseif(APPLE)
  target_compile_definitions(sqlcipher PRIVATE SQLCIPHER_CRYPTO_CC)
  target_link_libraries(sqlcipher PRIVATE "-framework Security" "-framework CoreFoundation")
else()
  find_package(OpenSSL REQUIRED)
  target_compile_definitions(sqlcipher PRIVATE SQLCIPHER_CRYPTO_OPENSSL)
  target_link_libraries(sqlcipher PRIVATE OpenSSL::Crypto)
  find_package(Threads REQUIRED)
  target_link_libraries(sqlcipher PRIVATE Threads::Threads ${CMAKE_DL_LIBS} m)
endif()
target_link_libraries(wavecore PUBLIC sqlcipher)

# Wave itself: Rekordbox host, reader, session and the windows.
file(GLOB WAVE_APP_SOURCES CONFIGURE_DEPENDS src/host/*.cpp src/app/*.cpp src/ui/*.cpp)
list(FILTER WAVE_APP_SOURCES EXCLUDE REGEX "/main\\.cpp$")
add_library(waveapp STATIC ${WAVE_APP_SOURCES})
target_link_libraries(waveapp PUBLIC wavecore Qt6::Widgets Qt6::Network Qt6::Concurrent)
target_compile_definitions(waveapp PUBLIC WAVE_VERSION="${PROJECT_VERSION}")
option(WAVE_PACKAGED "A release build: it updates and installs itself" OFF)
if(WAVE_PACKAGED)
  target_compile_definitions(waveapp PUBLIC WAVE_PACKAGED)
endif()
if(WIN32)
  target_link_libraries(waveapp PUBLIC user32 advapi32 shell32 ole32 oleaut32 uuid winmm dwmapi powrprof)
elseif(APPLE)
  target_link_libraries(waveapp PUBLIC "-framework ApplicationServices" "-framework CoreMIDI" "-framework CoreFoundation"
                                       "-framework IOKit")
endif()

# The bridge Wave puts inside Rekordbox, embedded: the Windows DLL as CI builds it (WAVE_BRIDGE_LIBRARY), the
# macOS dylib from the repository.
set(WAVE_BRIDGE_LIBRARY "" CACHE FILEPATH "The Rekordbox bridge library to embed")
if(NOT WAVE_BRIDGE_LIBRARY AND APPLE)
  set(WAVE_BRIDGE_LIBRARY "${CMAKE_CURRENT_SOURCE_DIR}/../rb_waveform_core/data/wave-bridge.dylib")
endif()
set(WAVE_RESOURCES resources/resources.qrc)
if(WAVE_BRIDGE_LIBRARY)
  get_filename_component(bridge_name "${WAVE_BRIDGE_LIBRARY}" NAME)
  string(REGEX REPLACE "^.*(\\.[a-z]+)$" "wave-bridge\\1" bridge_alias "${bridge_name}")
  file(WRITE "${CMAKE_CURRENT_BINARY_DIR}/bridge.qrc"
       "<RCC><qresource prefix=\"/bridge\"><file alias=\"${bridge_alias}\">${WAVE_BRIDGE_LIBRARY}</file></qresource></RCC>\n")
  list(APPEND WAVE_RESOURCES "${CMAKE_CURRENT_BINARY_DIR}/bridge.qrc")
else()
  message(STATUS "No Rekordbox bridge embedded (set WAVE_BRIDGE_LIBRARY): Wave can't start Rekordbox connected")
endif()

qt_add_executable(RekordboxWave WIN32 MACOSX_BUNDLE src/main.cpp ${WAVE_RESOURCES})
target_link_libraries(RekordboxWave PRIVATE waveapp)
set_target_properties(RekordboxWave PROPERTIES OUTPUT_NAME "Rekordbox Wave")
if(WIN32)
  configure_file(resources/wave.rc.in "${CMAKE_CURRENT_BINARY_DIR}/wave.rc" @ONLY)
  target_sources(RekordboxWave PRIVATE "${CMAKE_CURRENT_BINARY_DIR}/wave.rc")
  target_link_options(RekordboxWave PRIVATE /MANIFEST:NO)  # ours is in wave.rc
elseif(APPLE)
  set_source_files_properties(resources/icon.icns PROPERTIES MACOSX_PACKAGE_LOCATION Resources)
  target_sources(RekordboxWave PRIVATE resources/icon.icns)
  set_target_properties(RekordboxWave PROPERTIES
    MACOSX_BUNDLE_GUI_IDENTIFIER io.github.palermostest25.rekordbox-wave
    MACOSX_BUNDLE_BUNDLE_NAME "Rekordbox Wave"
    MACOSX_BUNDLE_BUNDLE_VERSION "${PROJECT_VERSION}"
    MACOSX_BUNDLE_SHORT_VERSION_STRING "${PROJECT_VERSION}"
    MACOSX_BUNDLE_ICON_FILE icon.icns
    MACOSX_BUNDLE_INFO_PLIST "${CMAKE_CURRENT_SOURCE_DIR}/resources/Info.plist.in")
endif()

enable_testing()
add_executable(core_tests tests/core_tests.cpp)
target_link_libraries(core_tests PRIVATE wavecore Qt6::Test)
target_compile_definitions(core_tests PRIVATE WAVE_TEST_DATA="${CMAKE_CURRENT_SOURCE_DIR}/tests/data")
add_test(NAME core_tests COMMAND core_tests)

add_executable(app_tests tests/app_tests.cpp)
target_link_libraries(app_tests PRIVATE waveapp Qt6::Test)
add_test(NAME app_tests COMMAND app_tests)
# Off screen where Qt has that platform; a static Windows Qt has only the desktop one, which CI's runner has.
if(NOT WIN32)
  set_tests_properties(app_tests PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=offscreen")
endif()
set_tests_properties(core_tests app_tests PROPERTIES TIMEOUT 300)
