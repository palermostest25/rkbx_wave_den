# Working on Rekordbox Wave

- **Always merge finished work into `main`** (open a pull request from the working branch and merge it),
  so the owner can update and test from `main` straight away. Release from what is on `main`.
- Releases: run the "Test and build desktop viewers" workflow with `release_tag` (for example `v2.7.0`)
  to build every platform and draft the release, then with `publish_tag` to publish it. Wave's
  auto-updater offers the latest published release. Bump `rb_waveform_core/__init__.py` first.
- Windows is the priority platform. CI is the only way to build the Windows bridge (MSVC) and installer.
- Tests: `QT_QPA_PLATFORM=offscreen python -m unittest discover -s tests`. On Linux one test fails because
  `time.CLOCK_UPTIME_RAW` is macOS-only; that is expected.
