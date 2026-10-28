---
name: Rekordbox Wave
description: XDJ-AZ-informed waveform visibility for a dedicated DJ display
colors:
  wave-bg: "#050505"
  separator: "#293039"
  text-primary: "#EDF1F5"
  text-secondary: "#A6B1BD"
  playhead: "#FF0000"
  downbeat: "#FF3C3C"
  beat: "#EBEBEB"
  low: "#0078FF"
  mid: "#D49B46"
  overlap: "#96613B"
  high: "#FFFFFF"
  memory: "#FFBE00"
  cue-a: "#E62828"
  cue-b: "#32C8D7"
  cue-c: "#2D7D2D"
  cue-d: "#A03CCD"
  cue-e: "#46E646"
  cue-f: "#FF9114"
  cue-g: "#2346CD"
  cue-h: "#F0DC2D"
  cue-fallback: "#00C882"
  flag-ink: "#07090C"
  settings-bg: "#14191F"
  field-bg: "#202730"
  button-bg: "#26313D"
  button-active: "#344151"
  tuning-bg: "#1A1A1A"
  tuning-button: "#333333"
typography:
  body:
    fontFamily: "Helvetica, Arial, sans-serif"
    fontSize: "11px–24px"
    fontWeight: 400
  label:
    fontFamily: "Helvetica, Arial, sans-serif"
    fontSize: "10px–22px"
    fontWeight: 700
rounded:
  signal: "0px"
spacing:
  rule: "1px"
  marker: "2px"
  signal-margin: "4px"
  header-inset: "6px"
  control-gap: "8px"
  deck-inset: "12px"
components:
  waveform-lane:
    backgroundColor: "{colors.wave-bg}"
    textColor: "{colors.text-primary}"
    rounded: "{rounded.signal}"
  lane-title:
    textColor: "{colors.text-secondary}"
    typography: "{typography.body}"
  cue-flag:
    typography: "{typography.label}"
    rounded: "{rounded.signal}"
  settings-button:
    backgroundColor: "{colors.button-bg}"
    textColor: "{colors.text-primary}"
    padding: "5px 8px"
  settings-button-active:
    backgroundColor: "{colors.button-active}"
  settings-field:
    backgroundColor: "{colors.field-bg}"
    textColor: "{colors.text-primary}"
---

# Design System: Rekordbox Wave

## Overview

**Creative North Star: "The Performance Rail"**

The waveform display acts like an instrument: quiet near-black structure makes frequency bands, timing and cue positions immediately legible. The user selected the XDJ-AZ as the visual reference. This documents the implemented Python/Tk/SDL system, not a claim of pixel-identical hardware reproduction.

Density comes from aligned signal and marker rails, with native controls in a separate settings window. The provisional draft is refreshed from `rb_waveform_core/config.py`, `render.py`, `rb_waveform_gpu.py`, `cues.py`, and `rkbx_wave.py`; the source now takes precedence over its earlier proposed palette. Native fonts and widget chrome may vary by platform.

**Key Characteristics:**
- Signal first, with restrained structural chrome.
- Consistent timing geometry across equal-height deck lanes.
- Frequency and cue colors carry meaning.
- Playback drives motion; controls remain native and separate.

## Colors

The palette combines electric low-frequency blue, warm mid-frequency amber and white highs against near-black, with saturated timing and cue markers.

### Primary
- **Low blue**, **Mid amber**, **High white**: the three measured signal bands. Symmetric mode adds **Overlap brown** where low and mid coincide outside the high band.
- **Playhead red**: the fixed two-pixel timing rail. **Downbeat coral**: measure starts. **Beat silver**: quieter intermediate ticks.

### Secondary
- **Cue A–H**: fixed slot colors from `cues.py`; unknown hot-cue slots use **Cue fallback**. **Memory amber** distinguishes memory cues.
- Saved-loop brackets inherit their cue color. There is no universal saved-loop gold token and no active-loop color state implemented.

### Neutral
- **Wave background** and **Separator**: the signal canvas and thin lane boundaries.
- **Text primary** and **Text secondary**: deck/BPM information and track/waiting labels.
- **Flag ink**: dark text on sufficiently bright cue colors; dim flags use high white.
- **Settings background**, **Field background**, **Button background**, **Button active**, **Tuning background**, **Tuning button**: native Tk control surfaces.

**The Semantic Color Rule.** Frequency, cue and timing colors identify data; do not reuse them as decorative accents.

The tokens above reflect constructor/config defaults. Three-band and RGB palettes are user configurable. Tune exposes separate Low, Mid and High color pickers for the three-band palette. Alternative RGB mode blends its separate low/mid/high RGB settings; it does not redefine the three-band tokens. The review screenshots use synthetic cue colors, which can differ from production slot defaults.

## Typography

SDL labels request Helvetica then Arial through pygame's system font resolver. The machine-readable body and label ranges describe the actual bounds; they are not CSS clamp expressions. Native settings use Tk's platform font defaults, with explicit TkDefaultFont bold headings in the tuning panel.

The body size is `clamp(11, round(lane_height × 0.065 × ui_scale), 24)` in physical rendering pixels. Cue lettering is bold at `max(10, body_size − 2)`. At four decks on the target display this gives body (18px), cue (16px); at four decks on the smaller review size, body (12px), cue (10px). Two decks reach body (24px), cue (22px) at the default scale.

Track titles and waiting messages use the secondary tone. Deck identifiers and BPM use the primary tone. BPM is formatted to two decimals; a dedicated tabular-numeral font is not implemented. Titles clip before the reserved BPM region instead of overlapping it.

## Layout

The SDL surface divides available height equally by deck count, with any remainder assigned to the last row. It is continuously resizable; no web breakpoint or fixed 1080p canvas is implemented. A one-pixel boundary separates lanes. The left identity rail is `max(38, int(body_size × 2.5))`; headers are `body_size + 14` high. Deck identifiers are inset by the deck-inset token. The waveform occupies the remaining width and height, reserving upper cue and lower loop rails.

The user multiplier is clamped to (0.5–2.0) in configuration. Settings offers (0.75, 1.0, 1.25, 1.5, 2.0). The fixed playhead sits halfway across the signal viewport, so its screen position includes the identity rail rather than being exactly half the total window width. Timing overlays use track seconds and the same viewport transform as the signal.

The dedicated secondary-display composition, target resolution, row order, monitor behavior and acceptance captures are recorded in `.impeccable/waveform-surface.md`. Keep future surface-specific arrangements there.

## Elevation & Depth

The waveform renderer has no shadows, gradients, floating cards or decorative elevation. Near-black signal space and thin separators provide structure. Settings uses the Tk clam theme, whose native borders, focus and active states are platform dependent; avoid treating screenshot chrome as portable pixel tokens.

## Shapes

Signal geometry is rectangular: square cue flags, straight stems, flat loop brackets and thin timing ticks. The center playhead is a two-pixel rail with a small top cap (8 × 3px). Downbeats use longer top/bottom ticks (12px); other beats use short ticks (5px). Optional downbeat triangles are available through settings. Native settings controls retain their toolkit shape and keyboard focus behavior.

## Components

### Waveform lane

An instrument strip, not a card. Loaded lanes show deck identity, clipped track title, optional BPM and the waveform. Empty lanes preserve identity and show “Waiting for track”. Default rendering is symmetric three-band; optional overview, band stacking and RGB modes remain supported. Waveform uploads use tiles up to (4096px) wide and a full-track cap of (32768px). Texture density targets the visible zoom up to (150px/second), then reduces for tracks exceeding the cap; do not claim source detail is preserved without that qualification. Source amplitude data determines the shape; decorative waveforms must not stand in for live data.

### Cue flags and saved loops

Flags show A–H for hot cues and M for memory cues; L is appended for a saved loop. Cue stems stay at their true track-time coordinate. The allocator searches free intervals across two label rails, choosing a nearby available position to avoid collisions and using a horizontal leader to retain the original cue anchor. If every label position is occupied, stems remain visible even when a flag is omitted. Saved loops draw a lower bracket and end stems in their cue color; brackets clip to the visible time window.

**The Saved Is Not Active Rule.** A saved-loop bracket indicates stored endpoints only. No active-loop engagement state is supplied to this renderer; do not imply that the loop is currently running.

### Settings buttons and fields

Tk clam buttons use the button background with flat relief and zero border width, compact padding and a distinct active background. Comboboxes and entries use the field surface. Keep toolkit keyboard focus, visible labels, checkbox state and native dialogs. The settings window resizes to its requested content dimensions when deck count or tuning visibility changes. The sidecar contains scoped HTML/CSS translations for visual inspection only; these are not a second runtime implementation and do not establish new native focus or hover behavior.

### Status copy

Synthetic data is explicitly labelled “SYNTHETIC DEMO”. On macOS, settings states that direct Rekordbox sync is unavailable and OSC requires a compatible sender. Do not let visual polish obscure an unavailable input or present demo playback as a live connection.

## Do's and Don'ts

### Do:
- **Do** preserve the shared track-time transform for waveform, beat, cue and loop positions.
- **Do** retain physical deck identity when rows are reordered or the deck count changes.
- **Do** keep native focus and labels usable in the separate settings window.
- **Do** distinguish synthetic demo, offline analysis and live input states.

### Don't:
- **Don't** use semantic signal and cue colors as decorative accents.
- **Don't** imply a saved loop is engaged without a verified active-loop state.
- **Don't** claim direct macOS live synchronization from a successful demo render.
- **Don't** introduce decorative motion or elevation into the waveform canvas.
