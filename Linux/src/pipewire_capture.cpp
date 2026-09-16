#include "opendisplay/pipewire_capture.hpp"

#include "opendisplay/log.hpp"
#include "opendisplay/pipewire_format.hpp"

#include <spa/param/format-utils.h>
#include <spa/param/video/raw.h>
#include <spa/utils/result.h>

#include <unistd.h>

#include <algorithm>
#include <chrono>
#include <cstring>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <variant>

namespace od {
namespace {

const pw_stream_events streamEvents = [] {
    pw_stream_events events{};
    events.version = PW_VERSION_STREAM_EVENTS;
    events.state_changed = PipeWireCapture::stateChanged;
    events.param_changed = PipeWireCapture::parameterChanged;
    events.process = PipeWireCapture::process;
    return events;
}();

std::optional<AVPixelFormat> pixelFormat(const spa_video_format format) {
    switch (format) {
    case SPA_VIDEO_FORMAT_BGRA: return AV_PIX_FMT_BGRA;
    case SPA_VIDEO_FORMAT_BGRx: return AV_PIX_FMT_BGR0;
    case SPA_VIDEO_FORMAT_RGBA: return AV_PIX_FMT_RGBA;
    case SPA_VIDEO_FORMAT_RGBx: return AV_PIX_FMT_RGB0;
    default: return std::nullopt;
    }
}

const char* pixelFormatName(const spa_video_format format) {
    switch (format) {
    case SPA_VIDEO_FORMAT_BGRA: return "BGRA";
    case SPA_VIDEO_FORMAT_BGRx: return "BGRx";
    case SPA_VIDEO_FORMAT_RGBA: return "RGBA";
    case SPA_VIDEO_FORMAT_RGBx: return "RGBx";
    default: return "unknown";
    }
}

}  // namespace

PipeWireCapture::~PipeWireCapture() { stop(); }

void PipeWireCapture::start(const int remoteFd, const std::uint32_t nodeId, const int width,
                            const int height, const int outputWidth, const int outputHeight,
                            const int fps, FrameCallback callback) {
    stop();
    {
        std::lock_guard lock(stateMutex_);
        state_ = PW_STREAM_STATE_UNCONNECTED;
        error_.clear();
    }
    static std::once_flag initialized;
    std::call_once(initialized, [] { pw_init(nullptr, nullptr); });

    callback_ = std::move(callback);
    outputWidth_ = outputWidth;
    outputHeight_ = outputHeight;
    try {
        converter_.emplace();
    } catch (...) {
        ::close(remoteFd);
        stop();
        throw;
    }
    loop_ = pw_thread_loop_new("opendisplay-capture", nullptr);
    if (loop_ == nullptr) {
        ::close(remoteFd);
        throw std::runtime_error("cannot create PipeWire thread loop");
    }
    context_ = pw_context_new(pw_thread_loop_get_loop(loop_), nullptr, 0);
    if (context_ == nullptr) {
        ::close(remoteFd);
        stop();
        throw std::runtime_error("cannot create PipeWire context");
    }
    core_ = pw_context_connect_fd(context_, remoteFd, nullptr, 0);
    if (core_ == nullptr) {
        ::close(remoteFd);
        stop();
        throw std::runtime_error("cannot connect to the portal PipeWire remote");
    }

    auto* properties = pw_properties_new(
        PW_KEY_MEDIA_TYPE, "Video",
        PW_KEY_MEDIA_CATEGORY, "Capture",
        PW_KEY_MEDIA_ROLE, "Screen",
        PW_KEY_APP_NAME, "OpenDisplay",
        nullptr);
    stream_ = pw_stream_new(core_, "OpenDisplay KDE capture", properties);
    if (stream_ == nullptr) {
        stop();
        throw std::runtime_error("cannot create PipeWire capture stream");
    }
    pw_stream_add_listener(stream_, &listener_, &streamEvents, this);

    std::uint8_t storage[1024]{};
    spa_pod_builder builder = SPA_POD_BUILDER_INIT(storage, sizeof(storage));
    const spa_pod* parameters[1];
    parameters[0] = buildPipeWireFormatOffer(builder, width, height, fps);

    const auto flags = static_cast<pw_stream_flags>(PW_STREAM_FLAG_AUTOCONNECT
        | PW_STREAM_FLAG_MAP_BUFFERS);
    const int result = pw_stream_connect(stream_, PW_DIRECTION_INPUT, nodeId, flags,
                                         parameters, 1);
    if (result < 0) {
        stop();
        throw std::runtime_error(std::string("cannot connect PipeWire stream: ")
                                 + spa_strerror(result));
    }
    if (pw_thread_loop_start(loop_) < 0) {
        stop();
        throw std::runtime_error("cannot start PipeWire thread loop");
    }

    std::unique_lock lock(stateMutex_);
    const bool configured = stateCondition_.wait_for(lock, std::chrono::seconds(10), [this] {
        return state_ == PW_STREAM_STATE_PAUSED || state_ == PW_STREAM_STATE_STREAMING
            || state_ == PW_STREAM_STATE_ERROR || !error_.empty();
    });
    if (!configured || state_ == PW_STREAM_STATE_ERROR || !error_.empty()) {
        const std::string failure = !configured
            ? "timed out while negotiating a video format"
            : (error_.empty() ? "unknown stream error" : error_);
        lock.unlock();
        stop();
        throw std::runtime_error("PipeWire capture failed: " + failure);
    }
}

void PipeWireCapture::stop() {
    if (loop_ != nullptr) {
        pw_thread_loop_stop(loop_);
    }
    if (stream_ != nullptr) {
        spa_hook_remove(&listener_);
        pw_stream_destroy(stream_);
        stream_ = nullptr;
    }
    if (core_ != nullptr) {
        pw_core_disconnect(core_);
        core_ = nullptr;
    }
    if (context_ != nullptr) {
        pw_context_destroy(context_);
        context_ = nullptr;
    }
    if (loop_ != nullptr) {
        pw_thread_loop_destroy(loop_);
        loop_ = nullptr;
    }
    converter_.reset();
    callback_ = {};
    format_ = {};
}

std::optional<std::string> PipeWireCapture::error() const {
    std::lock_guard lock(stateMutex_);
    if (error_.empty()) {
        return std::nullopt;
    }
    return error_;
}

void PipeWireCapture::stateChanged(void* data, pw_stream_state, const pw_stream_state state,
                                   const char* error) {
    auto& self = *static_cast<PipeWireCapture*>(data);
    {
        std::lock_guard lock(self.stateMutex_);
        self.state_ = state;
        if (state == PW_STREAM_STATE_ERROR) {
            self.error_ = error != nullptr ? error : "unknown stream error";
        }
    }
    self.stateCondition_.notify_all();
    if (state == PW_STREAM_STATE_ERROR) {
        log(std::string("PipeWire stream error: ") + (error != nullptr ? error : "unknown"));
    } else if (state == PW_STREAM_STATE_STREAMING) {
        log("PipeWire capture is streaming");
    }
}

void PipeWireCapture::parameterChanged(void* data, const std::uint32_t id,
                                       const spa_pod* parameter) {
    auto& self = *static_cast<PipeWireCapture*>(data);
    if (parameter == nullptr || id != SPA_PARAM_Format) {
        return;
    }
    spa_video_info_raw negotiated{};
    if (spa_format_video_raw_parse(parameter, &negotiated) < 0
        || !pixelFormat(negotiated.format)) {
        constexpr auto message = "PipeWire returned an unsupported video format";
        log(message);
        self.format_ = {};
        {
            std::lock_guard lock(self.stateMutex_);
            self.error_ = message;
        }
        self.stateCondition_.notify_all();
        return;
    }
    self.format_ = negotiated;
    log("PipeWire format: " + std::to_string(self.format_.size.width) + "x"
        + std::to_string(self.format_.size.height) + " "
        + pixelFormatName(self.format_.format) + " @ "
        + std::to_string(self.format_.framerate.num) + "/"
        + std::to_string(self.format_.framerate.denom) + " fps");
}

void PipeWireCapture::process(void* data) {
    static_cast<PipeWireCapture*>(data)->handleProcess();
}

void PipeWireCapture::handleProcess() {
    pw_buffer* pipewireBuffer = pw_stream_dequeue_buffer(stream_);
    if (pipewireBuffer == nullptr) {
        return;
    }
    // Staging copies the rows out of the mapped buffer, so the buffer goes
    // back to PipeWire on every path below, exactly once, before the scaling
    // starts: the stream keeps a free buffer to capture the next frame into
    // while this thread converts the last one.
    std::optional<StagedFrame> staged = stageBuffer(*pipewireBuffer->buffer);
    pw_stream_queue_buffer(stream_, pipewireBuffer);
    if (!staged) {
        return;
    }
    std::optional<CapturedFrame> frame = convertStaged(std::move(*staged));
    if (frame && callback_) {
        callback_(std::move(*frame));
    }
}

std::optional<PipeWireCapture::StagedFrame> PipeWireCapture::stageBuffer(
    const spa_buffer& buffer) {
    if (buffer.n_datas == 0 || format_.size.width == 0) {
        return std::nullopt;
    }
    const auto& data = buffer.datas[0];
    const auto width = static_cast<int>(format_.size.width);
    const auto height = static_cast<int>(format_.size.height);
    const auto chunk = rgbChunk(data, width, height);
    if (!chunk) {
        return std::nullopt;
    }

    StagedFrame staged;
    staged.frame.format = VideoFormat{
        .width = outputWidth_,
        .height = outputHeight_,
        .stride = outputWidth_,
        .fps = format_.framerate.denom > 0
            ? static_cast<int>(format_.framerate.num / format_.framerate.denom)
            : 60,
    };
    staged.frame.capturedAtMs = wallClockMs();
    staged.frame.sequence = sequence_.fetch_add(1);
    if (std::holds_alternative<NeutralChunk>(*chunk)) {
        // The plane holds no picture, so none of its bytes are read.
        staged.neutral = true;
        return staged;
    }

    // The converter copies the rows into its own padded storage, so the
    // mapped PipeWire buffer is never handed to libswscale directly.
    const auto& layout = std::get<RgbChunkLayout>(*chunk);
    const Rgb32Image image{
        .rows = static_cast<const std::uint8_t*>(data.data) + layout.offset,
        .stride = layout.stride,
        .width = width,
        .height = height,
        .format = *pixelFormat(format_.format),
    };
    try {
        converter_->stage(image);
    } catch (const std::exception& exception) {
        recordConversionFailure(exception);
        return std::nullopt;
    }
    return staged;
}

std::optional<CapturedFrame> PipeWireCapture::convertStaged(StagedFrame staged) {
    if (staged.neutral) {
        staged.frame.bytes = blackNv12(outputWidth_, outputHeight_);
        return std::move(staged.frame);
    }
    try {
        staged.frame.bytes = converter_->convert(outputWidth_, outputHeight_);
    } catch (const std::exception& exception) {
        recordConversionFailure(exception);
        return std::nullopt;
    }
    return std::move(staged.frame);
}

void PipeWireCapture::recordConversionFailure(const std::exception& exception) {
    log(std::string("PipeWire frame conversion failed: ") + exception.what());
    std::lock_guard lock(stateMutex_);
    error_ = exception.what();
}

}  // namespace od
