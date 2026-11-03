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

### Platter, monitor and colours

- **Platters:** when you scratch, jog-cue or move a deck by hand, Wave stops predicting and glides between real positions, so the waveform never runs ahead and snaps back. Normal playback still runs predicted and smooth. On macOS rekordbox shows time in tenths of a second. For tiny moves, Wave uses the edge of the tenth the platter just crossed, so a small wiggle no longer jitters. Controller PLAY/CUE presses do not determine transport state.
- **Monitor:** full screen on a monitor is remembered. Whenever that monitor is connected, at launch or when plugged in, Wave fills it; otherwise Wave stays a normal window. F11/Esc leave full screen for now, and plugging the monitor in again returns to it.
- **Zoom:** pinch on a trackpad, scroll with two fingers or a mouse wheel over the waveforms, or turn **SHIFT + BROWSE** on the controller. That is rekordbox's own waveform zoom, so plain BROWSE still only browses tracks. +/− keys and the Settings menu step through fixed sizes (1 to 128 bars).
- **Waveform style:** Settings → Waveform → *Rekordbox* or *Wave classic* (the earlier look). The Rekordbox palette comes from flat-colour pixels measured on rekordbox 7's own decks: lows `#0054E2`, mids `#FFA600`, mids inside the bass `#B46607`, highs `#F6ECD9`. The lows and mids match a published rekordbox-style Mixxx skin (`#0055E1`, `#FFA600`). Bands are read and scaled the way rekordbox 7 draws them. I measured them against rekordbox's own deck drawing of a track: each stored PWV7 column (low, mid, high) correlates r = 0.89–0.95 with one band colour. All three bands share one linear scale that fills the lane at a raw value of about 100, with a thin highs line kept through the centre.

### Live hot cues and memory cues

Cue edits appear after they are read back from Rekordbox’s library. Wave polls loaded tracks’ cues several times a second using a read-only connection. Controller presses do not add or delete markers speculatively; Rekordbox must save the change before Wave shows it.

### Authoritative live state

Rekordbox is the source of playback position and BPM. Wave interpolates between observations for smooth rendering, but does not treat controller buttons or repeated backward jumps as confirmation of a loop. Native readers currently do **not** expose authoritative engaged-loop bounds. Saved loops remain visible; live loop shading requires a reader that supplies actual loop start, end and engagement (the existing OSC protocol accepts these fields, but requires a separate compatible sender).

Controller MIDI remains a display input for SHIFT + BROWSE zoom. Wave sends no MIDI commands to Rekordbox or the controller.

### Touch collection browser (development branch)

**BROWSE** opens a read-only collection browser in the same window. Search title/artist with the physical or built-in touch keyboard. Tap TITLE, ARTIST, BPM, KEY or TIME to sort the entire result set, ascending/descending. Scroll rows by touch or wheel; PREVIOUS/NEXT navigate 200-track pages. REFRESH discovers mounted OneLibrary USB exports. Search, sorting and source discovery run in a worker so they do not block waveform painting. Older USB schemas without key metadata show an empty key.

This is an incomplete step toward the requested Stembox-style workflow: **direct deck loading is not implemented**. Browsing does not load an analysis file and pretend a Rekordbox deck has loaded. Native command/acknowledgement integration, hardware Browse navigation, USB playlists, full performance controls and physical Surface Pro 7 acceptance remain outstanding. No release of this branch should be described as the completed touch DJ product.

No saved loop is falsely presented as currently engaged. Unknown native versions fail with an explanatory connection status. Streaming analysis depends on accessible ANLZ files.

On Mac, allow **Rekordbox Wave** in **System Settings → Privacy & Security → Accessibility**, then select Native in Wave. Keep Rekordbox in PERFORMANCE mode with its deck elapsed-time fields available (Browse and 4-Deck Horizontal were checked). Wave reads titles/times and resolves their existing ANLZ files through an enforced read-only SQLCipher connection. It does not modify Rekordbox, launch a helper or create another Rekordbox copy.

Playback-time values have 0.1-second display precision; bounded interpolation smooths movement between updates. Tracks are matched by title and artist, then by the deck's BPM and length, so streaming versions that share a title (Beatport drops "(Original Mix)") resolve to the version on the deck. Copies of one recording resolve to the one you prepared (analysed, most cues, most played). A track missing from the collection is looked up on any mounted rekordbox USB stick's export library. An unmatched track is retried every 2 seconds while a stream finishes analysing. On Windows the analysis path is read from rekordbox itself, so no matching is needed. Current live verification includes streaming tracks with cached analysis. Direct USB tracks absent from the local collection may need their ANLZ file selected manually in Settings (Native playback remains connected); unresolved metadata is reported instead of guessing a different track.

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
