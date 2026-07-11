// Fresh harness: tgcalls group AudioStreamingPartInternal custom parsers that
// take attacker-controlled broadcast/stream bytes BEFORE the ffmpeg handoff:
//   - parseChannelUpdates  (AudioStreamingPartInternal.cpp:59-97)  [never fuzzed]
//   - readInt32            (:47-57)
// Plus a faithful model of the VideoStreamingPartState post-consume SLICE
// bounds (VideoStreamingPart.cpp:742-799) — the events[i].offset / endOffset
// slicing that produces dataSlice, to dynamically confirm the static claim that
// the offset guards keep every slice inside [begin,end].
//
// Extracted verbatim (absl::optional -> std::optional). No ffmpeg/webrtc needed.
// Build: clang++ -std=c++17 -g -O1 -fsanitize=address,fuzzer -o fuzz_audio_channels fuzz_audio_channels.cpp
#include <cstdint>
#include <cstring>
#include <optional>
#include <string>
#include <vector>

namespace {

// ---- audio: AudioStreamingPartInternal.cpp readInt32 (std::string variant) ----
std::optional<uint32_t> readInt32_str(std::string const &data, int &offset) {
    if (offset + 4 > (int)data.length()) {
        return std::nullopt;
    }
    int32_t value = 0;
    memcpy(&value, data.data() + offset, 4);
    offset += 4;
    return value;
}

struct ChannelUpdate { int frameIndex = 0; int id = 0; uint32_t ssrc = 0; };

std::vector<ChannelUpdate> parseChannelUpdates(std::string const &data, int &offset) {
    std::vector<ChannelUpdate> result;
    auto channels = readInt32_str(data, offset);
    if (!channels) return {};
    auto count = readInt32_str(data, offset);
    if (!count) return {};
    for (int i = 0; i < (int)count.value(); i++) {
        auto frameIndex = readInt32_str(data, offset);
        if (!frameIndex) return {};
        auto channelId = readInt32_str(data, offset);
        if (!channelId) return {};
        auto ssrc = readInt32_str(data, offset);
        if (!ssrc) return {};
        ChannelUpdate update;
        update.frameIndex = frameIndex.value();
        update.id = channelId.value();
        update.ssrc = ssrc.value();
        result.push_back(update);
    }
    return result;
}

// ---- video: the vector<uint8_t> readers + the SLICE-bounds model ----
std::optional<int32_t> readInt32(std::vector<uint8_t> const &data, int &offset) {
    if (offset + 4 > (int)data.size()) return std::nullopt;
    int32_t value = 0;
    memcpy(&value, data.data() + offset, 4);
    offset += 4;
    return value;
}
std::optional<uint8_t> readBytesAsInt32(std::vector<uint8_t> const &data, int &offset, int count) {
    if (offset + count > (int)data.size()) return std::nullopt;
    if (count == 0) return std::nullopt;
    if (count <= 4) {
        int32_t value = 0;
        memcpy(&value, data.data() + offset, count);
        offset += count;
        return value;
    }
    return std::nullopt;
}
int32_t roundUp(int32_t n, int32_t m) {
    if (m == 0) return n;
    int32_t r = n % m;
    return r == 0 ? n : n + m - r;
}
std::optional<std::string> readSerializedString(std::vector<uint8_t> const &data, int &offset) {
    if (const auto tmp = readBytesAsInt32(data, offset, 1)) {
        int paddingBytes = 0, length = 0;
        if (tmp.value() == 254) {
            if (const auto len = readBytesAsInt32(data, offset, 3)) {
                length = len.value();
                paddingBytes = roundUp(length, 4) - length;
            } else return std::nullopt;
        } else {
            length = tmp.value();
            paddingBytes = roundUp(length + 1, 4) - (length + 1);
        }
        if (offset + length > (int)data.size()) return std::nullopt;
        std::string result(data.data() + offset, data.data() + offset + length);
        offset += length;
        offset += paddingBytes;
        return result;
    }
    return std::nullopt;
}

struct Event { int32_t offset = 0; std::string endpointId; int32_t rotation = 0; int32_t extra = 0; };

std::optional<Event> readVideoStreamEvent(std::vector<uint8_t> const &data, int &offset) {
    Event e;
    if (const auto v = readInt32(data, offset)) e.offset = v.value(); else return std::nullopt;
    if (const auto v = readSerializedString(data, offset)) e.endpointId = v.value(); else return std::nullopt;
    if (const auto v = readInt32(data, offset)) e.rotation = v.value(); else return std::nullopt;
    if (const auto v = readInt32(data, offset)) e.extra = v.value(); else return std::nullopt;
    return e;
}

// Faithful model of consume + the VideoStreamingPartState slice loop.
void videoConsumeAndSlice(std::vector<uint8_t> data) {
    int offset = 0;
    std::vector<Event> events;
    if (const auto sig = readInt32(data, offset)) {
        if ((uint32_t)sig.value() != 0xa12e810d) return;
    } else return;
    std::string container;
    if (const auto c = readSerializedString(data, offset)) container = c.value(); else return;
    int32_t activeMask = 0; (void)activeMask;
    if (const auto a = readInt32(data, offset)) activeMask = a.value(); else return;
    if (const auto eventCount = readInt32(data, offset)) {
        if (eventCount.value() > 0) {
            if (const auto e = readVideoStreamEvent(data, offset)) events.push_back(e.value());
            else return;
        } else return;
    } else return;
    data.erase(data.begin(), data.begin() + offset);

    // VideoStreamingPartState ctor slice loop (VideoStreamingPart.cpp:742-799)
    for (size_t i = 0; i < events.size(); i++) {
        if (events[i].offset < 0) continue;
        size_t endOffset = 0;
        if (i == events.size() - 1) endOffset = data.size();
        else endOffset = events[i + 1].offset;
        if (endOffset <= (size_t)events[i].offset) continue;
        if (endOffset > data.size()) continue;
        std::vector<uint8_t> dataSlice(data.begin() + events[i].offset, data.begin() + endOffset);
        // touch the slice so ASan sees any bad range
        volatile uint8_t sink = 0;
        for (auto b : dataSlice) sink ^= b;
        (void)sink;
    }
}

} // namespace

extern "C" int LLVMFuzzerTestOneInput(const uint8_t *d, size_t s) {
    // Split the input: first byte selects target, rest is payload.
    if (s < 1) return 0;
    const uint8_t sel = d[0];
    const uint8_t *p = d + 1;
    size_t n = s - 1;
    if (sel & 1) {
        std::string str((const char *)p, n);
        int offset = 0;
        auto r = parseChannelUpdates(str, offset);
        (void)r;
    } else {
        std::vector<uint8_t> data(p, p + n);
        videoConsumeAndSlice(std::move(data));
    }
    return 0;
}
