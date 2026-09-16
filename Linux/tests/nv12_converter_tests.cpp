#include "opendisplay/nv12_converter.hpp"

#include <unistd.h>

#include <algorithm>
#include <cassert>
#include <cstdint>
#include <cstdlib>
#include <fstream>
#include <iterator>
#include <string>
#include <utility>
#include <vector>

namespace {

// The pattern a review reproduced the slice corruption with: every byte
// differs from its neighbours, so a misplaced row or vector shows up.
std::vector<std::uint8_t> patternedImage(const int width, const int height, const int stride) {
    std::vector<std::uint8_t> bytes(static_cast<std::size_t>(stride) * static_cast<std::size_t>(height));
    for (std::size_t index = 0; index < bytes.size(); ++index) {
        bytes[index] = static_cast<std::uint8_t>((index * 17 + index / 23) % 256);
    }
    return bytes;
}

std::string readFile(const std::string& path) {
    std::ifstream file(path, std::ios::binary);
    return std::string((std::istreambuf_iterator<char>(file)), {});
}

// FFmpeg's own `scale=W:H:flags=fast_bilinear,format=nv12` is the reference:
// it is what the encoder subprocess ran before the capture thread converted.
std::string ffmpegReference(const std::vector<std::uint8_t>& image, const int stride,
                            const int width, const int height, const char* pixelFormat,
                            const int outputWidth, const int outputHeight) {
    char input[] = "/tmp/opendisplay-nv12-input-XXXXXX";
    char output[] = "/tmp/opendisplay-nv12-reference-XXXXXX";
    const int inputFd = ::mkstemp(input);
    const int outputFd = ::mkstemp(output);
    assert(inputFd >= 0 && outputFd >= 0);
    ::close(inputFd);
    ::close(outputFd);
    {
        std::ofstream file(input, std::ios::binary);
        for (int row = 0; row < height; ++row) {
            file.write(reinterpret_cast<const char*>(image.data())
                           + static_cast<std::ptrdiff_t>(row) * stride,
                       static_cast<std::streamsize>(width) * 4);
        }
    }
    const std::string command = "ffmpeg -v error -y -f rawvideo -pixel_format "
        + std::string(pixelFormat) + " -video_size " + std::to_string(width) + "x"
        + std::to_string(height) + " -i " + input + " -vf scale=" + std::to_string(outputWidth)
        + ":" + std::to_string(outputHeight) + ":flags=fast_bilinear,format=nv12 -f rawvideo "
        + output;
    assert(std::system(command.c_str()) == 0);
    const std::string reference = readFile(output);
    ::unlink(input);
    ::unlink(output);
    assert(reference.size()
           == static_cast<std::size_t>(outputWidth) * static_cast<std::size_t>(outputHeight) * 3 / 2);
    return reference;
}

// The capture path in one step: stage the rows, then scale and pack them.
std::string convert(od::Nv12Converter& converter, const od::Rgb32Image& source,
                    const int outputWidth, const int outputHeight) {
    converter.stage(source);
    return converter.convert(outputWidth, outputHeight);
}

struct Format {
    AVPixelFormat format;
    const char* ffmpegName;
};

constexpr Format formats[] = {
    {AV_PIX_FMT_BGRA, "bgra"},
    {AV_PIX_FMT_BGR0, "bgr0"},
    {AV_PIX_FMT_RGBA, "rgba"},
    {AV_PIX_FMT_RGB0, "rgb0"},
};

// Threaded libswscale writes wrong bytes at slice boundaries when the NV12
// destination rows are not 32-byte aligned; 130 is even but not aligned.
void threadedScaleToUnalignedEvenSizeMatchesFfmpeg(od::Nv12Converter& converter) {
    const auto image = patternedImage(128, 96, 128 * 4);
    for (const auto& [format, name] : formats) {
        const auto reference = ffmpegReference(image, 128 * 4, 128, 96, name, 130, 98);
        const od::Rgb32Image source{
            .rows = image.data(), .stride = 128 * 4, .width = 128, .height = 96, .format = format,
        };
        for (int pass = 0; pass < 8; ++pass) {
            assert(convert(converter, source, 130, 98) == reference);
        }
    }
}

// The native path: the same size in and out, rows already aligned.
void nativeSizeMatchesFfmpeg(od::Nv12Converter& converter) {
    const auto image = patternedImage(128, 96, 128 * 4);
    const auto reference = ffmpegReference(image, 128 * 4, 128, 96, "bgra", 128, 96);
    const od::Rgb32Image source{
        .rows = image.data(), .stride = 128 * 4, .width = 128, .height = 96,
        .format = AV_PIX_FMT_BGRA,
    };
    assert(convert(converter, source, 128, 96) == reference);
}

// libswscale reads and writes whole SIMD vectors: a 2 or 8 pixel row is
// shorter than one vector, so the owned padded storage must absorb the
// overrun on both sides and the result must still be exact.
void rowsShorterThanOneVectorMatchFfmpeg(od::Nv12Converter& converter) {
    for (const int width : {2, 8}) {
        const int height = width == 2 ? 64 : 8;
        const auto image = patternedImage(width, height, width * 4);
        const auto reference = ffmpegReference(image, width * 4, width, height, "bgr0", width, height);
        const od::Rgb32Image source{
            .rows = image.data(), .stride = width * 4, .width = width, .height = height,
            .format = AV_PIX_FMT_BGR0,
        };
        assert(convert(converter, source, width, height) == reference);
    }
}

// PipeWire rows can carry padding; the padding bytes must not reach the output.
void paddedSourceStrideIsIgnored(od::Nv12Converter& converter) {
    constexpr int stride = 128 * 4 + 64;
    const auto image = patternedImage(128, 96, stride);
    const auto reference = ffmpegReference(image, stride, 128, 96, "rgba", 130, 98);
    const od::Rgb32Image source{
        .rows = image.data(), .stride = stride, .width = 128, .height = 96, .format = AV_PIX_FMT_RGBA,
    };
    assert(convert(converter, source, 130, 98) == reference);
}

// The capture thread gives the PipeWire buffer back between stage() and
// convert(), so the staged rows must be the converter's own copy: overwriting
// the source memory in between must not change the picture.
void stagedRowsOutliveTheSourceMemory(od::Nv12Converter& converter) {
    auto image = patternedImage(128, 96, 128 * 4);
    const auto reference = ffmpegReference(image, 128 * 4, 128, 96, "bgra", 130, 98);
    converter.stage(od::Rgb32Image{
        .rows = image.data(), .stride = 128 * 4, .width = 128, .height = 96,
        .format = AV_PIX_FMT_BGRA,
    });
    std::fill(image.begin(), image.end(), std::uint8_t{0xff});
    assert(converter.convert(130, 98) == reference);
}

// A PipeWire chunk flagged empty becomes blackNv12() without touching the
// converter, whose frames still hold the previous picture. The black must be
// the picture the converter makes of a black source, so the encoder sees the
// same levels either way, at even and odd output sizes.
void emptyChunkBlackMatchesConvertedBlackSource(od::Nv12Converter& converter) {
    const auto pattern = patternedImage(128, 96, 128 * 4);
    const std::vector<std::uint8_t> zeros(128 * 4 * 96, 0);
    for (const auto [width, height] : {std::pair{130, 98}, std::pair{64, 64}, std::pair{33, 17}}) {
        const auto picture = convert(
            converter,
            od::Rgb32Image{.rows = pattern.data(), .stride = 128 * 4, .width = 128, .height = 96,
                           .format = AV_PIX_FMT_BGRA},
            width, height);
        const auto black = od::blackNv12(width, height);
        assert(black.size() == picture.size());
        assert(black != picture);
        const auto reference = convert(
            converter,
            od::Rgb32Image{.rows = zeros.data(), .stride = 128 * 4, .width = 128, .height = 96,
                           .format = AV_PIX_FMT_BGRA},
            width, height);
        assert(black == reference);
    }
}

}  // namespace

int main() {
    od::Nv12Converter converter;
    // One converter runs every case, so the buffers are reallocated between
    // sizes and formats the way a renegotiated stream reallocates them.
    threadedScaleToUnalignedEvenSizeMatchesFfmpeg(converter);
    nativeSizeMatchesFfmpeg(converter);
    rowsShorterThanOneVectorMatchFfmpeg(converter);
    paddedSourceStrideIsIgnored(converter);
    stagedRowsOutliveTheSourceMemory(converter);
    threadedScaleToUnalignedEvenSizeMatchesFfmpeg(converter);
    emptyChunkBlackMatchesConvertedBlackSource(converter);
    return 0;
}
