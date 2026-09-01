#!/usr/bin/env python3
"""Small dependency-free LAN manager for the ZED standalone DJ system."""

from __future__ import annotations

import argparse
import base64
import json
import mimetypes
import os
from pathlib import Path, PurePosixPath
import secrets
import shutil
import subprocess
import threading
import time
from http import HTTPStatus
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from urllib.parse import parse_qs, unquote, urlsplit


USB_STATE_PATH = Path("/run/user/1000/zed-usb-state")
RUNTIME_STATE_PATH = Path("/run/user/1000/zed-manager-state.json")
MIXXX_COMMAND_PATH = Path("/run/user/1000/zed-manager-command")
MIXXX_STATE_PATH = Path("/run/user/1000/zed-manager-mixxx-state")
DEFAULT_LIBRARY_ROOT = Path("/home/pi/Music/ZED Library")
DEFAULT_ACCESS_KEY_PATH = Path("/home/pi/.config/zed-manager/access-key")
MAX_REQUEST_BYTES = 64 * 1024
MAX_UPLOAD_BYTES = 8 * 1024 * 1024 * 1024

AUDIO_EXTENSIONS = {
    ".aac", ".aif", ".aiff", ".alac", ".flac", ".m4a", ".mp3",
    ".mp4", ".ogg", ".opus", ".wav", ".wave", ".wv",
}
SYSTEM_NAMES = {
    "$recycle.bin", "system volume information", "recovery", "efi",
    "recycler", "recycled", "lost+found", "__macosx", ".spotlight-v100",
    ".trashes", ".fseventsd",
}


def atomic_write(path: Path, text: str, mode: int = 0o600) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    temporary = path.with_name(path.name + ".new")
    temporary.write_text(text, encoding="utf-8")
    os.chmod(temporary, mode)
    os.replace(temporary, path)


def load_or_create_access_key(path: Path) -> str:
    try:
        existing = path.read_text(encoding="utf-8").strip()
        if len(existing) >= 12:
            return existing
    except OSError:
        pass
    alphabet = "ABCDEFGHJKLMNPQRSTUVWXYZabcdefghijkmnopqrstuvwxyz23456789"
    key = "".join(secrets.choice(alphabet) for _ in range(18))
    atomic_write(path, key + "\n")
    return key


def parse_tab_state(text: str, magic: str) -> dict[str, str]:
    lines = text.splitlines()
    if not lines or lines[0].strip() != magic:
        return {}
    result: dict[str, str] = {}
    for line in lines[1:]:
        key, separator, value = line.partition("\t")
        if separator:
            result[key] = value
    return result


def read_tab_state(path: Path, magic: str) -> dict[str, str]:
    try:
        return parse_tab_state(path.read_text(encoding="utf-8"), magic)
    except OSError:
        return {}


def hidden_or_system(name: str) -> bool:
    folded = name.casefold()
    return not name or name.startswith(".") or folded in SYSTEM_NAMES


def audio_file(path: Path) -> bool:
    return path.suffix.casefold() in AUDIO_EXTENSIONS


def safe_relative_path(value: str) -> Path:
    normalized = value.replace("\\", "/")
    if normalized.startswith("/"):
        raise ValueError("Geçersiz kaynak yolu")
    normalized = normalized.strip("/")
    if not normalized:
        return Path()
    pure = PurePosixPath(normalized)
    if pure.is_absolute() or any(part in ("", ".", "..") for part in pure.parts):
        raise ValueError("Geçersiz kaynak yolu")
    if any(hidden_or_system(part) for part in pure.parts):
        raise ValueError("Gizli veya sistem klasörü seçilemez")
    return Path(*pure.parts)


def safe_entry_name(value: str) -> str:
    name = value.strip()
    if (not name or name in (".", "..") or len(name) > 180 or
            "/" in name or "\\" in name or "\x00" in name or
            hidden_or_system(name)):
        raise ValueError("Geçersiz dosya veya klasör adı")
    return name


def contained_path(root: Path, relative: Path) -> Path:
    root_resolved = root.resolve(strict=True)
    unresolved = root_resolved / relative
    cursor = root_resolved
    for part in relative.parts:
        cursor /= part
        if cursor.is_symlink():
            raise ValueError("Sembolik bağlantılar desteklenmiyor")
    candidate = unresolved.resolve(strict=True)
    try:
        candidate.relative_to(root_resolved)
    except ValueError as error:
        raise ValueError("Kaynak USB dışında") from error
    return candidate


def files_equal(source: Path, destination: Path, chunk_size: int = 1024 * 1024) -> bool:
    try:
        if source.stat().st_size != destination.stat().st_size:
            return False
        with source.open("rb") as source_file, destination.open("rb") as destination_file:
            while True:
                source_chunk = source_file.read(chunk_size)
                if source_chunk != destination_file.read(chunk_size):
                    return False
                if not source_chunk:
                    return True
    except OSError:
        return False


def unique_destination(path: Path, source: Path) -> tuple[Path, bool]:
    if not path.exists():
        return path, False
    if path.is_file() and files_equal(source, path):
        return path, True
    for index in range(2, 10000):
        candidate = path.with_name(f"{path.stem} (imported {index}){path.suffix}")
        if not candidate.exists():
            return candidate, False
    raise OSError("Benzersiz hedef adı oluşturulamadı")


def discover_audio(selection: Path) -> list[Path]:
    if selection.is_file():
        return [selection] if audio_file(selection) else []
    files: list[Path] = []
    for current, directories, filenames in os.walk(selection, followlinks=False):
        directories[:] = sorted(
            (name for name in directories
             if not hidden_or_system(name)
             and not (Path(current) / name).is_symlink()),
            key=str.casefold,
        )
        for filename in sorted(filenames, key=str.casefold):
            if hidden_or_system(filename):
                continue
            path = Path(current) / filename
            if path.is_symlink() or not audio_file(path):
                continue
            files.append(path)
    return files


class ManagerState:
    def __init__(self, library_root: Path, usb_state: Path = USB_STATE_PATH,
                 runtime_state: Path = RUNTIME_STATE_PATH,
                 mixxx_command: Path = MIXXX_COMMAND_PATH,
                 mixxx_state: Path = MIXXX_STATE_PATH) -> None:
        self.library_root = library_root
        self.usb_state_path = usb_state
        self.runtime_state_path = runtime_state
        self.mixxx_command_path = mixxx_command
        self.mixxx_state_path = mixxx_state
        self.library_root.mkdir(parents=True, exist_ok=True)
        self.lock = threading.Lock()
        self.upload_lock = threading.Lock()
        self.cancel_event = threading.Event()
        self.worker: threading.Thread | None = None
        self.progress: dict[str, object] = {
            "active": False,
            "phase": "idle",
            "message": "Hazır",
            "files_total": 0,
            "files_done": 0,
            "files_copied": 0,
            "files_skipped": 0,
            "files_failed": 0,
            "bytes_total": 0,
            "bytes_done": 0,
        }
        self.upload: dict[str, object] = {
            "active": False,
            "phase": "idle",
            "message": "Hazır",
            "bytes_total": 0,
            "bytes_done": 0,
            "filename": "",
        }

    def usb(self) -> dict[str, str]:
        state = read_tab_state(self.usb_state_path, "D2USB1")
        if state.get("PRESENT") != "1":
            return {}
        mount = Path(state.get("MOUNT", ""))
        if not mount.is_dir() or not os.access(mount, os.R_OK):
            return {}
        return state

    def snapshot(self) -> dict[str, object]:
        usb = self.usb()
        with self.lock:
            progress = dict(self.progress)
            upload = dict(self.upload)
        try:
            usage = shutil.disk_usage(self.library_root.parent)
            disk = {"free": usage.free, "total": usage.total}
        except OSError:
            disk = {"free": 0, "total": 0}
        mixxx = read_tab_state(self.mixxx_state_path, "ZEDMGR1")
        return {
            "usb": {
                "present": bool(usb),
                "label": usb.get("LABEL", ""),
                "mount": usb.get("MOUNT", ""),
                "source": usb.get("SOURCE", ""),
                "fstype": usb.get("FSTYPE", ""),
            },
            "import": progress,
            "upload": upload,
            "disk": disk,
            "mixxx": {
                "id": mixxx.get("ID", ""),
                "status": mixxx.get("STATUS", ""),
                "time": mixxx.get("TIME", ""),
            },
            "library_root": str(self.library_root),
        }

    def browse(self, relative_text: str) -> dict[str, object]:
        usb = self.usb()
        if not usb:
            raise FileNotFoundError("USB bağlı değil")
        relative = safe_relative_path(relative_text)
        mount = Path(usb["MOUNT"])
        directory = contained_path(mount, relative)
        if not directory.is_dir():
            raise NotADirectoryError("Klasör bulunamadı")
        entries: list[dict[str, object]] = []
        with os.scandir(directory) as iterator:
            ordered = sorted(iterator, key=lambda entry: (not entry.is_dir(follow_symlinks=False), entry.name.casefold()))
        for entry in ordered:
            if hidden_or_system(entry.name) or entry.is_symlink():
                continue
            is_directory = entry.is_dir(follow_symlinks=False)
            if not is_directory and not audio_file(Path(entry.name)):
                continue
            entries.append({
                "name": entry.name,
                "path": (PurePosixPath(relative.as_posix()) / entry.name).as_posix().lstrip("./"),
                "directory": is_directory,
                "size": 0 if is_directory else self._entry_size(entry),
            })
        return {"path": relative.as_posix() if relative.parts else "", "entries": entries}

    @staticmethod
    def _entry_size(entry: os.DirEntry[str]) -> int:
        try:
            return entry.stat(follow_symlinks=False).st_size
        except OSError:
            return 0

    def browse_library(self, relative_text: str) -> dict[str, object]:
        relative = safe_relative_path(relative_text)
        directory = contained_path(self.library_root, relative)
        if not directory.is_dir():
            raise NotADirectoryError("Yerel klasör bulunamadı")
        entries: list[dict[str, object]] = []
        with os.scandir(directory) as iterator:
            ordered = sorted(
                iterator,
                key=lambda entry: (
                    not entry.is_dir(follow_symlinks=False),
                    entry.name.casefold(),
                ),
            )
        for entry in ordered:
            if hidden_or_system(entry.name) or entry.is_symlink():
                continue
            is_directory = entry.is_dir(follow_symlinks=False)
            if not is_directory and not audio_file(Path(entry.name)):
                continue
            entries.append({
                "name": entry.name,
                "path": (PurePosixPath(relative.as_posix()) / entry.name).as_posix().lstrip("./"),
                "directory": is_directory,
                "size": 0 if is_directory else self._entry_size(entry),
            })
        return {
            "path": relative.as_posix() if relative.parts else "",
            "entries": entries,
        }

    def create_folder(self, parent_text: str, name_text: str) -> str:
        parent = contained_path(self.library_root, safe_relative_path(parent_text))
        if not parent.is_dir():
            raise NotADirectoryError("Üst klasör bulunamadı")
        name = safe_entry_name(name_text)
        destination = parent / name
        destination.mkdir(mode=0o755, exist_ok=False)
        return destination.relative_to(self.library_root).as_posix()

    def delete_library_entry(self, relative_text: str) -> str:
        relative = safe_relative_path(relative_text)
        if not relative.parts:
            raise ValueError("Yerel arşivin kökü silinemez")
        if not self.upload_lock.acquire(blocking=False):
            raise RuntimeError("Yükleme sürerken dosya silinemez")
        try:
            target = contained_path(self.library_root, relative)
            if target.is_dir():
                shutil.rmtree(target)
            elif target.is_file():
                target.unlink()
            else:
                raise FileNotFoundError("Silinecek öğe bulunamadı")
            return relative.as_posix()
        finally:
            self.upload_lock.release()

    def store_upload(self, directory_text: str, filename_text: str,
                     stream, length: int) -> dict[str, object]:
        if length <= 0 or length > MAX_UPLOAD_BYTES:
            raise ValueError("Geçersiz veya çok büyük yükleme")
        filename = safe_entry_name(filename_text)
        if not audio_file(Path(filename)):
            raise ValueError("Yalnızca desteklenen ses dosyaları yüklenebilir")
        directory = contained_path(
            self.library_root, safe_relative_path(directory_text))
        if not directory.is_dir():
            raise NotADirectoryError("Hedef klasör bulunamadı")
        free = shutil.disk_usage(self.library_root).free
        if free < length + 128 * 1024 * 1024:
            raise OSError("Yerel arşivde yeterli boş alan yok")
        if not self.upload_lock.acquire(blocking=False):
            raise RuntimeError("Başka bir yükleme devam ediyor")

        job_id = f"{int(time.time())}-{secrets.token_hex(4)}"
        temporary = directory / f".{filename}.zed-upload-{job_id}.partial"
        try:
            with self.lock:
                self.upload = {
                    "id": job_id,
                    "active": True,
                    "phase": "uploading",
                    "message": f"{filename} yükleniyor",
                    "bytes_total": length,
                    "bytes_done": 0,
                    "filename": filename,
                }
            remaining = length
            written = 0
            with temporary.open("xb") as destination_file:
                while remaining:
                    chunk = stream.read(min(1024 * 1024, remaining))
                    if not chunk:
                        raise OSError("Yükleme beklenenden önce kesildi")
                    destination_file.write(chunk)
                    written += len(chunk)
                    remaining -= len(chunk)
                    with self.lock:
                        self.upload["bytes_done"] = written
                destination_file.flush()
                os.fsync(destination_file.fileno())

            requested = directory / filename
            skipped = requested.exists() and requested.is_file() and files_equal(temporary, requested)
            if skipped:
                final_path = requested
                temporary.unlink()
            else:
                final_path, _ = unique_destination(requested, temporary)
                os.replace(temporary, final_path)
            relative = final_path.relative_to(self.library_root).as_posix()
            with self.lock:
                self.upload.update({
                    "active": False,
                    "phase": "complete",
                    "message": "Dosya zaten vardı" if skipped else "Yükleme tamamlandı",
                    "bytes_done": length,
                    "path": relative,
                    "skipped": skipped,
                })
            return {"path": relative, "skipped": skipped}
        except Exception as error:
            try:
                temporary.unlink()
            except FileNotFoundError:
                pass
            with self.lock:
                self.upload.update({
                    "active": False,
                    "phase": "error",
                    "message": str(error),
                })
            raise
        finally:
            self.upload_lock.release()

    def start_import(self, selections: list[str]) -> str:
        usb = self.usb()
        if not usb:
            raise FileNotFoundError("USB bağlı değil")
        if not selections:
            selections = [""]
        normalized = [safe_relative_path(value) for value in selections]
        with self.lock:
            if bool(self.progress.get("active")):
                raise RuntimeError("İçe aktarım zaten çalışıyor")
            job_id = f"{int(time.time())}-{secrets.token_hex(4)}"
            self.cancel_event.clear()
            self.progress = {
                "id": job_id,
                "active": True,
                "phase": "scanning",
                "message": "USB taranıyor",
                "files_total": 0,
                "files_done": 0,
                "files_copied": 0,
                "files_skipped": 0,
                "files_failed": 0,
                "bytes_total": 0,
                "bytes_done": 0,
            }
            self._publish_progress_locked()
            self.worker = threading.Thread(
                target=self._import_worker,
                args=(job_id, dict(usb), normalized),
                name="zed-import",
                daemon=True,
            )
            self.worker.start()
        return job_id

    def cancel(self) -> None:
        with self.lock:
            if bool(self.progress.get("active")):
                self.progress["message"] = "İptal bekleniyor"
                self.cancel_event.set()
                self._publish_progress_locked()

    def _publish_progress_locked(self) -> None:
        atomic_write(self.runtime_state_path,
                     json.dumps(self.progress, ensure_ascii=False) + "\n")

    def _set_progress(self, **updates: object) -> None:
        with self.lock:
            self.progress.update(updates)
            self._publish_progress_locked()

    def _import_worker(self, job_id: str, usb: dict[str, str], selections: list[Path]) -> None:
        try:
            mount = Path(usb["MOUNT"])
            label = usb.get("LABEL", "USB").strip() or "USB"
            safe_label = "".join(char if char not in '/\\:*?\"<>|' else "_" for char in label)
            destination_root = self.library_root / safe_label
            selected_files: list[tuple[Path, Path, int]] = []
            seen: set[Path] = set()
            for relative in selections:
                source = contained_path(mount, relative)
                for path in discover_audio(source):
                    if path in seen:
                        continue
                    seen.add(path)
                    try:
                        size = path.stat().st_size
                    except OSError:
                        size = 0
                    selected_files.append((path, path.relative_to(mount), size))
            bytes_total = sum(size for _, _, size in selected_files)
            self._set_progress(
                phase="copying",
                message="Parçalar yerel arşive kopyalanıyor",
                files_total=len(selected_files),
                bytes_total=bytes_total,
            )
            copied = skipped = failed = bytes_done = 0
            consecutive_io_errors = 0
            device_failure = False
            errors: list[dict[str, str]] = []
            for index, (source, relative, size) in enumerate(selected_files, start=1):
                if self.cancel_event.is_set():
                    self._set_progress(active=False, phase="cancelled", message="İçe aktarım iptal edildi")
                    return
                temporary: Path | None = None
                try:
                    destination, duplicate = unique_destination(destination_root / relative, source)
                    if duplicate:
                        skipped += 1
                        consecutive_io_errors = 0
                    else:
                        destination.parent.mkdir(parents=True, exist_ok=True)
                        temporary = destination.with_name(destination.name + f".zed-partial-{job_id}")
                        shutil.copy2(source, temporary)
                        os.replace(temporary, destination)
                        copied += 1
                        consecutive_io_errors = 0
                except OSError as error:
                    failed += 1
                    if error.errno == 5:
                        consecutive_io_errors += 1
                    else:
                        consecutive_io_errors = 0
                    if len(errors) < 20:
                        errors.append({"file": relative.as_posix(), "error": str(error)})
                finally:
                    if temporary is not None:
                        try:
                            temporary.unlink()
                        except FileNotFoundError:
                            pass
                bytes_done += size
                self._set_progress(
                    files_done=index,
                    files_copied=copied,
                    files_skipped=skipped,
                    files_failed=failed,
                    bytes_done=bytes_done,
                    current=relative.as_posix(),
                    errors=errors,
                )
                if consecutive_io_errors >= 5:
                    device_failure = True
                    break
            if copied > 0:
                self._request_mixxx_scan(job_id)
            if device_failure:
                final_phase = "device_error"
                message = (
                    "USB okuması güvenlik için durduruldu: art arda 5 donanım "
                    "I/O hatası. USB kablosunu, portu ve diski kontrol edin"
                )
            elif failed:
                final_phase = "complete_with_errors"
                message = (
                    f"Aktarım tamamlandı: {copied} kopyalandı, "
                    f"{skipped} zaten vardı, {failed} USB dosyası okunamadı"
                )
            elif copied > 0:
                final_phase = "complete"
                message = "İçe aktarım tamamlandı; Mixxx kütüphane taraması istendi"
            else:
                final_phase = "complete"
                message = "Seçilen parçaların tümü yerel arşivde zaten mevcut"
            self._set_progress(
                active=False,
                phase=final_phase,
                message=message,
                current="",
            )
        except Exception as error:  # keep the service alive and expose a concise error
            self._set_progress(active=False, phase="error", message=str(error), current="")

    def _request_mixxx_scan(self, job_id: str, purge_missing: bool = False) -> None:
        action = "SCAN_PURGE_MISSING" if purge_missing else "SCAN_SYNC_FOLDERS"
        content = f"ZEDMGR1\nID\t{job_id}\nACTION\t{action}\nTIME\t{int(time.time())}\n"
        atomic_write(self.mixxx_command_path, content)

    def request_mixxx_scan(self, reason: str = "library") -> str:
        safe_reason = "".join(char for char in reason if char.isalnum() or char in "-_")[:24]
        job_id = f"{safe_reason or 'library'}-{int(time.time())}-{secrets.token_hex(3)}"
        self._request_mixxx_scan(job_id)
        return job_id

    def request_mixxx_purge_missing(self) -> str:
        job_id = f"purge-missing-{int(time.time())}-{secrets.token_hex(3)}"
        self._request_mixxx_scan(job_id, purge_missing=True)
        return job_id

    def eject(self) -> str:
        with self.lock:
            if bool(self.progress.get("active")):
                raise RuntimeError("İçe aktarım sürerken USB çıkarılamaz")
        usb = self.usb()
        if not usb:
            raise FileNotFoundError("USB bağlı değil")
        source = usb.get("SOURCE", "")
        if not source.startswith("/dev/") or any(char.isspace() for char in source):
            raise ValueError("Geçersiz USB aygıtı")
        result = subprocess.run(
            ["/usr/bin/udisksctl", "unmount", "-b", source],
            capture_output=True,
            text=True,
            timeout=30,
            check=False,
        )
        if result.returncode != 0:
            raise RuntimeError((result.stderr or result.stdout or "USB ayrılamadı").strip())
        return (result.stdout or "USB güvenle ayrıldı").strip()


HTML = r"""<!doctype html>
<html lang="tr"><head><meta charset="utf-8"><meta name="viewport" content="width=device-width,initial-scale=1">
<title>ZED Library Manager</title>
<style>
:root{color-scheme:dark;--bg:#050708;--panel:#101517;--line:#263034;--cyan:#27e8e0;--amber:#ffb020;--red:#ff4d43;--muted:#8fa0a6}*{box-sizing:border-box}body{margin:0;background:radial-gradient(circle at 50% -20%,#193236 0,#050708 45%);font:15px system-ui,sans-serif;color:#f5fbfc;min-height:100vh}.wrap{max-width:920px;margin:auto;padding:24px}.brand{letter-spacing:.32em;font-weight:800;font-size:32px}.tag{color:var(--cyan);font-size:12px;letter-spacing:.18em;margin-top:4px}.grid{display:grid;grid-template-columns:1fr 1fr;gap:16px;margin-top:22px}.card{background:rgba(16,21,23,.94);border:1px solid var(--line);border-radius:12px;padding:18px;box-shadow:0 16px 40px #0008}.wide{grid-column:1/-1}.label{font-size:11px;color:var(--muted);letter-spacing:.14em}.value{font-size:21px;font-weight:650;margin-top:5px}.ok{color:var(--cyan)}.warn{color:var(--amber)}button{border:1px solid #3c4b50;background:#172024;color:#fff;border-radius:7px;padding:10px 14px;font-weight:650;cursor:pointer}button.primary{border-color:#168a87;background:#0b4544;color:#bffffc}button.danger{border-color:#88322e;color:#ffb7b3}button:disabled{opacity:.35;cursor:not-allowed}.toolbar{display:flex;gap:8px;flex-wrap:wrap;margin-top:14px}.crumb{color:var(--cyan);margin:14px 0;min-height:22px}.entries{border:1px solid var(--line);border-radius:8px;overflow:hidden}.entry{display:grid;grid-template-columns:32px 1fr auto;align-items:center;gap:9px;padding:10px 12px;border-bottom:1px solid #20282b}.entry:last-child{border:0}.entry:hover{background:#122326}.entry input{width:18px;height:18px}.entry .open{background:none;border:0;padding:0;text-align:left;font-weight:600}.drop{border:1px dashed #2b6d6a;border-radius:8px;padding:13px;text-align:center;color:var(--muted);margin-top:12px}.drop.active{border-color:var(--cyan);background:#0b3333;color:#c9fffc}.small{font-size:12px;color:var(--muted)}progress{width:100%;height:12px;accent-color:var(--cyan);margin-top:12px}.statusline{display:flex;justify-content:space-between;gap:12px;margin-top:8px}.toast{position:fixed;right:18px;bottom:18px;background:#142326;border:1px solid #2c7773;padding:12px 16px;border-radius:8px;display:none;max-width:420px}@media(max-width:650px){.wrap{padding:16px}.grid{grid-template-columns:1fr}.wide{grid-column:auto}.brand{font-size:27px}}
</style></head><body><main class="wrap"><div class="brand">ZED</div><div class="tag">OPEN DJ SYSTEM · LIBRARY MANAGER</div>
<section class="grid"><article class="card"><div class="label">USB PLAYER</div><div id="usb" class="value">Kontrol ediliyor…</div><div id="usbmeta" class="small"></div></article><article class="card"><div class="label">YEREL ARŞİV</div><div id="disk" class="value">—</div><div id="library" class="small"></div></article>
<article class="card wide"><div class="label">YEREL ARŞİV DOSYALARI</div><div id="localCrumb" class="crumb">/</div><div class="toolbar"><button id="localUp">Üst klasör</button><button id="localSelectAll">Tümünü seç</button><button id="newFolder">Yeni klasör</button><button id="chooseFiles" class="primary">Parça yükle</button><button id="deleteLocal" class="danger">Seçilenleri sil</button><button id="rescan">Mixxx'i tara</button><button id="purgeMissing" class="danger">Eksik kayıtları temizle</button><input id="fileInput" type="file" multiple accept=".aac,.aif,.aiff,.alac,.flac,.m4a,.mp3,.mp4,.ogg,.opus,.wav,.wave,.wv" hidden></div><div id="dropZone" class="drop">Ses dosyalarını bu alana sürükleyip bırakın</div><div id="localEntries" class="entries" style="margin-top:12px"></div></article>
<article class="card wide"><div class="label">USB İÇERİĞİ</div><div id="crumb" class="crumb">/</div><div class="toolbar"><button id="up">Üst klasör</button><button id="selectAll">Tümünü seç</button><button id="import" class="primary">Seçilenleri içe aktar</button><button id="eject" class="danger">USB'yi güvenle çıkar</button></div><div id="entries" class="entries" style="margin-top:12px"></div></article>
<article class="card wide"><div class="label">AKTARIM DURUMU</div><div id="message" class="value">Hazır</div><progress id="progress" value="0" max="1"></progress><div class="statusline"><span id="counts" class="small"></span><span id="mixxx" class="small"></span></div><div class="toolbar"><button id="cancel">Aktarımı iptal et</button></div></article></section></main><div id="toast" class="toast"></div>
<script>
const CSRF="__CSRF__";let path="",localPath="",snapshot=null;const $=id=>document.getElementById(id);const fmt=n=>{if(!n)return"0 B";const u=["B","KB","MB","GB","TB"];let i=0;while(n>=1024&&i<u.length-1){n/=1024;i++}return n.toFixed(i?1:0)+" "+u[i]};
function toast(t){$("toast").textContent=t;$("toast").style.display="block";setTimeout(()=>$("toast").style.display="none",3500)}
async function api(url,opt={}){opt.headers={...(opt.headers||{}),"X-ZED-CSRF":CSRF};const r=await fetch(url,opt);const j=await r.json();if(!r.ok)throw Error(j.error||"İşlem başarısız");return j}
async function refresh(){try{snapshot=await api("/api/status");const u=snapshot.usb;$("usb").textContent=u.present?u.label:"USB bağlı değil";$("usb").className="value "+(u.present?"ok":"warn");$("usbmeta").textContent=u.present?u.source+" · "+u.fstype:"";$("disk").textContent=fmt(snapshot.disk.free)+" boş";$("library").textContent=snapshot.library_root;const imp=snapshot.import,up=snapshot.upload,p=up.active?up:imp;$("message").textContent=p.message||"Hazır";$("message").className="value "+(["complete_with_errors","device_error","error"].includes(p.phase)?"warn":"");$("progress").max=Math.max(1,p.bytes_total||p.files_total||1);$("progress").value=p.bytes_total?p.bytes_done:(p.files_done||0);$("counts").textContent=up.active?(up.filename+" · "+fmt(up.bytes_done||0)+" / "+fmt(up.bytes_total||0)):((imp.files_done||0)+" / "+(imp.files_total||0)+" parça · "+fmt(imp.bytes_done||0)+(imp.files_failed?" · "+imp.files_failed+" okunamadı":""));$("mixxx").textContent=snapshot.mixxx.status?"Mixxx: "+snapshot.mixxx.status:"";$("cancel").disabled=!imp.active;$("import").disabled=!u.present||imp.active;$("eject").disabled=!u.present||imp.active;$("chooseFiles").disabled=up.active;$("deleteLocal").disabled=up.active;if(!u.present){$("entries").innerHTML='<div class="entry">Okunabilir DJ USB aygıtı bekleniyor</div>'}}catch(e){toast(e.message)}}
async function browse(next=path){try{const data=await api("/api/browse?path="+encodeURIComponent(next));path=data.path;$("crumb").textContent="/"+path;$("up").disabled=!path;$("entries").innerHTML=data.entries.map((e,i)=>`<div class="entry"><input type="checkbox" data-path="${encodeURIComponent(e.path)}"><button class="open" data-open="${e.directory?encodeURIComponent(e.path):""}">${e.directory?"▸ ":"♪ "}${esc(e.name)}</button><span class="small">${e.directory?"KLASÖR":fmt(e.size)}</span></div>`).join("")||'<div class="entry">Bu klasörde desteklenen parça yok</div>';document.querySelectorAll("[data-open]").forEach(b=>b.onclick=()=>b.dataset.open&&browse(decodeURIComponent(b.dataset.open)))}catch(e){toast(e.message)}}
async function localBrowse(next=localPath){try{const data=await api("/api/library?path="+encodeURIComponent(next));localPath=data.path;$("localCrumb").textContent="/"+localPath;$("localUp").disabled=!localPath;$("localEntries").innerHTML=data.entries.map(e=>`<div class="entry"><input type="checkbox" data-local="${encodeURIComponent(e.path)}"><button class="open" data-local-open="${e.directory?encodeURIComponent(e.path):""}">${e.directory?"▸ ":"♪ "}${esc(e.name)}</button><span class="small">${e.directory?"KLASÖR":fmt(e.size)}</span></div>`).join("")||'<div class="entry">Bu klasör boş</div>';document.querySelectorAll("[data-local-open]").forEach(b=>b.onclick=()=>b.dataset.localOpen&&localBrowse(decodeURIComponent(b.dataset.localOpen)))}catch(e){toast(e.message)}}
function esc(s){const d=document.createElement("div");d.textContent=s;return d.innerHTML}
$('localUp').onclick=()=>localBrowse(localPath.split('/').slice(0,-1).join('/'));$('localSelectAll').onclick=()=>document.querySelectorAll('#localEntries input[type="checkbox"]').forEach(c=>c.checked=true);
$('newFolder').onclick=async()=>{const name=prompt('Yeni klasör adı:');if(!name)return;try{await api('/api/folder',{method:'POST',headers:{'Content-Type':'application/json'},body:JSON.stringify({path:localPath,name})});await api('/api/rescan',{method:'POST'});await localBrowse();toast('Klasör oluşturuldu; Mixxx listesi güncelleniyor')}catch(e){toast(e.message)}};
async function uploadFiles(files){files=[...files];if(!files.length)return;let done=0,failure='';try{for(const file of files){await api('/api/upload',{method:'POST',headers:{'Content-Type':'application/octet-stream','X-ZED-Path':encodeURIComponent(localPath),'X-ZED-Filename':encodeURIComponent(file.name)},body:file});done++;await refresh()}}catch(e){failure=e.message}finally{if(done)await api('/api/rescan',{method:'POST'}).catch(e=>failure=failure||e.message);await localBrowse();$('fileInput').value='';refresh();toast(failure?(done+' dosya yüklendi; sonra durdu: '+failure):(done+' parça yüklendi; Mixxx taraması başlatıldı'))}}
$('chooseFiles').onclick=()=>$('fileInput').click();$('fileInput').onchange=e=>uploadFiles(e.target.files);const dz=$('dropZone');['dragenter','dragover'].forEach(n=>dz.addEventListener(n,e=>{e.preventDefault();dz.classList.add('active')}));['dragleave','drop'].forEach(n=>dz.addEventListener(n,e=>{e.preventDefault();dz.classList.remove('active')}));dz.addEventListener('drop',e=>uploadFiles(e.dataTransfer.files));
$('deleteLocal').onclick=async()=>{const paths=[...document.querySelectorAll('#localEntries input:checked')].map(c=>decodeURIComponent(c.dataset.local));if(!paths.length){toast('Silinecek dosya veya klasörü seçin');return}if(!confirm(paths.length+' seçim ve klasör içerikleri kalıcı olarak silinsin mi?'))return;let done=0,failure='';try{for(const selected of paths){await api('/api/delete',{method:'POST',headers:{'Content-Type':'application/json'},body:JSON.stringify({path:selected,confirm:selected})});done++}}catch(e){failure=e.message}finally{if(done)await api('/api/rescan',{method:'POST'}).catch(e=>failure=failure||e.message);await localBrowse();toast(failure?(done+' seçim silindi; sonra durdu: '+failure):'Seçilenler silindi; Mixxx taraması başlatıldı')}};
$('rescan').onclick=async()=>{try{await api('/api/rescan',{method:'POST'});toast('Mixxx kütüphane taraması başlatıldı')}catch(e){toast(e.message)}};
$('purgeMissing').onclick=async()=>{if(!confirm('Fiziksel dosyası bulunmayan tüm kayıtlar playlist, crate ve Mixxx veritabanından silinsin mi? Mevcut ses dosyalarına dokunulmaz.'))return;try{await api('/api/purge-missing',{method:'POST'});toast('Eksik kayıt taraması ve temizliği başlatıldı')}catch(e){toast(e.message)}};
$("up").onclick=()=>browse(path.split("/").slice(0,-1).join("/"));$("selectAll").onclick=()=>document.querySelectorAll('#entries input[type="checkbox"]').forEach(c=>c.checked=true);
$("import").onclick=async()=>{const paths=[...document.querySelectorAll('#entries input:checked')].map(c=>decodeURIComponent(c.dataset.path));if(!paths.length){toast("En az bir klasör veya parça seçin");return}if(!confirm(paths.length+" seçim yerel arşive aktarılsın mı?"))return;try{await api("/api/import",{method:"POST",headers:{"Content-Type":"application/json"},body:JSON.stringify({paths})});toast("İçe aktarım başladı");refresh()}catch(e){toast(e.message)}};
$("cancel").onclick=async()=>{try{await api("/api/cancel",{method:"POST"});toast("İptal istendi")}catch(e){toast(e.message)}};$("eject").onclick=async()=>{if(!confirm("USB güvenle ayrılsın mı?"))return;try{const j=await api("/api/eject",{method:"POST"});toast(j.message);path="";refresh()}catch(e){toast(e.message)}};
localBrowse();refresh().then(()=>snapshot?.usb.present&&browse(""));setInterval(refresh,1000);
</script></body></html>"""


class ZedHandler(BaseHTTPRequestHandler):
    server_version = "ZEDManager/1"

    @property
    def manager(self) -> ManagerState:
        return self.server.manager  # type: ignore[attr-defined]

    @property
    def csrf(self) -> str:
        return self.server.csrf  # type: ignore[attr-defined]

    @property
    def access_key(self) -> str:
        return self.server.access_key  # type: ignore[attr-defined]

    def _authenticated(self) -> bool:
        value = self.headers.get("Authorization", "")
        if not value.startswith("Basic "):
            return False
        try:
            decoded = base64.b64decode(value[6:], validate=True).decode("utf-8")
            username, separator, password = decoded.partition(":")
        except (ValueError, UnicodeDecodeError):
            return False
        return bool(separator) and username == "zed" and secrets.compare_digest(password, self.access_key)

    def _request_authentication(self) -> None:
        data = "Kimlik doğrulama gerekli".encode("utf-8")
        self.send_response(HTTPStatus.UNAUTHORIZED)
        self.send_header("WWW-Authenticate", 'Basic realm="ZED Open DJ System", charset="UTF-8"')
        self.send_header("Content-Type", "text/plain; charset=utf-8")
        self.send_header("Content-Length", str(len(data)))
        self.send_header("Cache-Control", "no-store")
        self.end_headers()
        self.wfile.write(data)

    def _headers(self, status: int, content_type: str, length: int) -> None:
        self.send_response(status)
        self.send_header("Content-Type", content_type)
        self.send_header("Content-Length", str(length))
        self.send_header("Cache-Control", "no-store")
        self.send_header("X-Content-Type-Options", "nosniff")
        self.send_header("Referrer-Policy", "no-referrer")
        self.send_header("Content-Security-Policy", "default-src 'self'; style-src 'unsafe-inline'; script-src 'unsafe-inline'")
        self.end_headers()

    def _json(self, payload: object, status: int = HTTPStatus.OK) -> None:
        data = json.dumps(payload, ensure_ascii=False).encode("utf-8")
        self._headers(status, "application/json; charset=utf-8", len(data))
        self.wfile.write(data)

    def _error(self, error: Exception, status: int = HTTPStatus.BAD_REQUEST) -> None:
        self._json({"error": str(error)}, status)

    def _csrf_ok(self) -> bool:
        return secrets.compare_digest(self.headers.get("X-ZED-CSRF", ""), self.csrf)

    def _body(self) -> dict[str, object]:
        try:
            length = int(self.headers.get("Content-Length", "0"))
        except ValueError as error:
            raise ValueError("Geçersiz istek") from error
        if length <= 0 or length > MAX_REQUEST_BYTES:
            raise ValueError("Geçersiz istek boyutu")
        return json.loads(self.rfile.read(length).decode("utf-8"))

    def do_GET(self) -> None:
        if not self._authenticated():
            self._request_authentication()
            return
        parsed = urlsplit(self.path)
        if parsed.path == "/":
            data = HTML.replace("__CSRF__", self.csrf).encode("utf-8")
            self._headers(HTTPStatus.OK, "text/html; charset=utf-8", len(data))
            self.wfile.write(data)
            return
        if parsed.path == "/api/status":
            self._json(self.manager.snapshot())
            return
        if parsed.path == "/api/browse":
            try:
                relative = parse_qs(parsed.query, keep_blank_values=True).get("path", [""])[0]
                self._json(self.manager.browse(unquote(relative)))
            except Exception as error:
                self._error(error, HTTPStatus.NOT_FOUND)
            return
        if parsed.path == "/api/library":
            try:
                relative = parse_qs(parsed.query, keep_blank_values=True).get("path", [""])[0]
                self._json(self.manager.browse_library(unquote(relative)))
            except Exception as error:
                self._error(error, HTTPStatus.NOT_FOUND)
            return
        self._json({"error": "Bulunamadı"}, HTTPStatus.NOT_FOUND)

    def do_POST(self) -> None:
        if not self._authenticated():
            self._request_authentication()
            return
        if not self._csrf_ok():
            self._json({"error": "Geçersiz güvenlik anahtarı"}, HTTPStatus.FORBIDDEN)
            return
        parsed = urlsplit(self.path)
        try:
            if parsed.path == "/api/import":
                body = self._body()
                paths = body.get("paths")
                if not isinstance(paths, list) or not all(isinstance(item, str) for item in paths):
                    raise ValueError("Geçersiz seçim")
                self._json({"id": self.manager.start_import(paths)}, HTTPStatus.ACCEPTED)
                return
            if parsed.path == "/api/cancel":
                self.manager.cancel()
                self._json({"ok": True})
                return
            if parsed.path == "/api/eject":
                self._json({"message": self.manager.eject()})
                return
            if parsed.path == "/api/folder":
                body = self._body()
                path = body.get("path")
                name = body.get("name")
                if not isinstance(path, str) or not isinstance(name, str):
                    raise ValueError("Geçersiz klasör isteği")
                self._json({"path": self.manager.create_folder(path, name)}, HTTPStatus.CREATED)
                return
            if parsed.path == "/api/delete":
                body = self._body()
                path = body.get("path")
                confirmation = body.get("confirm")
                if (not isinstance(path, str) or not isinstance(confirmation, str) or
                        not path or not secrets.compare_digest(path, confirmation)):
                    raise ValueError("Silme onayı seçilen yolla eşleşmiyor")
                self._json({"path": self.manager.delete_library_entry(path)})
                return
            if parsed.path == "/api/upload":
                try:
                    length = int(self.headers.get("Content-Length", "0"))
                except ValueError as error:
                    raise ValueError("Geçersiz yükleme boyutu") from error
                directory = unquote(self.headers.get("X-ZED-Path", ""))
                filename = unquote(self.headers.get("X-ZED-Filename", ""))
                self._json(
                    self.manager.store_upload(directory, filename, self.rfile, length),
                    HTTPStatus.CREATED,
                )
                return
            if parsed.path == "/api/rescan":
                self._json({"id": self.manager.request_mixxx_scan("web")}, HTTPStatus.ACCEPTED)
                return
            if parsed.path == "/api/purge-missing":
                self._json({"id": self.manager.request_mixxx_purge_missing()}, HTTPStatus.ACCEPTED)
                return
            self._json({"error": "Bulunamadı"}, HTTPStatus.NOT_FOUND)
        except FileNotFoundError as error:
            self._error(error, HTTPStatus.NOT_FOUND)
        except RuntimeError as error:
            self._error(error, HTTPStatus.CONFLICT)
        except Exception as error:
            self._error(error)

    def log_message(self, fmt: str, *args: object) -> None:
        if self.command != "GET" or self.path == "/":
            super().log_message(fmt, *args)


class ZedServer(ThreadingHTTPServer):
    daemon_threads = True
    allow_reuse_address = True

    def __init__(self, address: tuple[str, int], manager: ManagerState,
                 access_key: str) -> None:
        super().__init__(address, ZedHandler)
        self.manager = manager
        self.csrf = secrets.token_urlsafe(24)
        self.access_key = access_key


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--bind", default="0.0.0.0")
    parser.add_argument("--port", type=int, default=8088)
    parser.add_argument("--library-root", type=Path, default=DEFAULT_LIBRARY_ROOT)
    parser.add_argument("--access-key-file", type=Path, default=DEFAULT_ACCESS_KEY_PATH)
    args = parser.parse_args()
    args.library_root.mkdir(parents=True, exist_ok=True)
    manager = ManagerState(args.library_root)
    access_key = load_or_create_access_key(args.access_key_file)
    server = ZedServer((args.bind, args.port), manager, access_key)
    print(f"ZED Manager listening on http://{args.bind}:{args.port}", flush=True)
    try:
        server.serve_forever(poll_interval=0.5)
    except KeyboardInterrupt:
        pass
    finally:
        server.server_close()
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
