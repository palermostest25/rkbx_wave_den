# Rekordbox Wave

## Platform
Desktop: Windows and macOS (Python, native Qt/PySide6).

## Users and purpose
A DJ using a DDJ-FLX10 initially wanted four enlarged waveforms on a separate 15.6-inch 1920 × 1080 display, with Rekordbox retaining browsing and deck controls on the main display. The confirmed next scope is a Stembox-inspired full-screen XDJ-AZ touch workflow on Windows Surface Pro 7 and macOS: browse, search, touch keyboard, deck loading and performance, while the FLX10 stays connected to Rekordbox.

## Confirmed requirements
- Two or four decks; default four-row order 3, 1, 2, 4, configurable.
- XDJ-AZ waveform visual language, true three-band rendering, accurate beat grid, cues and loops.
- Persistent monitor binding and scalable layout across display sizes.
- Mixed sources, primarily exported USB media.
- Preserve currently supported Rekordbox versions. Existing live reader is Windows-specific, versions 7.2.2 and 7.2.14; macOS live memory support is unvalidated.

## Boundaries
Rekordbox stays the audio engine and the authority for playback, cues and loops; the FLX10 stays connected to it. Wave's bridge (native/) reads Rekordbox 7.2.19's engine and browser and asks Rekordbox to load tracks and toggle key lock, never onto a playing deck. Library access is read-only. Wave never modifies Rekordbox's files and never claims support for a build it has not verified. The Windows bridge is built and verified against the binary but not yet tested on hardware.

## Revised acceptance requirements
One native window with integrated readable settings. Selecting a monitor fills the full panel, including Windows taskbar area. Use the existing project source and build/test workflow; further binary investigation was stopped at the user’s request.
