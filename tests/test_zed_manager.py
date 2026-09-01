#!/usr/bin/env python3

import importlib.util
import io
import json
import os
from pathlib import Path
import tempfile
import time


ROOT = Path(__file__).resolve().parents[1]
MODULE_PATH = ROOT / "web" / "zed_manager.py"
SPEC = importlib.util.spec_from_file_location("zed_manager", MODULE_PATH)
assert SPEC and SPEC.loader
zed = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(zed)


def wait_for_job(manager, timeout=5.0):
    deadline = time.monotonic() + timeout
    while time.monotonic() < deadline:
        state = manager.snapshot()["import"]
        if not state["active"]:
            return state
        time.sleep(0.02)
    raise AssertionError("import job timed out")


def main():
    source = (ROOT / "mixxx-source-patches" / "librarycontrol.cpp").read_text(encoding="utf-8")
    service = (ROOT / "systemd" / "zed-manager.service").read_text(encoding="utf-8")
    assert "processZedManagerCommand" in source
    assert "startLibraryScan()" in source
    assert "libraryScanFinished" in source
    assert "SCAN_PURGE_MISSING" in source
    assert "purgeZedManagerMissingTracks" in source
    assert "missingTracks.purgeTracks(indices)" in source
    assert "SCAN_SYNC_FOLDERS" in source
    assert "syncZedManagerFolders" in source
    assert "ZED / " in source
    assert "ProtectSystem=strict" in service
    assert "IOSchedulingClass=idle" in service
    for hardening in (
        "NoNewPrivileges=true",
        "PrivateDevices=true",
        "ProtectHome=read-only",
        "CapabilityBoundingSet=",
        "RestrictAddressFamilies=AF_UNIX AF_INET AF_INET6",
        "RestrictNamespaces=true",
        "SystemCallArchitectures=native",
        "UMask=0077",
    ):
        assert hardening in service, hardening
    manager_source = MODULE_PATH.read_text(encoding="utf-8")
    assert "_authenticated" in manager_source
    assert '<html lang="en">' in manager_source
    for label in (
        "LOCAL LIBRARY FILES",
        "Upload tracks",
        "Purge missing records",
        "TRANSFER STATUS",
        "Authentication required",
    ):
        assert label in manager_source, label
    for obsolete_turkish_label in (
        "YEREL ARŞİV",
        "Parça yükle",
        "Eksik kayıtları temizle",
        "AKTARIM DURUMU",
        "Kimlik doğrulama gerekli",
    ):
        assert obsolete_turkish_label not in manager_source, obsolete_turkish_label

    with tempfile.TemporaryDirectory() as key_directory:
        key_path = Path(key_directory) / "access-key"
        first_key = zed.load_or_create_access_key(key_path)
        assert len(first_key) == 18
        assert zed.load_or_create_access_key(key_path) == first_key
        if os.name == "posix":
            assert os.stat(key_path).st_mode & 0o777 == 0o600

    assert zed.hidden_or_system("$RECYCLE.BIN")
    assert zed.hidden_or_system(".Spotlight-V100")
    assert not zed.hidden_or_system("2026")
    assert zed.safe_relative_path("Sets/2026") == Path("Sets/2026")
    for invalid in ("../etc", "/etc", ".hidden/music"):
        try:
            zed.safe_relative_path(invalid)
        except ValueError:
            pass
        else:
            raise AssertionError(f"unsafe path accepted: {invalid}")

    with tempfile.TemporaryDirectory() as temporary:
        root = Path(temporary)
        usb = root / "usb"
        music = root / "Music" / "ZED Library"
        runtime = root / "run"
        (usb / "2026" / "House").mkdir(parents=True)
        (usb / "$RECYCLE.BIN").mkdir()
        (usb / "2026" / "House" / "track one.mp3").write_bytes(b"A" * 8192)
        (usb / "2026" / "House" / "notes.txt").write_text("ignore")
        (usb / "$RECYCLE.BIN" / "deleted.mp3").write_bytes(b"bad")
        state_path = runtime / "usb-state"
        zed.atomic_write(state_path, "\n".join((
            "D2USB1", "COUNT\t1", "PRESENT\t1", "LABEL\tTEST USB",
            f"MOUNT\t{usb}", "SOURCE\t/dev/sdz1", "FSTYPE\tvfat", "",
        )))
        manager = zed.ManagerState(
            music,
            usb_state=state_path,
            runtime_state=runtime / "manager.json",
            mixxx_command=runtime / "command",
            mixxx_state=runtime / "mixxx-state",
        )

        remote_folder = manager.create_folder("", "Remote Sets")
        assert remote_folder == "Remote Sets"
        upload = manager.store_upload(
            remote_folder, "network track.mp3", io.BytesIO(b"REMOTE-A"), 8)
        assert upload == {"path": "Remote Sets/network track.mp3", "skipped": False}
        duplicate_upload = manager.store_upload(
            remote_folder, "network track.mp3", io.BytesIO(b"REMOTE-A"), 8)
        assert duplicate_upload["skipped"] is True
        conflict_upload = manager.store_upload(
            remote_folder, "network track.mp3", io.BytesIO(b"REMOTE-B"), 8)
        assert conflict_upload == {
            "path": "Remote Sets/network track (imported 2).mp3",
            "skipped": False,
        }
        (music / "Remote Sets" / "ignore.txt").write_text("hidden from player")
        (music / ".private").mkdir()
        local_listing = manager.browse_library("")
        assert [entry["name"] for entry in local_listing["entries"]] == ["Remote Sets"]
        local_tracks = manager.browse_library("Remote Sets")
        assert [entry["name"] for entry in local_tracks["entries"]] == [
            "network track (imported 2).mp3", "network track.mp3"]
        for invalid_name in ("notes.txt", "../escape.mp3", ".hidden.mp3"):
            try:
                manager.store_upload("", invalid_name, io.BytesIO(b"X"), 1)
            except ValueError:
                pass
            else:
                raise AssertionError(f"unsafe upload accepted: {invalid_name}")
        manager.create_folder("Remote Sets", "Delete Me")
        (music / "Remote Sets" / "Delete Me" / "inside.mp3").write_bytes(b"X")
        assert manager.delete_library_entry("Remote Sets/Delete Me") == "Remote Sets/Delete Me"
        assert not (music / "Remote Sets" / "Delete Me").exists()
        try:
            manager.delete_library_entry("")
        except ValueError:
            pass
        else:
            raise AssertionError("library root deletion accepted")
        scan_id = manager.request_mixxx_scan("web-test")
        assert scan_id.startswith("web-test-")
        scan_command = zed.read_tab_state(runtime / "command", "ZEDMGR1")
        assert scan_command["ACTION"] == "SCAN_SYNC_FOLDERS"
        purge_id = manager.request_mixxx_purge_missing()
        assert purge_id.startswith("purge-missing-")
        purge_command = zed.read_tab_state(runtime / "command", "ZEDMGR1")
        assert purge_command["ACTION"] == "SCAN_PURGE_MISSING"

        listing = manager.browse("")
        assert [entry["name"] for entry in listing["entries"]] == ["2026"]
        manager.start_import(["2026"])
        result = wait_for_job(manager)
        assert result["phase"] == "complete", result
        assert result["files_copied"] == 1, result
        imported = music / "TEST USB" / "2026" / "House" / "track one.mp3"
        assert imported.read_bytes() == b"A" * 8192
        command = zed.read_tab_state(runtime / "command", "ZEDMGR1")
        assert command["ACTION"] == "SCAN_SYNC_FOLDERS"

        manager.start_import(["2026"])
        duplicate = wait_for_job(manager)
        assert duplicate["files_skipped"] == 1, duplicate
        assert len(list(imported.parent.glob("*.mp3"))) == 1
        persisted = json.loads((runtime / "manager.json").read_text())
        assert persisted["phase"] == "complete"

        (usb / "2027").mkdir()
        (usb / "2027" / "good.mp3").write_bytes(b"G" * 4096)
        (usb / "2027" / "bad.mp3").write_bytes(b"B" * 4096)
        original_copy = zed.shutil.copy2

        def copy_with_one_io_error(source, destination):
            if Path(source).name == "bad.mp3":
                raise OSError(5, "simulated USB read error")
            return original_copy(source, destination)

        zed.shutil.copy2 = copy_with_one_io_error
        try:
            manager.start_import(["2027"])
            partial = wait_for_job(manager)
        finally:
            zed.shutil.copy2 = original_copy
        assert partial["phase"] == "complete_with_errors", partial
        assert partial["files_copied"] == 1, partial
        assert partial["files_failed"] == 1, partial
        assert len(partial["errors"]) == 1, partial
        assert (music / "TEST USB" / "2027" / "good.mp3").exists()
        assert not (music / "TEST USB" / "2027" / "bad.mp3").exists()

        (usb / "2028").mkdir()
        for index in range(8):
            (usb / "2028" / f"broken-{index}.mp3").write_bytes(b"X" * 128)

        def always_failing_copy(source, destination):
            raise OSError(5, "simulated failing USB bridge")

        zed.shutil.copy2 = always_failing_copy
        try:
            manager.start_import(["2028"])
            stopped = wait_for_job(manager)
        finally:
            zed.shutil.copy2 = original_copy
        assert stopped["phase"] == "device_error", stopped
        assert stopped["files_done"] == 5, stopped
        assert stopped["files_failed"] == 5, stopped
        assert "5 consecutive hardware" in stopped["message"], stopped

    print("ZED_MANAGER_TEST_OK")


if __name__ == "__main__":
    main()
