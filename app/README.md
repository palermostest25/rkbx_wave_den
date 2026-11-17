# Rekordbox Wave in C++

The rewrite of Rekordbox Wave in C++20 with Qt 6, the same toolkit the Python app uses, so it is a
faithful port rather than a redesign. It is built beside the Python app; the Python app keeps shipping
until this one does everything it does, and then releases switch to it.

## Stages

1. **Core** (done): Rekordbox's analysis files (`core/anlz`), the waveform pictures and their tile cache
   (`core/waveform`), playback timing (`core/timeline`) and the bridge client (`core/bridge`, reading
   the layout in `native/mac_bridge/bridge_layout.h` that both bridges share).
2. Deck screen, toolbar and settings.
3. Rekordbox host (start, live connect, restart), popups, touch browser, controller (MIDI), touch
   keyboard, keep-awake, single instance, logging, updater and installer; then the switch-over.

## Checked against the Python app

`tests/make_fixtures.py` writes a synthetic track's ANLZ files and what the Python app reads and draws
from them. `tests/core_tests.cpp` checks the C++ core gives the same values and the **same pixels**,
and runs the Python playback-timing tests, ported case by case.

## Build

```sh
cmake -S app -B app-build -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build app-build
QT_QPA_PLATFORM=offscreen ctest --test-dir app-build --output-on-failure
```

Needs Qt 6.4 or later. CI builds and tests it on Windows, macOS and Linux.
