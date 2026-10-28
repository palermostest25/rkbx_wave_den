# Rekordbox Wave

A dedicated waveform monitor for a Rekordbox / DDJ-FLX10 setup. Keep browsing and deck controls in Rekordbox; show two or four scrolling waveforms on a second display.

## 1.2 preview — compatibility

| Platform | Viewer | Direct Rekordbox playback connection |
| --- | --- | --- |
| Windows x64 | Standalone `.exe` | Existing memory layouts for 7.2.2 and 7.2.14 |
| macOS Apple Silicon | Native `.app` | **Not implemented**; offline preview or external OSC sender only |
| macOS Intel | Native `.app` | **Not implemented**; offline preview or external OSC sender only |

This is a **prerelease**, not a completed Mac live integration. Streaming waveforms depend on accessible ANLZ analysis files. Saved loop boundaries are displayed; **currently engaged live loops are not yet available from the existing memory reader**. No controller-specific USB driver is required by the viewer. Live FLX10 timing, external-monitor hotplug, and Windows audio-session behavior need testing on the actual rig. Exact pixel fidelity to an XDJ-AZ is not claimed.

## Downloads

Use the assets on [GitHub Releases](https://github.com/palermostest25/rkbx_wave_den/releases). On Windows, launch `Rekordbox-Wave-Windows-x64.exe`. On Mac, unzip the archive for your processor and open `Rekordbox Wave.app`. Mac preview builds are ad-hoc signed, not notarized with an Apple Developer identity. Use macOS Privacy & Security's Open Anyway flow if Gatekeeper blocks the downloaded build.

## Display setup

- Select **2** or **4** decks. Four decks default to **3,1,2,4**; enter a permutation of 1–4 under **Row order**. Two-deck mode shows decks 1 and 2.
- Choose a **Display**, then **Fill display** for a borderless waveform screen. The app remembers the choice; disconnected displays fall back to the primary monitor and rebind on reconnection. **Refresh** updates the selector. Display names are supplied by the OS; identical monitors without unique names may need reselection.
- Use **Scale** for cue labels and deck text. Layout follows the available resolution; 1920 × 1080 is the main target.
- Use **Zoom**, or `+` / `−` while the waveform window has focus. Windows global zoom hotkeys continue to work while Rekordbox has focus. Global hotkeys on macOS are not implemented.
- **Escape** restores the waveform window border. Settings remain in a separate window.
- **Save / Load** manage named profiles. The current session is saved to the app-owned default on exit; named profiles are never overwritten automatically.

The default waveform is three-band: blue lows, amber mids, white highs and brown low/mid overlap. The first beat of a bar is red; other beats have quieter ticks. `RGB`, `Overview` and separate-band `Stack` remain optional modes. Beat colours, line width, smoothing and band gains remain configurable under **Tune**.

Hot-cue flags carry A–H; memory cues use M. An `L` suffix and a bracket below the signal identify a saved loop. Exported USB cues are read from DAT, EXT and 2EX together, preferring the richer PCO2 colours. Legacy RGB-only analysis is displayed in monochrome rather than treated as fabricated frequency bands. Cue edits made after loading currently require reloading the track. Local library cue colours fall back to slot colours; native database colour mapping is not yet implemented.

## Offline setup and demo

From source:

```sh
python -m pip install .
python rkbx_wave.py --demo
```

The demo uses explicitly labelled synthetic waveforms and never controls Rekordbox or audio. To inspect a real exported USB track, select **Offline**, choose its deck, then **Open ANLZ folder…** and choose the directory containing that track's DAT / EXT / 2EX files. Offline previews show a static playhead.

## OSC input

OSC is an alternative for an existing compatible sender, **not an automatic Mac connection to Rekordbox**. Choose **OSC** to listen on localhost UDP port 4460. Deck IDs are zero-based:

- `/track/0/anlz_path` — string path to an ANLZ folder (or its DAT file)
- `/time/0` — floating-point playback position in seconds
- `/bpm/0/current` — current BPM
- `/bpm/0/original` — original BPM

Repeat for decks 1–3. Sender paths must exist on the viewer's computer. Empty ANLZ paths clear the loaded deck. Rekordbox itself does not provide these OSC messages; a compatible sender must supply them.

## Build and verify

Use Python 3.9+ for source; automated native bundles use Python 3.12. Build on the target OS:

```sh
python -m pip install . pyinstaller
python -m unittest discover -s tests -v
python scripts/build_app.py
```

Windows produces a single-file executable; macOS produces an application bundle. GitHub Actions builds Windows x64, macOS Intel and macOS Apple Silicon separately. The source and release tag identify the same commit as the binaries. Screenshots can be reproduced with `python scripts/capture_demo.py --out work/screenshots`.

Settings: `%APPDATA%/rkbx_wave` on Windows; `~/Library/Application Support/rkbx_wave` on Mac.

## Credits and license

Fork of [mrmilbe/rkbx_wave](https://github.com/mrmilbe/rkbx_wave). Original copyright notices and MIT license are retained. ANLZ/database parsing uses [pyrekordbox](https://github.com/dylanljones/pyrekordbox). Format references: [Deep Symmetry's three-band analysis](https://djl-analysis.deepsymmetry.org/djl-analysis/track_metadata.html) and the [AlphaTheta XDJ-AZ manual](https://downloads.support.alphatheta.com/manuals/all-in-one-dj-systems/XDJ-AZ/html/en/000COV_en/Part_names/Part_names.htm).
