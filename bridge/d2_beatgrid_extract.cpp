#include <cstdio>
#include <cstdlib>
#include <string>
#include <sqlite3.h>

#include "beats.pb.h"

int main(int argc, char** argv) {
    if (argc != 2) return 2;

    sqlite3* db = nullptr;
    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_open_v2("/home/pi/.mixxx/mixxxdb.sqlite", &db,
                        SQLITE_OPEN_READONLY, nullptr) != SQLITE_OK) return 3;
    if (std::string(argv[1]) == "--first-beatmap") {
        const char* find_sql =
            "SELECT id FROM library WHERE mixxx_deleted = 0 "
            "AND beats_version = 'BeatMap-1.0' AND length(beats) > 0 LIMIT 1";
        if (sqlite3_prepare_v2(db, find_sql, -1, &stmt, nullptr) != SQLITE_OK) {
            sqlite3_close(db);
            return 3;
        }
        int result = 4;
        if (sqlite3_step(stmt) == SQLITE_ROW) {
            std::printf("%d\n", sqlite3_column_int(stmt, 0));
            result = 0;
        }
        sqlite3_finalize(stmt);
        sqlite3_close(db);
        return result;
    }
    const int track_id = std::atoi(argv[1]);
    if (track_id <= 0) {
        sqlite3_close(db);
        return 2;
    }
    const char* sql =
        "SELECT beats, samplerate, beats_version FROM library "
        "WHERE id = ? AND mixxx_deleted = 0";
    if (sqlite3_prepare_v2(db, sql, -1, &stmt, nullptr) != SQLITE_OK) {
        sqlite3_close(db);
        return 3;
    }
    sqlite3_bind_int(stmt, 1, track_id);
    int result = 4;
    if (sqlite3_step(stmt) == SQLITE_ROW) {
        const void* blob = sqlite3_column_blob(stmt, 0);
        const int blob_size = sqlite3_column_bytes(stmt, 0);
        const double samplerate = sqlite3_column_double(stmt, 1);
        const unsigned char* version = sqlite3_column_text(stmt, 2);
        const std::string beats_version(
                version ? reinterpret_cast<const char*>(version) : "");
        if (blob && blob_size > 0 && samplerate > 0 &&
            beats_version == "BeatGrid-2.0") {
            mixxx::track::io::BeatGrid grid;
            if (grid.ParseFromArray(blob, blob_size) && grid.has_bpm() &&
                grid.has_first_beat() && grid.bpm().has_bpm() &&
                grid.first_beat().has_frame_position()) {
                std::printf("GRID %d %.9f %.9f\n",
                            grid.first_beat().frame_position(), samplerate,
                            grid.bpm().bpm());
                result = 0;
            }
        } else if (blob && blob_size > 0 && samplerate > 0 &&
                   beats_version == "BeatMap-1.0") {
            mixxx::track::io::BeatMap map;
            if (map.ParseFromArray(blob, blob_size) && map.beat_size() > 0) {
                std::printf("MAP %d %.9f\n", map.beat_size(), samplerate);
                for (int i = 0; i < map.beat_size(); ++i) {
                    const auto& beat = map.beat(i);
                    if (beat.has_frame_position() && beat.enabled())
                        std::printf("%d\n", beat.frame_position());
                    else
                        std::printf("-1\n");
                }
                result = 0;
            }
        }
    }
    sqlite3_finalize(stmt);
    sqlite3_close(db);
    return result;
}
