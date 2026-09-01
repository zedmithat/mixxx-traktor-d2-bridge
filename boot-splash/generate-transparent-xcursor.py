#!/usr/bin/env python3
"""Generate a valid one-pixel, fully transparent XCursor image."""

from pathlib import Path
import struct
import sys


XCURSOR_MAGIC = 0x72756358
XCURSOR_IMAGE_TYPE = 0xFFFD0002
XCURSOR_FILE_VERSION = 0x00010000
XCURSOR_IMAGE_VERSION = 1


def build_cursor() -> bytes:
    file_header_size = 16
    toc_size = 12
    image_offset = file_header_size + toc_size
    image_header_size = 36

    file_header = struct.pack(
        "<IIII", XCURSOR_MAGIC, file_header_size, XCURSOR_FILE_VERSION, 1
    )
    toc = struct.pack("<III", XCURSOR_IMAGE_TYPE, 1, image_offset)
    image_header = struct.pack(
        "<IIIIIIIII",
        image_header_size,
        XCURSOR_IMAGE_TYPE,
        1,
        XCURSOR_IMAGE_VERSION,
        1,
        1,
        0,
        0,
        0,
    )
    transparent_argb_pixel = struct.pack("<I", 0)
    return file_header + toc + image_header + transparent_argb_pixel


def main() -> int:
    if len(sys.argv) != 2:
        print(f"Usage: {sys.argv[0]} OUTPUT", file=sys.stderr)
        return 2
    output = Path(sys.argv[1])
    output.parent.mkdir(parents=True, exist_ok=True)
    output.write_bytes(build_cursor())
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
