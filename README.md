# Rekordbox Wave

A dedicated two- or four-deck waveform display for a separate monitor, with an AZ-style three-band canvas. Rekordbox retains browsing, audio and performance controls.

## Native Qt application

The 2.0 interface uses one Qt window. **SETTINGS** or **F2** opens its integrated settings page; there is no separate options application. Settings start at 22 logical pixels and have their own text-size control. Qt handles Windows/macOS display scaling.

Selecting a physical **Monitor** automatically enters full screen. It covers the complete panel, including the area normally occupied by the Windows taskbar. Windows uses a Per-Monitor-V2 DPI manifest and native monitor bounds. Full screen persists while settings are open. **Escape** returns from settings to waves; press it again to leave full screen. **F11** toggles full screen.

- **2 DECK / 4 DECK:** switch without rebuilding the window or discarding tracks. Default four-deck order is **3,1,2,4**. Use a preset or type any permutation of 1–4 into **Deck order**.
- **Waveform:** zoom in bars/beats, playhead placement, beat/bar markers, cue and loop visibility, colours and per-band levels.
- **Display:** monitor binding and separate settings/cue text scales. A disconnected monitor falls back to the primary; the selected monitor is restored when it returns.
- **Connection:** native playback, compatible OSC input, or offline ANLZ inspection.
- **Profiles:** import/export JSON. Changes are saved atomically to the app-owned settings file.

`+` / `−` zoom while focused. The existing Windows global numpad zoom keys also work while Rekordbox has focus. Double-click the canvas to open settings. The display has no controls that seek or change audio playback.

## Waveforms, cues and loops

PWV7 detail uses its actual mid/high/low byte ordering, fixed seven-bit heights and separate frequency-band scaling. Blue lows, amber mids, brown overlap and a small white high-frequency layer replace the old track-normalized rendering. The fixed white playhead is distinct from red first-beat ticks. Non-downbeats remain visible and bar numbers are drawn below the signal.

Hot-cue flags and memory cues retain exact track positions. Nearby labels use free positions rather than overwriting one another. Saved loop boundaries are bracketed below the waveform. USB cues are combined from DAT/EXT/2EX, preferring richer PCO2 colours. Native-resolution cached tiles retain detail on long tracks; reduced peak-preserving tiles are used when zoomed out. Local database loading supports current and older pyrekordbox API layouts.

## Playback compatibility

| Input | Available data |
| --- | --- |
| Windows native | Existing Rekordbox 7.2.2 / 7.2.14.0 playback-position, BPM and analysis-path layouts |
| Apple Silicon native | Community source layouts for 7.2.8 / 7.2.17 / 7.2.18; requires macOS permission to read the target process |
| Intel Mac | Qt viewer and OSC/offline input; no validated native memory layout |
| OSC | Position, BPM and optional authoritative playing/rate/live-loop state |

The original native Windows reader does **not** expose live engaged-loop fields. Saved loops are rendered; live-loop highlighting requires a sender that supplies live state. No saved loop is falsely presented as currently engaged. Unknown native versions fail with an explanatory connection status. Streaming analysis depends on accessible ANLZ files.

The installed vendor-signed Mac Rekordbox can deny process access. This application never modifies its signature, entitlements, security settings or library. A Mac binary alone does not remove that OS restriction. Native Mac 7.2.19 and Intel playback support are not claimed.

## Run and build

Use Python 3.10+; native build jobs use Python 3.12.

```sh
python -m pip install .
python rkbx_wave.py
python rkbx_wave.py --demo
```

The demo is explicitly labelled and uses synthetic analysis. For a real offline waveform, choose **Offline**, then select a deck and **OPEN ANALYSIS FILE** in Settings. Offline inspection has a static playhead.

```sh
python -m pip install . pyinstaller
python -m unittest discover -s tests -v
python scripts/build_app.py
```

Windows produces one `.exe`; Mac produces an `.app`. Native CI tests the integrated UI and full-screen bounds; an additional Windows pass uses 150% scale and checks ownership of the bottom monitor corners. `scripts/capture_demo.py --out work/captures` captures the actual Qt surface. `QT_QPA_PLATFORM=offscreen` allows layout-only captures on a headless system.

Settings are in `%APPDATA%/rkbx_wave` on Windows and `~/Library/Application Support/rkbx_wave` on Mac. `--config <file>` selects another profile location.

## OSC protocol

Choose **OSC** to listen on localhost UDP 4460. A separate compatible sender is required; Rekordbox does not supply this protocol itself. Deck IDs are zero-based:

- `/track/0/anlz_path`: local folder or DAT-file path; empty clears the deck.
- `/time/0`: playback seconds.
- `/bpm/0/current`, `/bpm/0/original`: BPM.
- `/playing/0`: 0 or 1; `/rate/0`: playback multiplier, including reverse.
- `/loop/0`: start seconds, end seconds, enabled (0 or 1).

## Credits and licences

Fork of [mrmilbe/rkbx_wave](https://github.com/mrmilbe/rkbx_wave), with original MIT notices retained. Uses [pyrekordbox](https://github.com/dylanljones/pyrekordbox), Qt/PySide6 under its LGPL terms, and community memory layouts from [rkbx_link](https://github.com/grufkork/rkbx_link). Waveform format/calibration references: [Deep Symmetry](https://djl-analysis.deepsymmetry.org/djl-analysis/track_metadata.html) and the [AlphaTheta XDJ-AZ manual](https://downloads.support.alphatheta.com/manuals/all-in-one-dj-systems/XDJ-AZ/html/en/000COV_en/Part_names/Part_names.htm). Qt sources and licence information are available at [Qt](https://www.qt.io/licensing/open-source-lgpl-obligations).
