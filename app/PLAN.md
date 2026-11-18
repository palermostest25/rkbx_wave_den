# C++ rewrite: plan and status

Goal: replace the Python app entirely, ship it through the same releases (same asset names, same settings
file), so the Python app's updater moves users onto it. Windows first; macOS kept working.

## Decisions
- C++20, Qt 6 Widgets (same toolkit as the Python app). QPainter on cached tile pixmaps.
- **Low heat:** repaint only while something moves (a deck playing or scrubbing, a preview gliding, an
  animation); idle = no frames. Timers coarse when idle.
- Settings: same folder and JSON (`%APPDATA%\rkbx_wave\default_config.json`, `~/Library/Application
  Support/rkbx_wave/default_config.json`), same keys, so a DJ's setup carries over.
- Windows: one static portable exe (Qt + SQLCipher static via vcpkg, cached in CI) plus the Inno installer.
  The portable exe offers INSTALL (Program Files + shortcuts, elevated).
- macOS: .app via macdeployqt in a DMG, bundle id `io.github.palermostest25.rekordbox-wave`, ad-hoc signed
  (the Python updater checks the identifier).
- Dropped: legacy readers for Rekordbox 7.2.2/7.2.14 (memory) and the macOS Accessibility deck reader
  (the bridge does this now), OSC input, offline analysis files, unused render modes.
- New: keyboard setting (Wave's keyboard / Windows touch keyboard); Install button in the portable exe.

## Stages
- [x] 1. Core: ANLZ, waveform tiles, timeline, bridge client; tests vs Python.
- [ ] 2. Settings model (JSON compatible), library (SQLCipher master.db + exportLibrary.db), catalog
       queries, cues from master.db, USB sticks.
- [ ] 3. Reader thread (bridge → decks), session (analysis loading, cue polling, loads, previews, zoom).
- [ ] 4. UI: theme, toolbar, status pill, banner, dimmer, deck canvas + info bar, full screen / monitor.
- [ ] 5. Settings UI (all pages).
- [ ] 6. Touch browser (sources, categories, tracks, artwork, breadcrumbs, search, keyboards, load bar).
- [ ] 7. Host: Rekordbox start/attach/restart/quit, popups (UIA/AX), Rekordbox setup/install, touch
       keyboard guard, keep awake, single instance, logging, MIDI (WinMM/CoreMIDI, Rekordbox maps).
- [ ] 8. Updater, portable INSTALL, installer, CI packaging (static Windows), smoke tests.
- [ ] 9. Switch releases to the C++ app; README.
