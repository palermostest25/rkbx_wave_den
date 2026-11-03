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
The initial release boundary was a playback display, with audio, browsing and performance controls staying in Rekordbox. The next touch workflow is planned and incomplete: the current increment adds read-only browsing, search, touch keyboard, scrolling and global numeric BPM sorting; it has no direct deck-load backend or full performance controls. Rekordbox remains the authority for playback, cues and engaged loops; controller input must not stand in for confirmed native state. Native authoritative engaged-loop bounds remain unavailable. Streaming analysis availability and platform/version compatibility require explicit validation; do not claim compatibility without testing.

## Revised acceptance requirements
One native window with integrated readable settings. Selecting a monitor fills the full panel, including Windows taskbar area. Use the existing project source and build/test workflow; further binary investigation was stopped at the user’s request.
