#!/usr/bin/env python3

from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
LIBRARY_CONTROL = (ROOT / "mixxx-source-patches" / "librarycontrol.cpp").read_text(
    encoding="utf-8"
)
WEB_MANAGER = (ROOT / "web" / "zed_manager.py").read_text(encoding="utf-8")


def main() -> None:
    required_native_contracts = (
        'QStringLiteral("SCAN_SYNC_FOLDERS")',
        "syncZedManagerFolders()",
        'QStringLiteral("/home/pi/Music/ZED Library")',
        'QStringLiteral("ZED / ")',
        "playlistDao.createPlaylist(playlistName)",
        "playlistDao.removeTracksFromPlaylist(playlistId, positions)",
        "playlistDao.appendTracksToPlaylist(trackIds, playlistId)",
        "playlistDao.deletePlaylists(obsoletePlaylistIds)",
        "trackDao.getAllTrackRefs(QDir(directoryPath))",
        "QFileInfo::exists(trackRef.getLocation())",
    )
    for contract in required_native_contracts:
        assert contract in LIBRARY_CONTROL, contract

    # Only the dedicated ZED namespace may be reconciled. Existing personal
    # playlists must stay outside the managed set.
    assert "playlist.second.startsWith(kPlaylistPrefix)" in LIBRARY_CONTROL
    assert "desiredPlaylists.contains(it.key())" in LIBRARY_CONTROL

    required_web_contracts = (
        'action = "SCAN_PURGE_MISSING" if purge_missing else "SCAN_SYNC_FOLDERS"',
        'parsed.path == "/api/folder"',
        'parsed.path == "/api/upload"',
        'parsed.path == "/api/delete"',
        'parsed.path == "/api/rescan"',
        'parsed.path == "/api/purge-missing"',
    )
    for contract in required_web_contracts:
        assert contract in WEB_MANAGER, contract

    print(
        "ZED_FOLDER_PLAYLIST_TEST_OK "
        "namespace='ZED /' sync=native-playlist-dao startup=false"
    )


if __name__ == "__main__":
    main()
