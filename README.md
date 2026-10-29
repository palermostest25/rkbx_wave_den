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
| macOS native | Read-only Accessibility connection to the normal Rekordbox app; verified live with 7.2.19.0342 |
| Intel Mac | Same Accessibility connection; Intel bundle built natively, live hardware not yet verified |
| OSC | Position, BPM and optional authoritative playing/rate/live-loop state |

The original native Windows reader does **not** expose live engaged-loop fields. Saved loops are rendered; live-loop highlighting requires a sender that supplies live state. No saved loop is falsely presented as currently engaged. Unknown native versions fail with an explanatory connection status. Streaming analysis depends on accessible ANLZ files.

On Mac, allow **Rekordbox Wave** in **System Settings → Privacy & Security → Accessibility**, then select Native in Wave. Keep Rekordbox in PERFORMANCE mode with its deck elapsed-time fields available (Browse and 4-Deck Horizontal were checked). Wave reads titles/times and resolves their existing ANLZ files through an enforced read-only SQLCipher connection. It does not modify Rekordbox, launch a helper or create another Rekordbox copy.

Playback-time values have 0.1-second display precision; bounded interpolation smooths movement between updates. Title, artist and track duration must uniquely match the library. Current live verification includes streaming tracks with cached analysis. Direct USB tracks absent from the local collection may need their ANLZ file selected manually in Settings (Native playback remains connected); unresolved metadata is reported instead of guessing a different track.

The earlier debug-copy approach is disabled because detached vendor helpers could bypass its process sandbox. It is not used by the Accessibility reader.

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
