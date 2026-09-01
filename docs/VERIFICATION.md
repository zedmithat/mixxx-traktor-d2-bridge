# Verified hardware snapshot

Last full verification: 2026-09-01, Raspberry Pi 5, Raspberry Pi OS,
Mixxx 2.5.6 custom build, two stock Traktor Kontrol D2 units.

## Build gates

- Clean `-Wall -Wextra -Werror` bridge and analysis-helper build
- BeatGrid protobuf self-test
- Irregular BeatMap/negative-lead-in geometry test
- Load-rejection and complete native functionality contracts
- Clean Mixxx Release configure/build in an isolated directory
- GCC 14 fetched-libdjinterop reproducibility patch

## Controller and UI gates

- Complete two-deck input/LED mapping suite
- PLAY/CUE, Browse, FX, loop, roll/freeze, beatjump, sampler and touch-strip
  contracts
- Exact track identity/load polling and failure feedback
- Hot Cue overview/colour updates with PortMidi callback coalescing
- Smart Lists, sorting, Player time modes, Zed FX layout and launch animation
- USB player, removable-media monitor and folder-to-playlist synchronization

## Live integration gates

- Two `17cc:1400` D2 devices and two D2 hubs enumerated
- D2 Bridge ALSA input and output subscriptions connected to Mixxx
- Mixxx, D2 bridge, LAN manager and USB monitor active with zero restarts
- No kernel USB reset/timeout/stall, OOM or crash entries
- No Raspberry Pi thermal throttling
- ZED manager unauthenticated request rejected and authenticated status accepted
- systemd security exposure: manager `2.2 OK`, USB monitor `1.9 OK`
- Mixxx SQLite `quick_check`: `ok`
- 96 active tracks, zero missing physical files and zero stale playlist rows
- Seven managed `ZED / ...` folder playlists

## Repeat the regression suite

```sh
make -C bridge test-native
make -C bridge test-js
```

The native tests do not write to the Mixxx database. Hardware acceptance
should still be repeated after changing libctlra USB transfer code, device
serial assignment, the ALSA/PortMidi lifecycle or Mixxx engine controls.
