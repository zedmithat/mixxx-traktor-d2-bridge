#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstdint>
#include <limits>
#include <string>
#include <utility>
#include <vector>
#include <sqlite3.h>

#include "beats.pb.h"

namespace {

bool decodeBeatGrid(const void* blob,
                    int blobSize,
                    std::int32_t* firstFrame,
                    double* bpm) {
    if (!blob || blobSize <= 0 || !firstFrame || !bpm) {
        return false;
    }

    mixxx::track::io::BeatGrid grid;
    if (!grid.ParseFromArray(blob, blobSize) || !grid.has_first_beat() ||
            !grid.first_beat().has_frame_position() || !grid.has_bpm() ||
            !grid.bpm().has_bpm()) {
        return false;
    }

    const double decodedBpm = grid.bpm().bpm();
    if (!std::isfinite(decodedBpm) || decodedBpm <= 0.0) {
        return false;
    }

    *firstFrame = grid.first_beat().frame_position();
    *bpm = decodedBpm;
    return true;
}

bool decodeBeatMap(const void* blob,
                   int blobSize,
                   std::vector<std::int32_t>* frames) {
    if (!blob || blobSize <= 0 || !frames) {
        return false;
    }

    mixxx::track::io::BeatMap map;
    if (!map.ParseFromArray(blob, blobSize) || map.beat_size() < 2) {
        return false;
    }

    std::vector<std::int32_t> decodedFrames;
    decodedFrames.reserve(static_cast<std::size_t>(map.beat_size()));
    for (int i = 0; i < map.beat_size(); ++i) {
        const auto& beat = map.beat(i);
        if (!beat.has_frame_position()) {
            return false;
        }

        const std::int32_t frame = beat.frame_position();
        if (!decodedFrames.empty() && frame <= decodedFrames.back()) {
            return false;
        }
        // Preserve every marker, including disabled markers. Mixxx uses the
        // ordered frame sequence as the authoritative variable-tempo map.
        decodedFrames.push_back(frame);
    }

    *frames = std::move(decodedFrames);
    return true;
}

std::string serialize(const google::protobuf::MessageLite& message) {
    std::string bytes;
    if (!message.SerializeToString(&bytes)) {
        return {};
    }
    return bytes;
}

int runSelfTest() {
    {
        mixxx::track::io::BeatMap map;
        auto* first = map.add_beat();
        first->set_frame_position(-512);
        first->set_enabled(false);
        map.add_beat()->set_frame_position(0);
        map.add_beat()->set_frame_position(512);
        const std::string bytes = serialize(map);
        std::vector<std::int32_t> frames;
        if (bytes.empty() || !decodeBeatMap(bytes.data(), bytes.size(), &frames) ||
                frames.size() != 3 || frames[0] != -512 || frames[1] != 0 ||
                frames[2] != 512) {
            std::fprintf(stderr, "negative/disabled BeatMap test failed\n");
            return 1;
        }
    }

    {
        const unsigned char malformed[] = {0x0a, 0x02, 0x08};
        std::vector<std::int32_t> frames;
        if (decodeBeatMap(malformed, sizeof(malformed), &frames)) {
            std::fprintf(stderr, "malformed BeatMap test failed\n");
            return 1;
        }
    }

    {
        mixxx::track::io::BeatMap duplicate;
        duplicate.add_beat()->set_frame_position(64);
        duplicate.add_beat()->set_frame_position(64);
        const std::string bytes = serialize(duplicate);
        std::vector<std::int32_t> frames;
        if (bytes.empty() || decodeBeatMap(bytes.data(), bytes.size(), &frames)) {
            std::fprintf(stderr, "duplicate BeatMap test failed\n");
            return 1;
        }
    }

    {
        mixxx::track::io::BeatMap outOfOrder;
        outOfOrder.add_beat()->set_frame_position(128);
        outOfOrder.add_beat()->set_frame_position(64);
        const std::string bytes = serialize(outOfOrder);
        std::vector<std::int32_t> frames;
        if (bytes.empty() || decodeBeatMap(bytes.data(), bytes.size(), &frames)) {
            std::fprintf(stderr, "out-of-order BeatMap test failed\n");
            return 1;
        }
    }

    {
        mixxx::track::io::BeatMap missingFrame;
        missingFrame.add_beat()->set_enabled(false);
        missingFrame.add_beat()->set_frame_position(64);
        const std::string bytes = serialize(missingFrame);
        std::vector<std::int32_t> frames;
        if (bytes.empty() || decodeBeatMap(bytes.data(), bytes.size(), &frames)) {
            std::fprintf(stderr, "missing-frame BeatMap test failed\n");
            return 1;
        }
    }

    {
        mixxx::track::io::BeatGrid grid;
        grid.mutable_first_beat()->set_frame_position(-256);
        grid.mutable_bpm()->set_bpm(123.5);
        const std::string bytes = serialize(grid);
        std::int32_t firstFrame = 0;
        double bpm = 0.0;
        if (bytes.empty() ||
                !decodeBeatGrid(bytes.data(), bytes.size(), &firstFrame, &bpm) ||
                firstFrame != -256 || bpm != 123.5) {
            std::fprintf(stderr, "negative BeatGrid test failed\n");
            return 1;
        }
    }

    {
        mixxx::track::io::BeatGrid grid;
        grid.mutable_first_beat()->set_frame_position(0);
        grid.mutable_bpm()->set_bpm(std::numeric_limits<double>::infinity());
        const std::string bytes = serialize(grid);
        std::int32_t firstFrame = 0;
        double bpm = 0.0;
        if (bytes.empty() || decodeBeatGrid(bytes.data(), bytes.size(), &firstFrame, &bpm)) {
            std::fprintf(stderr, "non-finite BeatGrid BPM test failed\n");
            return 1;
        }
    }

    {
        mixxx::track::io::BeatGrid grid;
        grid.mutable_first_beat()->set_frame_position(0);
        grid.mutable_bpm()->set_bpm(0.0);
        const std::string bytes = serialize(grid);
        std::int32_t firstFrame = 0;
        double bpm = 0.0;
        if (bytes.empty() || decodeBeatGrid(bytes.data(), bytes.size(), &firstFrame, &bpm)) {
            std::fprintf(stderr, "non-positive BeatGrid BPM test failed\n");
            return 1;
        }
    }

    std::puts("D2_BEATGRID_EXTRACT_TEST_OK");
    return 0;
}

} // namespace

int main(int argc, char** argv) {
    if (argc != 2) return 2;

    if (std::string(argv[1]) == "--self-test") {
        return runSelfTest();
    }

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
        if (blob && blob_size > 0 && std::isfinite(samplerate) &&
            samplerate > 0.0 && beats_version == "BeatGrid-2.0") {
            std::int32_t first_frame = 0;
            double bpm = 0.0;
            if (decodeBeatGrid(blob, blob_size, &first_frame, &bpm)) {
                std::printf("GRID %d %.9f %.9f\n",
                            static_cast<int>(first_frame), samplerate, bpm);
                result = 0;
            }
        } else if (blob && blob_size > 0 && std::isfinite(samplerate) &&
                   samplerate > 0.0 &&
                   beats_version == "BeatMap-1.0") {
            std::vector<std::int32_t> frames;
            if (decodeBeatMap(blob, blob_size, &frames)) {
                std::printf("MAP %d %.9f\n",
                            static_cast<int>(frames.size()), samplerate);
                for (const std::int32_t frame : frames) {
                    std::printf("%d\n", static_cast<int>(frame));
                }
                result = 0;
            }
        }
    }
    sqlite3_finalize(stmt);
    sqlite3_close(db);
    return result;
}
