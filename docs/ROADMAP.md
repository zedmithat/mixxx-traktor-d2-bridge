# D2 Integration Roadmap

This list records the agreed implementation order. A completed item must keep
the controller-input, complete-frame USB and player/browser regression tests
green before the next item starts.

## Completed

1. Isolate complete-frame D2 screen transfers from physical HID/MIDI input so
   display traffic cannot starve controller polling.
2. Resolve loaded tracks by exact Mixxx `track_id` or exact location and use
   that identity for metadata, waveform and analysis loading.
3. Validate and complete the real Mixxx BeatMap/downbeat path, including
   variable-tempo grids, without changing the working constant-tempo BeatGrid
   fallback.

   Parser and render geometry are implemented and covered by an offline
   irregular-map/negative-lead-in fixture. Live acceptance remains open
   because the current Mixxx library contains no `BeatMap-1.0` track (all 128
   analyzed tracks use `BeatGrid-2.0`). Mixxx's protobuf also has no native
   downbeat/time-signature field, so its first marker remains an explicit 4/4
   visual anchor rather than a claimed detected downbeat.
4. Keep Mixxx's safe `LoadWhenDeckPlaying=Reject` behavior. When a load is
   rejected because the target deck is playing, keep Browse open and show the
   temporary message `DECK PLAYING - STOP TO LOAD` on that D2 display. Do not
   stop the deck and do not switch to Player view.

   Implemented through an exact rejection-sequence CO emitted by Mixxx's
   authoritative load guard, not by guessing from LEDs or renderer state. The
   JS bridge routes that event to the corresponding D2 and the C compositor
   shows a two-second cached Browse overlay. Offline/no-selection failures are
   deliberately excluded. Automated and physical acceptance tests pass.

5. Show explicit Browse feedback for offline/missing files and other failed
   loads instead of presenting an empty Player screen.

   A missing path is rejected synchronously by Mixxx before EngineBuffer is
   touched, preserving the paused deck and avoiding a blocking error dialog.
   No-selection and active asynchronous reader/decoder failures have separate
   monotonic event sequences. The JS bridge keeps Browse open and the C
   compositor shows a centered, per-D2 two-second notice. Clean Browse and the
   accepted playing-deck warning remain pixel-identical. Automated and
   physical acceptance tests pass.

6. Run an end-to-end functionality audit: every visible badge/value and every
   physical control must be connected to live Mixxx state and covered by a
   regression test.

   Completed audit slices:

   - Track identity is now an atomic transaction: after every `TRACKID` or
     exact-location load, the controller forcibly replays duration, BPM,
     rate, position, play state, remaining time, beat window, transposed key,
     hotcues and phase. This prevents the JS cache from hiding values reset by
     the bridge when the same track/state is restored. Competing startup,
     settle and resilience timers are idempotent, so one accepted load starts
     exactly one metadata/waveform generation.
   - Both D2 surfaces have a regression matrix for HOTCUE, LOOP, FREEZE/ROLL,
     SAMPLER and BEATJUMP modes, pad press/release behavior, zoom, keylock,
     key sync, elapsed/remaining time, quantize and Capture.
   - LOOP, FREEZE/ROLL and BEATJUMP views consume the same live `LEDPACK` pad
     colors as the hardware LEDs. Their 4x2 cells now show selected/active loop
     size and momentary Roll/Beatjump press state instead of static decoration;
     their larger labels share one optically-centred typography contract.
   - The visible Player HUD now has a parser-to-renderer regression contract
     for title, two-decimal live BPM, transposed key/keylock, loop size/active
     state, elapsed/remaining time, duration, tempo, quantize, sync, zoom,
     phase, overview hotcues and the interpolated playhead. The same test also
     proves deck isolation. Unconsumed `CHANNEL` and `SAMPLERATE` wire messages
     were removed so they cannot add controller-output latency.

   The complete visible/control matrix passed its automated and physical
   acceptance checks on both decks.

7. Move the remaining HOTCUE and SAMPLER performance screens from the legacy
   per-pixel glyph loop to the same live 4x2 fast compositor used by Loop,
   Roll/Freeze and Beatjump. Preserve pad geometry and state colors while
   preventing these two views from delaying physical controller input.

   Both modes now share the accepted fast compositor and live hardware pad
   colors. Automated and physical acceptance tests pass on both D2 units.

8. Stop retransmitting an unchanged 480x272 Performance frame on every USB
   display callback. Cache only the visible mode, title and eight live pad
   colors, while forcing a complete redraw whenever a D2 returns from Player
   or Browse to the same Performance mode.

   The exact visible-state cache, non-blocking state snapshot and Player/
   Browse re-entry contract are deployed. Automated and physical acceptance
   tests pass on both D2 units.

9. Remove the shared FreeType face/glyph-cache race between the 60 Hz Deck
   producer and the HID-owned Browser/Performance compositor without adding
   a font mutex that can stall physical input.

   Every render thread now owns an isolated FreeType library, face and glyph
   cache. Concurrent rendering, pixel-identical Player/Browser/Performance
   output, stress tests and physical acceptance pass on both D2 units.

10. Remove the obsolete per-pixel fallback renderer after proving every valid
    screen enum has a dedicated Deck, Browser or Performance compositor.

    Every declared view is now covered by an exhaustive dispatch contract;
    invalid enum values are rejected. The removed renderer had no reachable
    valid caller. `-Werror`, 20-run stress, pixel-identical output and physical
    acceptance pass on both D2 units.

11. Prepare the accepted hardware snapshot for GitHub without publishing
    generated captures, temporary candidates, device serials or credentials.
    Verify that the repository source is byte-identical to the deployed Pi
    source and that a clean checkout has one reproducible build/test path.

    The active bridge, libctlra driver, controller mapping, MIDI XML and all
    50 zed skin files match the repository byte-for-byte. Secret scanning is
    clean, generated files are ignored, a fresh Pi native build/test passes,
    and the complete desktop JS/Python regression suite passes. The README
    contains the accepted player skin and D2 screen gallery.

12. Add an authenticated LAN library manager that keeps file I/O away from
    Mixxx's audio and D2 USB threads.

    The manager now supports restricted folder creation, multi-file picker and
    drag-and-drop uploads, atomic conflict-safe writes, confirmed deletion,
    free-space reporting and one native Mixxx scan per completed batch. It does
    not copy or scan merely because the Pi starts.

13. Remove stale offline library entries without deleting existing audio.

    The explicit web action first runs the native scanner and then passes only
    `fs_deleted=1` rows through `TrackCollectionManager::purgeTracks()`, which
    consistently clears playlist, crate, cue and analysis references. A safe
    SQLite backup was taken before hardware acceptance.

14. Mirror the physical `ZED Library` hierarchy into controller-visible Mixxx
    playlists.

    Native `PlaylistDAO` synchronization owns only the `ZED / ...` namespace,
    keeps unrelated user playlists intact, updates contents after explicit web
    or USB imports and removes obsolete managed playlists when their directory
    disappears. The desktop skin and both D2 browsers consume the same native
    sidebar entries.
