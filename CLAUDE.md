# Working on Rekordbox Wave

- **Always merge finished work into `main`** (open a pull request from the working branch and merge it),
  so the owner can update and test from `main` straight away. Release from what is on `main`.
- Releases: run the "Test and build desktop viewers" workflow with `release_tag` (for example `v2.7.0`)
  to build every platform and draft the release, then with `publish_tag` to publish it. Wave's
  auto-updater offers the latest published release. Bump `project(RekordboxWave VERSION ...)` in
  `app/CMakeLists.txt` first (the C++ app in `app/` is what ships since 3.0).
- Windows is the priority platform. CI is the only way to build the Windows bridge (MSVC) and installer.
- C++ tests: `cmake -S app -B app-build -G Ninja && cmake --build app-build && ctest --test-dir app-build`.
  Python (legacy) tests: `QT_QPA_PLATFORM=offscreen python -m unittest discover -s tests`; on Linux one
  fails because `time.CLOCK_UPTIME_RAW` is macOS-only; that is expected.
