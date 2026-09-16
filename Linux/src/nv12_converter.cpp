#include "opendisplay/nv12_converter.hpp"

extern "C" {
#include <libavutil/imgutils.h>
}

#include <algorithm>
#include <stdexcept>

namespace od {
namespace {

// Keeps `frame` backed by padded, aligned storage for these dimensions; the
// buffer is reallocated only when the format or size changes.
void reserve(AVFrame& frame, const AVPixelFormat format, const int width, const int height) {
    if (frame.data[0] != nullptr && frame.format == format && frame.width == width
        && frame.height == height) {
        return;
    }
    av_frame_unref(&frame);
    frame.format = format;
    frame.width = width;
    frame.height = height;
    if (av_frame_get_buffer(&frame, 0) < 0) {
        throw std::runtime_error("cannot allocate the frame converter buffer");
    }
}

}  // namespace

Nv12Converter::Nv12Converter()
    : scaler_(sws_alloc_context()), source_(av_frame_alloc()), target_(av_frame_alloc()) {
    if (scaler_ == nullptr || source_ == nullptr || target_ == nullptr) {
        sws_free_context(&scaler_);
        av_frame_free(&source_);
        av_frame_free(&target_);
        throw std::runtime_error("cannot allocate the frame converter");
    }
    // Same filter as FFmpeg's `scale=W:H:flags=fast_bilinear,format=nv12`;
    // the frame properties are set per call, so a format change needs no
    // new context.
    scaler_->flags = SWS_FAST_BILINEAR;
    scaler_->threads = 0;
}

Nv12Converter::~Nv12Converter() {
    sws_free_context(&scaler_);
    av_frame_free(&source_);
    av_frame_free(&target_);
}

void Nv12Converter::stage(const Rgb32Image& image) {
    reserve(*source_, image.format, image.width, image.height);
    av_image_copy_plane(source_->data[0], source_->linesize[0], image.rows, image.stride,
                        image.width * 4, image.height);
}

std::string Nv12Converter::convert(const int outputWidth, const int outputHeight) {
    reserve(*target_, AV_PIX_FMT_NV12, outputWidth, outputHeight);
    if (sws_scale_frame(scaler_, target_, source_) < 0) {
        throw std::runtime_error("cannot convert the frame to NV12");
    }
    const int packedSize = av_image_get_buffer_size(AV_PIX_FMT_NV12, outputWidth, outputHeight, 1);
    // The copy below fills a buffer of exactly this size completely, so
    // zero-filling it first writes the 8 MB of a 2752x2064 frame twice; a
    // short copy leaves the tail unwritten and is reported as an empty string.
    std::string packed;
    packed.resize_and_overwrite(
        static_cast<std::size_t>(packedSize), [&](char* rows, const std::size_t size) {
            const int written = av_image_copy_to_buffer(
                reinterpret_cast<std::uint8_t*>(rows), static_cast<int>(size), target_->data,
                target_->linesize, AV_PIX_FMT_NV12, outputWidth, outputHeight, 1);
            return written == static_cast<int>(size) ? size : std::size_t{0};
        });
    if (packed.empty()) {
        throw std::runtime_error("cannot pack the converted frame as NV12");
    }
    return packed;
}

std::string blackNv12(const int width, const int height) {
    const int packedSize = av_image_get_buffer_size(AV_PIX_FMT_NV12, width, height, 1);
    std::string packed(static_cast<std::size_t>(packedSize), '\x80');
    std::fill_n(packed.begin(), static_cast<std::size_t>(width) * static_cast<std::size_t>(height),
                '\x10');
    return packed;
}

}  // namespace od
