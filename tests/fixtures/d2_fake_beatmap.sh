#!/bin/sh

# Deterministic variable-tempo BeatMap fixture for the bridge's offline
# metadata test. The first marker is a valid negative lead-in frame and the
# intervals are deliberately irregular.
printf '%s\n' \
    'MAP 7 1000.000000000' \
    '-250' \
    '0' \
    '480' \
    '1010' \
    '1490' \
    '2080' \
    '2590'
