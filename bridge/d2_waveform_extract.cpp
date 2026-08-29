#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <fstream>
#include <vector>
#include <zlib.h>

#include "waveform.pb.h"

namespace {
constexpr int kOutputSamples = 32768;

int clampByte(int value) {
    return std::max(0, std::min(255, value));
}
}

int main(int argc, char** argv) {
    if (argc != 2) return 2;

    std::ifstream input(argv[1], std::ios::binary | std::ios::ate);
    if (!input) return 3;
    const std::streamsize fileSize = input.tellg();
    if (fileSize < 5) return 4;
    input.seekg(0);

    std::vector<unsigned char> compressed(static_cast<size_t>(fileSize));
    if (!input.read(reinterpret_cast<char*>(compressed.data()), fileSize)) return 5;

    /* Mixxx AnalysisDAO stores qCompress data: a big-endian uncompressed
     * size prefix followed by a zlib stream. Never trust the prefix for an
     * allocation; cap it against the file size and the waveform limit. */
    const uint32_t announced =
            (static_cast<uint32_t>(compressed[0]) << 24) |
            (static_cast<uint32_t>(compressed[1]) << 16) |
            (static_cast<uint32_t>(compressed[2]) << 8) |
            static_cast<uint32_t>(compressed[3]);
    if (announced == 0 || announced > 64U * 1024U * 1024U) return 6;
    std::vector<unsigned char> raw(announced);
    uLongf rawSize = static_cast<uLongf>(raw.size());
    if (uncompress(raw.data(), &rawSize, compressed.data() + 4,
                   static_cast<uLong>(compressed.size() - 4)) != Z_OK)
        return 7;

    mixxx::track::io::Waveform waveform;
    if (!waveform.ParseFromArray(raw.data(), static_cast<int>(rawSize)) ||
        !waveform.has_signal_filtered() ||
        !waveform.signal_filtered().has_low() ||
        !waveform.signal_filtered().has_mid() ||
        !waveform.signal_filtered().has_high())
        return 8;

    const auto& low = waveform.signal_filtered().low().value();
    const auto& mid = waveform.signal_filtered().mid().value();
    const auto& high = waveform.signal_filtered().high().value();
    const int sampleCount = std::min({low.size(), mid.size(), high.size()});
    if (sampleCount < 2) return 9;

    for (int output = 0; output < kOutputSamples; ++output) {
        const int source = static_cast<int>(
                (static_cast<int64_t>(output) * (sampleCount - 1) +
                 (kOutputSamples - 1) / 2) / (kOutputSamples - 1));
        const unsigned char pixel[3] = {
            static_cast<unsigned char>(clampByte(low.Get(source))),
            static_cast<unsigned char>(clampByte(mid.Get(source))),
            static_cast<unsigned char>(clampByte(high.Get(source))),
        };
        if (std::fwrite(pixel, sizeof(pixel), 1, stdout) != 1) return 10;
    }
    return 0;
}
