#include "opendisplay/pipewire_capture.hpp"

#include "opendisplay/log.hpp"
#include "opendisplay/pipewire_format.hpp"

#include <spa/param/format-utils.h>
#include <spa/param/param.h>
#include <spa/buffer/meta.h>
#include <spa/param/video/raw.h>
#include <spa/utils/result.h>

#include <unistd.h>

#include <algorithm>
#include <chrono>
#include <cstring>
#include <mutex>
#include <optional>
#include <stdexcept>

namespace od {
namespace {

constexpr int maxCursorBitmapDimension = 384;

const pw_stream_events streamEvents = [] {
    pw_stream_events events{};
    events.version = PW_VERSION_STREAM_EVENTS;
    events.state_changed = PipeWireCapture::stateChanged;
    events.param_changed = PipeWireCapture::parameterChanged;
    events.process = PipeWireCapture::process;
    return events;
}();

std::optional<VideoFormat::PixelFormat> pixelFormat(const spa_video_format format) {
    switch (format) {
    case SPA_VIDEO_FORMAT_BGRA: return VideoFormat::PixelFormat::Bgra;
    case SPA_VIDEO_FORMAT_BGRx: return VideoFormat::PixelFormat::Bgrx;
    case SPA_VIDEO_FORMAT_RGBA: return VideoFormat::PixelFormat::Rgba;
    case SPA_VIDEO_FORMAT_RGBx: return VideoFormat::PixelFormat::Rgbx;
    default: return std::nullopt;
    }
}

int cursorMetaSize(const int width, const int height) {
    return static_cast<int>(sizeof(spa_meta_cursor) + sizeof(spa_meta_bitmap))
        + width * height * 4;
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
                            const int height, const int fps, FrameCallback callback,
                            const bool cursorMetadata, CursorCallback cursorCallback) {
    stop();
    {
        std::lock_guard lock(stateMutex_);
        state_ = PW_STREAM_STATE_UNCONNECTED;
        error_.clear();
    }
    static std::once_flag initialized;
    std::call_once(initialized, [] { pw_init(nullptr, nullptr); });

    callback_ = std::move(callback);
    cursorCallback_ = std::move(cursorCallback);
    cursorMetadataEnabled_ = cursorMetadata;
    cursorMetadataRequested_ = false;
    cursorMetadataSeen_ = false;
    bufferStorageLogged_ = false;
    captureWidth_ = width;
    captureHeight_ = height;
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
    core_ = remoteFd >= 0 ? pw_context_connect_fd(context_, remoteFd, nullptr, 0)
                          : pw_context_connect(context_, nullptr, 0);
    if (core_ == nullptr) {
        if (remoteFd >= 0) ::close(remoteFd);
        stop();
        throw std::runtime_error("cannot connect to the PipeWire server");
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
    callback_ = {};
    cursorCallback_ = {};
    cursorMetadataRequested_ = false;
    cursorMetadataEnabled_ = false;
    cursorMetadataSeen_ = false;
    bufferStorageLogged_ = false;
    captureWidth_ = 0;
    captureHeight_ = 0;
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
    if (self.cursorMetadataEnabled_ && !self.cursorMetadataRequested_) {
        self.requestCursorMetadata();
    }
}

void PipeWireCapture::requestCursorMetadata() {
    std::uint8_t storage[128]{};
    spa_pod_builder builder = SPA_POD_BUILDER_INIT(storage, sizeof(storage));
    const spa_pod* parameters[1];
    parameters[0] = static_cast<const spa_pod*>(spa_pod_builder_add_object(
        &builder, SPA_TYPE_OBJECT_ParamMeta, SPA_PARAM_Meta,
        SPA_PARAM_META_type, SPA_POD_Id(SPA_META_Cursor),
        SPA_PARAM_META_size, SPA_POD_CHOICE_RANGE_Int(
            cursorMetaSize(64, 64), cursorMetaSize(1, 1),
            cursorMetaSize(maxCursorBitmapDimension, maxCursorBitmapDimension))));
    const int result = pw_stream_update_params(stream_, parameters, 1);
    if (result < 0) {
        debug(std::string("PipeWire cursor metadata request failed: ") + spa_strerror(result));
        return;
    }
    cursorMetadataRequested_ = true;
    debug("Requested PipeWire cursor metadata");
}

void PipeWireCapture::process(void* data) {
    static_cast<PipeWireCapture*>(data)->handleProcess();
}

void PipeWireCapture::handleProcess() {
    pw_buffer* pipewireBuffer = pw_stream_dequeue_buffer(stream_);
    if (pipewireBuffer == nullptr) {
        return;
    }
    auto* buffer = pipewireBuffer->buffer;
    if (buffer->n_datas == 0 || buffer->datas[0].data == nullptr
        || buffer->datas[0].chunk == nullptr || format_.size.width == 0) {
        pw_stream_queue_buffer(stream_, pipewireBuffer);
        return;
    }

    const auto& data = buffer->datas[0];
    const auto* chunk = data.chunk;
    if (!bufferStorageLogged_) {
        const char* storage = "unknown";
        switch (data.type) {
        case SPA_DATA_MemPtr: storage = "MemPtr"; break;
        case SPA_DATA_MemFd: storage = "MemFd"; break;
        case SPA_DATA_DmaBuf: storage = "DmaBuf"; break;
        case SPA_DATA_MemId: storage = "MemId"; break;
        default: break;
        }
        debug(std::string("PipeWire buffer storage: ") + storage + ", stride "
              + std::to_string(chunk->stride) + ", flags " + std::to_string(data.flags));
        bufferStorageLogged_ = true;
    }
    const auto width = static_cast<int>(format_.size.width);
    const auto height = static_cast<int>(format_.size.height);
    const int sourceStride = chunk->stride > 0 ? chunk->stride : width * 4;
    const int targetStride = width * 4;
    const auto required = static_cast<std::uint64_t>(chunk->offset)
        + static_cast<std::uint64_t>(sourceStride) * static_cast<std::uint64_t>(height - 1)
        + static_cast<std::uint64_t>(targetStride);
    if (sourceStride < targetStride || required > data.maxsize) {
        pw_stream_queue_buffer(stream_, pipewireBuffer);
        return;
    }
    const auto* source = static_cast<const char*>(data.data) + chunk->offset;

    if (cursorMetadataEnabled_ && cursorCallback_) {
        auto* cursor = static_cast<spa_meta_cursor*>(spa_buffer_find_meta_data(
            buffer, SPA_META_Cursor, sizeof(spa_meta_cursor)));
        if (cursor != nullptr) {
            if (!cursorMetadataSeen_) {
                log("PipeWire cursor metadata is available");
                cursorMetadataSeen_ = true;
            }
            CursorUpdate update;
            update.x = cursor->position.x;
            update.y = cursor->position.y;
            update.visible = cursor->id != 0 && update.x >= 0 && update.y >= 0
                && update.x < captureWidth_ && update.y < captureHeight_;

            if (cursor->id != 0 && cursor->bitmap_offset >= sizeof(spa_meta_cursor)) {
                auto* bitmap = SPA_PTROFF(cursor, cursor->bitmap_offset, spa_meta_bitmap);
                const auto bitmapFormat = pixelFormat(
                    static_cast<spa_video_format>(bitmap->format));
                const int cursorWidth = static_cast<int>(bitmap->size.width);
                const int cursorHeight = static_cast<int>(bitmap->size.height);
                if (bitmapFormat && cursorWidth > 0 && cursorHeight > 0
                    && cursorWidth <= maxCursorBitmapDimension
                    && cursorHeight <= maxCursorBitmapDimension
                    && bitmap->stride >= cursorWidth * 4
                    && bitmap->offset >= sizeof(spa_meta_bitmap)
                    && static_cast<std::size_t>(cursor->bitmap_offset)
                        + static_cast<std::size_t>(bitmap->offset)
                        + static_cast<std::size_t>(bitmap->stride) * cursorHeight
                        <= static_cast<std::size_t>(cursorMetaSize(
                            maxCursorBitmapDimension, maxCursorBitmapDimension))) {
                    const auto* pixels = SPA_PTROFF(bitmap, bitmap->offset, std::uint8_t);
                    const auto rowBytes = static_cast<std::size_t>(cursorWidth) * 4;
                    const auto stride = static_cast<std::size_t>(bitmap->stride);
                    CursorBitmap sprite{
                        .width = cursorWidth,
                        .height = cursorHeight,
                        .stride = static_cast<int>(rowBytes),
                        .hotspotX = cursor->hotspot.x,
                        .hotspotY = cursor->hotspot.y,
                        .pixelFormat = *bitmapFormat,
                        .bytes = std::vector<std::uint8_t>(rowBytes * cursorHeight),
                    };
                    for (int row = 0; row < cursorHeight; ++row) {
                        std::memcpy(sprite.bytes.data() + static_cast<std::size_t>(row) * rowBytes,
                                    pixels + static_cast<std::size_t>(row) * stride, rowBytes);
                    }
                    update.bitmap = std::move(sprite);
                }
            }
            cursorCallback_(std::move(update));
        }
    }

    CapturedFrame frame;
    frame.format = VideoFormat{
        .width = width,
        .height = height,
        .stride = targetStride,
        .fps = format_.framerate.denom > 0
            ? static_cast<int>(format_.framerate.num / format_.framerate.denom)
            : 60,
        .pixelFormat = *pixelFormat(format_.format),
    };
    frame.capturedAtMs = wallClockMs();
    frame.sequence = sequence_.fetch_add(1);
    frame.bytes.resize(static_cast<std::size_t>(targetStride * height));
    if (sourceStride == targetStride) {
        // Mutter's tightly packed frames are contiguous. One bulk copy avoids
        // thousands of small memcpy calls on every high-resolution frame.
        std::memcpy(frame.bytes.data(), source,
                    static_cast<std::size_t>(targetStride) * height);
    } else {
        for (int row = 0; row < height; ++row) {
            std::memcpy(frame.bytes.data() + static_cast<std::size_t>(row) * targetStride,
                        source + static_cast<std::size_t>(row) * sourceStride,
                        static_cast<std::size_t>(targetStride));
        }
    }
    pw_stream_queue_buffer(stream_, pipewireBuffer);
    if (callback_) {
        callback_(std::move(frame));
    }
}

}  // namespace od
