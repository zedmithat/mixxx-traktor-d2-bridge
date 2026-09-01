#!/usr/bin/env python3
"""Publish mounted DJ USB media as a tiny, atomic runtime state file."""

from __future__ import annotations

import argparse
import os
from pathlib import Path, PurePosixPath
import time
from typing import Iterable


DEFAULT_STATE = Path("/run/user/1000/zed-usb-state")
DEFAULT_MOUNTINFO = Path("/proc/self/mountinfo")
MOUNT_ROOTS = ("/media/pi/", "/run/media/pi/")


def generated_mount_name(label: str) -> bool:
    """Return True for udisks fallback names used by unlabeled partitions."""
    lowered = label.casefold()
    return "-usb-" in lowered and lowered.startswith(("sd", "hd", "nvme"))


def device_sort_key(device: dict[str, str]) -> tuple[bool, str]:
    # A real filesystem label such as "REKORDBOX" or "Yeni Birim" represents
    # the DJ source. EFI/Recovery partitions usually receive generated udisks
    # names and must not become the D2's primary USB player.
    return generated_mount_name(device["label"]), device["label"].casefold()


def decode_mount_field(value: str) -> str:
    for escaped, plain in (("\\040", " "), ("\\011", "\t"),
                           ("\\012", "\n"), ("\\134", "\\")):
        value = value.replace(escaped, plain)
    return value


def sanitize(value: str) -> str:
    return value.replace("\t", " ").replace("\r", " ").replace("\n", " ")


def mounted_media(lines: Iterable[str]) -> list[dict[str, str]]:
    devices: list[dict[str, str]] = []
    for raw_line in lines:
        fields = raw_line.rstrip("\n").split()
        try:
            separator = fields.index("-")
        except ValueError:
            continue
        if len(fields) < 6 or separator + 2 >= len(fields):
            continue
        mount = decode_mount_field(fields[4])
        if not any(mount.startswith(root) and len(mount) > len(root)
                   for root in MOUNT_ROOTS):
            continue
        if not os.path.isdir(mount) or not os.access(mount, os.R_OK):
            continue
        devices.append({
            "label": sanitize(PurePosixPath(mount).name),
            "mount": sanitize(mount),
            "source": sanitize(decode_mount_field(fields[separator + 2])),
            "fstype": sanitize(fields[separator + 1]),
        })
    devices.sort(key=device_sort_key)
    return devices


def render_state(devices: list[dict[str, str]]) -> str:
    lines = ["D2USB1", f"COUNT\t{len(devices)}"]
    if devices:
        primary = devices[0]
        lines.extend((
            "PRESENT\t1",
            f"LABEL\t{primary['label']}",
            f"MOUNT\t{primary['mount']}",
            f"SOURCE\t{primary['source']}",
            f"FSTYPE\t{primary['fstype']}",
        ))
    else:
        lines.append("PRESENT\t0")
    return "\n".join(lines) + "\n"


def publish(state_path: Path, content: str) -> bool:
    try:
        if state_path.read_text(encoding="utf-8") == content:
            return False
    except FileNotFoundError:
        pass
    state_path.parent.mkdir(parents=True, exist_ok=True)
    temporary = state_path.with_name(state_path.name + ".new")
    temporary.write_text(content, encoding="utf-8")
    os.replace(temporary, state_path)
    return True


def sample(mountinfo_path: Path, state_path: Path) -> bool:
    try:
        lines = mountinfo_path.read_text(encoding="utf-8").splitlines()
    except OSError:
        lines = []
    return publish(state_path, render_state(mounted_media(lines)))


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--mountinfo", type=Path, default=DEFAULT_MOUNTINFO)
    parser.add_argument("--state", type=Path, default=DEFAULT_STATE)
    parser.add_argument("--once", action="store_true")
    parser.add_argument("--interval", type=float, default=1.0)
    args = parser.parse_args()

    while True:
        sample(args.mountinfo, args.state)
        if args.once:
            return 0
        time.sleep(max(0.25, args.interval))


if __name__ == "__main__":
    raise SystemExit(main())
