// Standalone fuzz harness extracting the tgcalls group VideoStreamingPart
// custom-container header parser (VideoStreamingPart.cpp:120-267) verbatim,
// with absl::optional -> std::optional. Goal: dynamically confirm/refute the
// static conclusion that these hand-rolled readers are bounds-safe.
#include <cstdint>
#include <cstring>
#include <optional>
#include <string>
#include <vector>

namespace {

std::optional<int32_t> readInt32(std::vector<uint8_t> const &data, int &offset) {
    if (offset + 4 > data.size()) {
        return std::nullopt;
    }
    int32_t value = 0;
    memcpy(&value, data.data() + offset, 4);
    offset += 4;
    return value;
}

std::optional<uint8_t> readBytesAsInt32(std::vector<uint8_t> const &data, int &offset, int count) {
    if (offset + count > data.size()) {
        return std::nullopt;
    }
    if (count == 0) {
        return std::nullopt;
    }
    if (count <= 4) {
        int32_t value = 0;
        memcpy(&value, data.data() + offset, count);
        offset += count;
        return value;
    } else {
        return std::nullopt;
    }
}

int32_t roundUp(int32_t numToRound, int32_t multiple) {
    if (multiple == 0) return numToRound;
    int32_t remainder = numToRound % multiple;
    if (remainder == 0) return numToRound;
    return numToRound + multiple - remainder;
}

std::optional<std::string> readSerializedString(std::vector<uint8_t> const &data, int &offset) {
    if (const auto tmp = readBytesAsInt32(data, offset, 1)) {
        int paddingBytes = 0;
        int length = 0;
        if (tmp.value() == 254) {
            if (const auto len = readBytesAsInt32(data, offset, 3)) {
                length = len.value();
                paddingBytes = roundUp(length, 4) - length;
            } else {
                return std::nullopt;
            }
        } else {
            length = tmp.value();
            paddingBytes = roundUp(length + 1, 4) - (length + 1);
        }
        if (offset + length > data.size()) {
            return std::nullopt;
        }
        std::string result(data.data() + offset, data.data() + offset + length);
        offset += length;
        offset += paddingBytes;
        return result;
    } else {
        return std::nullopt;
    }
}

struct VideoStreamEvent { int32_t offset = 0; std::string endpointId; int32_t rotation = 0; int32_t extra = 0; };
struct VideoStreamInfo { std::string container; int32_t activeMask = 0; std::vector<VideoStreamEvent> events; };

std::optional<VideoStreamEvent> readVideoStreamEvent(std::vector<uint8_t> const &data, int &offset) {
    VideoStreamEvent event;
    if (const auto v = readInt32(data, offset)) event.offset = v.value(); else return std::nullopt;
    if (const auto v = readSerializedString(data, offset)) event.endpointId = v.value(); else return std::nullopt;
    if (const auto v = readInt32(data, offset)) event.rotation = v.value(); else return std::nullopt;
    if (const auto v = readInt32(data, offset)) event.extra = v.value(); else return std::nullopt;
    return event;
}

std::optional<VideoStreamInfo> consumeVideoStreamInfo(std::vector<uint8_t> &data) {
    int offset = 0;
    if (const auto signature = readInt32(data, offset)) {
        if ((uint32_t)signature.value() != 0xa12e810d) return std::nullopt;
    } else return std::nullopt;
    VideoStreamInfo info;
    if (const auto c = readSerializedString(data, offset)) info.container = c.value(); else return std::nullopt;
    if (const auto a = readInt32(data, offset)) info.activeMask = a.value(); else return std::nullopt;
    if (const auto eventCount = readInt32(data, offset)) {
        if (eventCount > 0) {
            if (const auto e = readVideoStreamEvent(data, offset)) info.events.push_back(e.value());
            else return std::nullopt;
        } else return std::nullopt;
    } else return std::nullopt;
    data.erase(data.begin(), data.begin() + offset);
    return info;
}

} // namespace

extern "C" int LLVMFuzzerTestOneInput(const uint8_t *d, size_t s) {
    std::vector<uint8_t> data(d, d + s);
    auto r = consumeVideoStreamInfo(data);
    (void)r;
    return 0;
}
