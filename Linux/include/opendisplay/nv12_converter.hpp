#pragma once

extern "C" {
#include <libavutil/frame.h>
#include <libswscale/swscale.h>
}

#include <cstdint>
#include <string>

namespace od {

/// One packed 32-bit RGB image in caller-owned memory, `stride` bytes per row.
struct Rgb32Image {
    const std::uint8_t* rows = nullptr;
    int stride = 0;
    int width = 0;
    int height = 0;
    AVPixelFormat format = AV_PIX_FMT_NONE;
};

/// Converts and scales 32-bit RGB images to tightly packed NV12 with the same
/// `fast_bilinear` filter as FFmpeg's scale filter, on all CPU cores.
///
/// libswscale works in whole SIMD vectors and slices threaded work on 32-byte
/// aligned rows: it reads and writes past the end of short rows and corrupts
/// slice boundaries when a destination stride is not aligned. Both frames
/// therefore live in libavutil-allocated, padded storage owned here; the
/// input rows are copied in and the tight NV12 bytes are packed out.
///
/// Copying in and converting are separate calls, so a caller that borrows the
/// source pixels can give that memory back as soon as stage() returns.
class Nv12Converter {
public:
    Nv12Converter();
    ~Nv12Converter();
    Nv12Converter(const Nv12Converter&) = delete;
    Nv12Converter& operator=(const Nv12Converter&) = delete;

    /// Copies `image` into the converter's own padded storage. Nothing reads
    /// `image.rows` after it returns, so the caller may release that memory.
    /// Throws std::runtime_error when the storage cannot be allocated.
    void stage(const Rgb32Image& image);

    /// Returns the image staged last as `outputWidth` x `outputHeight` NV12:
    /// the luma plane followed by the interleaved chroma plane, both with a
    /// stride of `outputWidth`. Throws std::runtime_error when libswscale
    /// rejects the conversion, which is also what an unstaged converter gets.
    [[nodiscard]] std::string convert(int outputWidth, int outputHeight);

private:
    SwsContext* scaler_ = nullptr;
    AVFrame* source_ = nullptr;
    AVFrame* target_ = nullptr;
};

/// The `width` x `height` NV12 picture Nv12Converter::convert() makes of a
/// black source, packed the same way: limited-range luma 16 with neutral
/// chroma 128. Stands in for a PipeWire chunk that is flagged empty.
[[nodiscard]] std::string blackNv12(int width, int height);

}  // namespace od
