# Dedicated waveform surface

User-confirmed target: a 15.6-inch secondary display at 1920 × 1080, using XDJ-AZ waveform visual language. Rekordbox retains browsing, audio and performance controls on the main display. This surface shows waveform lanes and compact track/BPM/cue information; settings stays in its separate native window.

Support two or four equal-height rows. Four decks default to 3, 1, 2, 4, with configurable order. Two decks preserve physical decks 1 and 2 and filter the chosen four-deck order. Display selection, fill-display mode and UI scale persist in configuration. Binding uses monitor identity rather than desktop position. macOS enumeration uses native NSScreen names and IDs, converting AppKit bottom-left coordinates to SDL top-left coordinates; the coordinate conversion has unit coverage. Other platforms use screeninfo monitor identities, with an SDL primary-display fallback if enumeration fails.

Acceptance evidence under `review/`: `demo-1920x1080-4deck.png`, `demo-1280x720-4deck.png`, `demo-1920x1080-2deck.png`, `settings.png`, `settings-error.png`. The final settings/error capture shows flat buttons, all deck status rows, and a missing-ANLZ-folder error without claiming a live connection. Demo captures use the actual SDL renderer with explicitly synthetic data; they verify composition and scaling, not native playback synchronization or hardware fidelity.

Still incomplete: direct macOS Rekordbox sync is not available; OSC requires a compatible external sender. The renderer shows saved loops but has no verified active-loop engagement state. Existing Windows memory readers are version-specific; their existence does not establish tested macOS support.
