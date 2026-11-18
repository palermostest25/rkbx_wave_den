# Rekordbox Wave in C++

Wave itself since 3.0: C++20 with Qt 6 Widgets, a faithful port of the Python app it replaced
(`rb_waveform_core/`, kept with its tests for reference), with the same settings file and the same
releases, so the Python app's updater moved its users onto it.

## Layout

- `src/core`: no widgets. Rekordbox's analysis files (`anlz`), the waveform pictures and their tile
  cache (`waveform`), playback timing (`timeline`), the bridge client (`bridge`, reading the layout in
  `native/mac_bridge/bridge_layout.h` that both bridges share), the settings file (`settings`) and
  Rekordbox's encrypted libraries, read-only (`library`).
- `src/host`: Rekordbox on this computer: finding, starting, live-connecting and quitting it, and its
  policy (`rekordbox`, `host`), its windows and UI Automation (`windows`), its popups (`popups`), the
  controller's MIDI read beside it (`midi`) and installing the supported version (`setup`).
- `src/app`: the bridge reader thread (`reader`), what the screen shows (`session`), the updater,
  the portable exe's INSTALL (`install`), and gig essentials: log, single instance, keep awake, battery,
  Windows' touch keyboard (`gig`).
- `src/ui`: the deck screen (`canvas`), the touch browser (`browser`), Settings (`settings_page`),
  the window (`mainwindow`) and the shared controls and stylesheet (`widgets`).
- `third_party/sqlcipher`: SQLCipher 4.5.6's amalgamation, with a two-line patch for a custom crypto
  provider, and `crypto_cng.c`, that provider on Windows' own CNG. macOS uses CommonCrypto; Linux
  (tests only) OpenSSL.

## Cool and light

The deck screen draws only while something moves: a playing deck, a preview gliding back, a long title
scrolling. Idle, it draws nothing. Playing, the lanes are blits of tiles rendered once and scaled once,
at the display's rate (paced to the compositor on Windows); the info bar redraws 20 times a second. The
bridge reader sends the screen a frame only when Rekordbox reports something new.

## Checked against the Python app

`tests/make_fixtures.py` writes a synthetic track's ANLZ files and what the Python app reads and draws
from them; `tests/core_tests.cpp` checks the same values and the **same pixels**, runs the Python
playback-timing tests, the settings and the library (on an encrypted fixture, and on one encrypted with
OpenSSL so every platform's crypto is checked against another). `tests/app_tests.cpp` ports the host
policy and popup tests and drives the browser and the window.

## Build

```sh
cmake -S app -B app-build -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build app-build
QT_QPA_PLATFORM=offscreen ctest --test-dir app-build --output-on-failure
"app-build/Rekordbox Wave" --demo --page settings
```

Needs Qt 6.4 or later. `-DWAVE_PACKAGED=ON` makes a release build (it updates and installs itself);
`-DWAVE_BRIDGE_LIBRARY=<dll>` embeds the Windows bridge (macOS embeds the repository's dylib). The
version is `project(... VERSION x.y.z)` in `CMakeLists.txt`.
