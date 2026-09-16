#include "opendisplay/pipewire_format.hpp"

#include <spa/param/video/format-utils.h>
#include <spa/param/video/raw.h>
#include <spa/pod/filter.h>

#include <array>
#include <cassert>
#include <cstdint>
#include <optional>
#include <variant>

namespace {

const spa_pod* fixedFormat(spa_pod_builder& builder, const spa_video_format format,
                           const std::uint32_t width, const std::uint32_t height,
                           const std::uint32_t fps) {
    const spa_rectangle size = SPA_RECTANGLE(width, height);
    const spa_fraction rate = SPA_FRACTION(fps, 1);
    return static_cast<spa_pod*>(spa_pod_builder_add_object(
        &builder,
        SPA_TYPE_OBJECT_Format, SPA_PARAM_EnumFormat,
        SPA_FORMAT_mediaType, SPA_POD_Id(SPA_MEDIA_TYPE_video),
        SPA_FORMAT_mediaSubtype, SPA_POD_Id(SPA_MEDIA_SUBTYPE_raw),
        SPA_FORMAT_VIDEO_format, SPA_POD_Id(format),
        SPA_FORMAT_VIDEO_size, SPA_POD_Rectangle(&size),
        SPA_FORMAT_VIDEO_framerate, SPA_POD_Fraction(&rate)));
}

void acceptsKwinCompatibleAlternative() {
    std::array<std::uint8_t, 1024> offerStorage{};
    spa_pod_builder offerBuilder = SPA_POD_BUILDER_INIT(
        offerStorage.data(), static_cast<std::uint32_t>(offerStorage.size()));
    const spa_pod* offer = od::buildPipeWireFormatOffer(offerBuilder, 2420, 1668, 60);

    std::array<std::uint8_t, 512> producerStorage{};
    spa_pod_builder producerBuilder = SPA_POD_BUILDER_INIT(
        producerStorage.data(), static_cast<std::uint32_t>(producerStorage.size()));
    const spa_pod* producer = fixedFormat(producerBuilder, SPA_VIDEO_FORMAT_RGBx, 1920, 1080, 30);

    std::array<std::uint8_t, 1024> resultStorage{};
    spa_pod_builder resultBuilder = SPA_POD_BUILDER_INIT(
        resultStorage.data(), static_cast<std::uint32_t>(resultStorage.size()));
    spa_pod* result = nullptr;
    assert(spa_pod_filter(&resultBuilder, &result, producer, offer) >= 0);
    assert(result != nullptr);

    spa_video_info_raw parsed{};
    assert(spa_format_video_raw_parse(result, &parsed) >= 0);
    assert(parsed.format == SPA_VIDEO_FORMAT_RGBx);
}

void rejectsUnsupportedPlaneLayout() {
    std::array<std::uint8_t, 1024> offerStorage{};
    spa_pod_builder offerBuilder = SPA_POD_BUILDER_INIT(
        offerStorage.data(), static_cast<std::uint32_t>(offerStorage.size()));
    const spa_pod* offer = od::buildPipeWireFormatOffer(offerBuilder, 2420, 1668, 60);

    std::array<std::uint8_t, 512> producerStorage{};
    spa_pod_builder producerBuilder = SPA_POD_BUILDER_INIT(
        producerStorage.data(), static_cast<std::uint32_t>(producerStorage.size()));
    const spa_pod* producer = fixedFormat(producerBuilder, SPA_VIDEO_FORMAT_NV12, 1920, 1080, 60);

    std::array<std::uint8_t, 1024> resultStorage{};
    spa_pod_builder resultBuilder = SPA_POD_BUILDER_INIT(
        resultStorage.data(), static_cast<std::uint32_t>(resultStorage.size()));
    spa_pod* result = nullptr;
    assert(spa_pod_filter(&resultBuilder, &result, producer, offer) < 0);
}

// A 4x2 BGRA image: two packed rows of 16 bytes in a mapped 64-byte plane.
std::uint8_t planeBytes[64];

spa_data plane(spa_chunk& chunk, const std::uint32_t maxsize = 64) {
    spa_data data{};
    data.data = planeBytes;
    data.maxsize = maxsize;
    data.chunk = &chunk;
    return data;
}

std::optional<od::RgbChunkLayout> layout(const spa_data& data) {
    const auto chunk = od::rgbChunk(data, 4, 2);
    if (!chunk || !std::holds_alternative<od::RgbChunkLayout>(*chunk)) {
        return std::nullopt;
    }
    return std::get<od::RgbChunkLayout>(*chunk);
}

bool neutral(const spa_data& data) {
    const auto chunk = od::rgbChunk(data, 4, 2);
    return chunk && std::holds_alternative<od::NeutralChunk>(*chunk);
}

void chunkLayoutFollowsSpaContract() {
    spa_chunk chunk{.offset = 0, .size = 32, .stride = 16, .flags = SPA_CHUNK_FLAG_NONE};
    auto rows = layout(plane(chunk));
    assert(rows && rows->offset == 0 && rows->stride == 16);

    // A zero stride means packed rows.
    chunk.stride = 0;
    rows = layout(plane(chunk));
    assert(rows && rows->stride == 16);

    // A padded stride needs the padding for every row but the last.
    chunk = {.offset = 0, .size = 48, .stride = 32, .flags = 0};
    rows = layout(plane(chunk));
    assert(rows && rows->stride == 32);
    chunk.size = 47;
    assert(!od::rgbChunk(plane(chunk), 4, 2));

    // The offset moves the rows and counts modulo maxsize.
    chunk = {.offset = 16, .size = 32, .stride = 16, .flags = 0};
    rows = layout(plane(chunk));
    assert(rows && rows->offset == 16);
    chunk.offset = 64 + 16;
    rows = layout(plane(chunk));
    assert(rows && rows->offset == 16);
    // The valid size is clamped to the bytes after the offset.
    chunk = {.offset = 48, .size = 32, .stride = 16, .flags = 0};
    assert(!od::rgbChunk(plane(chunk), 4, 2));

    // Chunks without bytes or with too few carry no image.
    chunk = {.offset = 0, .size = 0, .stride = 16, .flags = 0};
    assert(!od::rgbChunk(plane(chunk), 4, 2));
    chunk.size = 31;
    assert(!od::rgbChunk(plane(chunk), 4, 2));

    // A stride shorter than a row, a missing chunk, and an unmapped plane are rejected.
    chunk = {.offset = 0, .size = 64, .stride = 12, .flags = 0};
    assert(!od::rgbChunk(plane(chunk), 4, 2));
    chunk.stride = 16;
    assert(!od::rgbChunk(plane(chunk, 0), 4, 2));
    spa_data unmapped = plane(chunk);
    unmapped.data = nullptr;
    assert(!od::rgbChunk(unmapped, 4, 2));
    spa_data missing = plane(chunk);
    missing.chunk = nullptr;
    assert(!od::rgbChunk(missing, 4, 2));
}

// An empty chunk stands for a neutral black picture and its bytes are never
// read, so the offset, size, stride, and plane mapping may say anything.
// Corrupted wins over empty: such a chunk is dropped.
void emptyChunkIsNeutralUnlessCorrupted() {
    // The same chunk that carried a picture a moment ago: only the flag changed.
    spa_chunk chunk{.offset = 0, .size = 32, .stride = 16, .flags = SPA_CHUNK_FLAG_NONE};
    assert(layout(plane(chunk)));
    chunk.flags = SPA_CHUNK_FLAG_EMPTY;
    assert(neutral(plane(chunk)));
    assert(!layout(plane(chunk)));

    // Empty without any valid bytes, without a mapping, and with garbage fields.
    chunk = {.offset = 0, .size = 0, .stride = 0, .flags = SPA_CHUNK_FLAG_EMPTY};
    assert(neutral(plane(chunk)));
    assert(neutral(plane(chunk, 0)));
    spa_data unmapped = plane(chunk);
    unmapped.data = nullptr;
    assert(neutral(unmapped));
    chunk = {.offset = 0xffffffff, .size = 0xffffffff, .stride = -1,
             .flags = SPA_CHUNK_FLAG_EMPTY};
    assert(neutral(plane(chunk)));

    chunk = {.offset = 0, .size = 32, .stride = 16,
             .flags = SPA_CHUNK_FLAG_EMPTY | SPA_CHUNK_FLAG_CORRUPTED};
    assert(!od::rgbChunk(plane(chunk), 4, 2));
    chunk.flags = SPA_CHUNK_FLAG_CORRUPTED;
    assert(!od::rgbChunk(plane(chunk), 4, 2));
    chunk.flags = SPA_CHUNK_FLAG_NONE;
    assert(layout(plane(chunk)));
}

}  // namespace

int main() {
    acceptsKwinCompatibleAlternative();
    rejectsUnsupportedPlaneLayout();
    chunkLayoutFollowsSpaContract();
    emptyChunkIsNeutralUnlessCorrupted();
}
