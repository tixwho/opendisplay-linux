#pragma once

#include <spa/buffer/buffer.h>
#include <spa/pod/builder.h>

#include <cstdint>
#include <optional>
#include <variant>

namespace od {

/// Where the rows of one packed 32-bit RGB image start inside a mapped
/// PipeWire data plane, resolved from its chunk.
struct RgbChunkLayout {
    std::uint32_t offset = 0;  ///< first byte of the top row inside the plane
    int stride = 0;            ///< bytes per row; at least `width * 4`
};

/// A chunk flagged empty: its bytes are unspecified and stand for a neutral
/// black picture, whatever its offset, size, or the plane mapping say.
struct NeutralChunk {};

using RgbChunk = std::variant<NeutralChunk, RgbChunkLayout>;

/// Applies the SPA chunk contract to a `width` x `height` 32-bit RGB plane:
/// the chunk offset counts modulo `maxsize`, its size is clamped to the
/// bytes left after that offset, and a stride of zero means packed rows.
/// Returns the row layout, a neutral chunk, or nothing for a chunk that is
/// flagged corrupted (also when it is flagged empty as well), is missing,
/// sits in an unmapped plane, has no valid bytes, or does not hold every
/// row of the image.
std::optional<RgbChunk> rgbChunk(const spa_data& data, int width, int height);

/// Builds the raw-video capabilities accepted by the capture and encoder path.
/// The returned pod is owned by the caller-provided builder storage.
const spa_pod* buildPipeWireFormatOffer(spa_pod_builder& builder, int width, int height,
                                        int fps);

}  // namespace od
