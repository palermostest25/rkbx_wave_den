# Native waveform display

## Authority and surface
The user explicitly pinned the XDJ-AZ enlarged waveform language. The main display contains stacked waveforms; audio, browsing and deck controls remain in Rekordbox. Settings are a page of the same native Qt window, opened with SETTINGS/F2, with large controls grouped into Display, Waveform, Connection and Profiles tabs.

## Tokens
Canvas #000000; deck rail #101010; separators #252525; primary #EEEEEE; secondary #A9B0B9; playhead #F8F8F8 with black 5px underlay and 2.2px white line; downbeat #F52620; other beat ticks #B7BCC1; low default RGB(20,95,235); mid RGB(244,164,20); overlap RGB(169,107,39); high #FFFFFF. Native Arial text; settings 22 logical px × user settings scale; toolbar controls16px; waveform labels14–23px × independent wave scale. Settings focus #00A6FF, field fill #20252C, field border #4D5662.

## Geometry and behavior
Header fills54px at default scale; remaining space divides equally into2/4 rows. Default order3,1,2,4. Narrow left deck badges, rectangular pointed cue flags, red/quiet-white ticks outside the signal, bar numbers below it, and a pale playhead at configurable32%/50%/20%. No decorative cards, glow or meters. Full-screen placement uses QScreen.geometry and Win32 rcMonitor, never the work area. Native Qt scaling and PerMonitorV2 manifest keep physical coverage consistent.

## Data and rendering
Native-resolution tiles are cached with a fixed memory budget; zoomed-out tiles preserve peaks. PWV7 has fixed seven-bit heights, with mid and high calibrated relative to low. Never amplify every high band to a white waveform. Saved loop brackets are cue-coloured. Only authoritative live loop input enables the shaded engaged-loop region. Synthetic data is labelled; real analysis captures use offline status.

## Verification limits
Windows acceptance tests inspect native full-screen rectangle and bottom-corner window ownership at100%/150% scaling. Layout captures cover1920×1080 and1280×720, two/four decks, integrated settings, clustered cues and empty lanes. These checks establish UI behavior, not live FLX10 timing or missing native transport fields. The existing native readers’ actual capabilities and Mac access constraints remain explicit in README.
