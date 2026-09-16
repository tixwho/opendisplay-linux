#include "opendisplay/pipewire_format.hpp"

#include <spa/param/format-utils.h>
#include <spa/param/video/raw.h>

#include <algorithm>
#include <cstdint>

namespace od {

std::optional<RgbChunk> rgbChunk(const spa_data& data, const int width, const int height) {
    const spa_chunk* chunk = data.chunk;
    if (chunk == nullptr || (chunk->flags & SPA_CHUNK_FLAG_CORRUPTED) != 0) {
        return std::nullopt;
    }
    if ((chunk->flags & SPA_CHUNK_FLAG_EMPTY) != 0) {
        return NeutralChunk{};
    }
    if (data.data == nullptr || data.maxsize == 0 || width <= 0 || height <= 0) {
        return std::nullopt;
    }
    const std::uint32_t offset = chunk->offset % data.maxsize;
    const std::uint64_t size = std::min<std::uint64_t>(chunk->size, data.maxsize - offset);
    const auto rowBytes = static_cast<std::uint64_t>(width) * 4;
    const int stride = chunk->stride > 0 ? chunk->stride : width * 4;
    if (static_cast<std::uint64_t>(stride) < rowBytes) {
        return std::nullopt;
    }
    const std::uint64_t required =
        static_cast<std::uint64_t>(stride) * static_cast<std::uint64_t>(height - 1) + rowBytes;
    if (size < required) {
        return std::nullopt;
    }
    return RgbChunkLayout{.offset = offset, .stride = stride};
}

const spa_pod* buildPipeWireFormatOffer(spa_pod_builder& builder, const int width,
                                        const int height, const int fps) {
    const auto requestedWidth = static_cast<std::uint32_t>(std::max(1, width));
    const auto requestedHeight = static_cast<std::uint32_t>(std::max(1, height));
    const auto requestedFps = static_cast<std::uint32_t>(std::max(1, fps));
    const spa_rectangle requestedSize = SPA_RECTANGLE(requestedWidth, requestedHeight);
    const spa_rectangle minimumSize = SPA_RECTANGLE(1, 1);
    const spa_rectangle maximumSize = SPA_RECTANGLE(16384, 16384);
    const spa_fraction requestedRate = SPA_FRACTION(requestedFps, 1);
    const spa_fraction minimumRate = SPA_FRACTION(0, 1);
    const spa_fraction maximumRate = SPA_FRACTION(std::max(requestedFps, 240U), 1);

    return static_cast<spa_pod*>(spa_pod_builder_add_object(
        &builder,
        SPA_TYPE_OBJECT_Format, SPA_PARAM_EnumFormat,
        SPA_FORMAT_mediaType, SPA_POD_Id(SPA_MEDIA_TYPE_video),
        SPA_FORMAT_mediaSubtype, SPA_POD_Id(SPA_MEDIA_SUBTYPE_raw),
        // KWin commonly exposes BGRx, while other producers use BGRA/RGBx/RGBA.
        // Every advertised layout is four bytes per pixel and is supported by
        // the FFmpeg input mapping.
        SPA_FORMAT_VIDEO_format, SPA_POD_CHOICE_ENUM_Id(
            5, SPA_VIDEO_FORMAT_BGRx, SPA_VIDEO_FORMAT_BGRx, SPA_VIDEO_FORMAT_BGRA,
            SPA_VIDEO_FORMAT_RGBx, SPA_VIDEO_FORMAT_RGBA),
        // The portal decides the actual virtual-output mode. The encoder scales
        // that mode to the receiver's requested output dimensions.
        SPA_FORMAT_VIDEO_size,
        SPA_POD_CHOICE_RANGE_Rectangle(&requestedSize, &minimumSize, &maximumSize),
        SPA_FORMAT_VIDEO_framerate,
        SPA_POD_CHOICE_RANGE_Fraction(&requestedRate, &minimumRate, &maximumRate)));
}

}  // namespace od
