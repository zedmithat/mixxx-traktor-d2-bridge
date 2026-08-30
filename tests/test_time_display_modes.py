#!/usr/bin/env python3
"""Contract test for the custom two-state Mixxx WNumberPos patch."""

from pathlib import Path
import sys


ROOT = Path(__file__).resolve().parents[1]
PATCH = ROOT / "mixxx-source-patches" / "wnumberpos-two-mode.patch"


def main() -> int:
    text = PATCH.read_text(encoding="utf-8")
    required = (
        "-            m_displayMode = TrackTime::DisplayMode::ELAPSED_AND_REMAINING;",
        "+        } else {",
        "             m_displayMode = TrackTime::DisplayMode::ELAPSED;",
        "-    } else if (remain == 2.0) {",
        "+        if (remain != 0.0) {",
        "+            m_pShowTrackTimeRemaining->set(0.0);",
        '-            << tr("Click to toggle between time elapsed/remaining time/both.")',
        '+            << tr("Click to toggle between elapsed and remaining time.")',
    )
    for line in required:
        if line not in text:
            raise AssertionError(f"two-mode WNumberPos patch lost contract line: {line}")

    added_lines = "\n".join(
        line[1:] for line in text.splitlines()
        if line.startswith("+") and not line.startswith("+++")
    )
    if "ELAPSED_AND_REMAINING" in added_lines:
        raise AssertionError("two-mode patch still adds the combined time mode")

    print("TIME_DISPLAY_MODES_TEST_OK modes=elapsed,remaining combined=disabled")
    return 0


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except AssertionError as error:
        print(f"TIME_DISPLAY_MODES_TEST_FAILED: {error}", file=sys.stderr)
        raise SystemExit(1)
