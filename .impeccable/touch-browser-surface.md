# Native touch collection browser

## Confirmed direction

Mode: Operate. Extend the existing native Qt window toward the user's Stembox-inspired, full-screen XDJ-AZ touch workflow on Windows Surface Pro 7 and macOS. The intended flow is browse → search with a touch keyboard → load a Rekordbox deck → perform, while the DDJ-FLX10 remains connected to Rekordbox. Scrolling and numeric BPM sorting are explicit requirements.

This document records a shippable browser increment, not acceptance of the complete touch DJ product. The original waveform-only surface brief is historical context. Preserve its established waveform language and the incumbent design files; this brief does not replace DESIGN.md or repair its schema/sidecar.

## Present visual system

Values below are extracted from `rb_waveform_core/qt_app.py` and `qt_browser.py`, in Qt logical pixels. They describe the present browser rather than new global tokens.

| Role | Present value |
| --- | --- |
| Shell / browser background | #000000 / #101010 |
| Table / alternate row | #111111 / #1B1B1B |
| Primary / browser status text | #EEEEEE / #BFC6CE |
| Table selection / selection text | #005B9A / #FFFFFF |
| Search fill / border / text selection | #202020 / #58616B / #006BB5 |
| Header fill / lower separator | #262626 / #555555 |
| Button fill / border | #101113 / #383C42 |
| Hover fill / focus accent | #202731 / #00A6FF |
| Disabled browser button text / border | #87909B / #2D333B |
| Typeface | Native Arial |
| Table and search text | 22 px |
| Header and browser status text | 18 px |
| Buttons / toolbar title / connection | 16 px / 24 px weight 600 / 14 px |

The surface is flat, dark and rectangular, using tonal rows and blue focus/selection feedback. It introduces no decorative cards or elevation.

## Present geometry

The shared toolbar stays above a stacked page: waveform, settings or browser. Its margins are 20/8/16/8 px (left/top/right/bottom). BROWSE changes to WAVEFORM while the browser is open. The connection status stays visible in the toolbar.

The browser uses 18 px horizontal and 16 px vertical margins with 12 px layout spacing. Source, search, KEYBOARD and REFRESH form the top row. Search and the keyboard toggle have a 48 px minimum widget height. The table stretches into the remaining space; rows are 54 px and the heading has a 52 px minimum height. Title and artist columns share available width; BPM, key and time use fixed widths of 110, 90 and 100 px. Long text elides. The vertical scrollbar is 24 px wide.

PREVIOUS, result status and NEXT sit below the table, followed by a wrapping read-only availability message. The keyboard occupies space below this message rather than covering the table: four number/letter rows and a final action row, 5 px gaps, 6/8/6/8 px margins, and 48 px minimum key heights. General button styling also contributes padding and borders; these minimums are not measured physical touch sizes.

## Implemented behavior

- Read-only local Collection and mounted OneLibrary USB source discovery; REFRESH retries discovery. Older USB exports may lack key metadata.
- Literal word search across title and artist, debounced by 180 ms. Search, queries and discovery use a single background worker, with stale results discarded.
- Title, artist, BPM, key and time headers sort the entire matching result set in either direction before pagination. BPM and duration sort their numeric database fields, not formatted text. Default sort is title ascending.
- Single-row selection, no inline editing, pixel scrolling by touch gesture or wheel, and PREVIOUS/NEXT pages of 200 results. Query changes clear selection; completed queries scroll to the top.
- Built-in number/letter keyboard with shift, punctuation, space, repeating delete, clear and done. Physical keyboard input remains available. Escape first hides the touch keyboard, then returns to the waveform page.
- Loading, empty, source failure and query failure messages are present. The persistent availability message explicitly says direct deck loading is not connected.

Browsing does not load a Rekordbox deck or substitute an offline analysis file for a load acknowledgement. Controller MIDI retains its existing display-zoom role; it does not create playback, cue edits or engaged-loop state. Native authoritative loop start/end/engagement remains unimplemented, and saved loops must not be shown as engaged on the basis of controller presses or inferred jumps.

## Verified viewport scope

Private review captures are under `.impeccable/review/touch-browser/`: `portable.png` and `portable-keyboard.png` at 1920 × 1080, and `surface-scaled.png` and `surface-scaled-keyboard.png` at 1368 × 912. The latter is a Surface effective scaled viewport, not a physical Surface capture. These are native Qt offscreen macOS layout checks, with keyboard open/closed. They establish composition at those sizes only. Their DEMO status labels synthetic waveform analysis; the browser query used the real local collection.

The review exercised a 4,289-track local collection, a real BPM-header click producing ascending numeric results with 200 rows, and the title/artist query `Swimming Paul` returning 54 results. This supports browser query and layout behavior, not live loading or a complete performance flow. Screenshots are private review evidence, not public artifacts.

## Outstanding acceptance

- Implement native deck-load commands with Rekordbox acknowledgement and visible failure/pending state before accepting browse → load → perform.
- Complete and verify hardware Browse navigation, USB playlist navigation and the requested performance controls.
- Obtain authoritative Rekordbox engaged-loop bounds and engagement. Show cue edits only after Rekordbox confirms them; do not infer these from FLX10 input.
- Validate actual touch scrolling, hit targets, keyboard usability, fullscreen/taskbar coverage and scaling on physical Surface Pro 7. The offscreen 1368 × 912 check does not establish Windows hardware support.
- Exercise the end-to-end Windows/macOS workflow with Rekordbox and the FLX10 connected, including load failures, source removal, live synchronization and supported versions. No such acceptance is established by this browser increment.
