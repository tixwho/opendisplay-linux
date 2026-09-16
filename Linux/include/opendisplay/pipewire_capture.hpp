#pragma once

#include "opendisplay/nv12_converter.hpp"
#include "opendisplay/types.hpp"

#include <pipewire/pipewire.h>
#include <spa/param/video/format-utils.h>

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <exception>
#include <functional>
#include <mutex>
#include <optional>
#include <string>

namespace od {

class PipeWireCapture {
public:
    using FrameCallback = std::function<void(CapturedFrame)>;

    PipeWireCapture() = default;
    ~PipeWireCapture();
    PipeWireCapture(const PipeWireCapture&) = delete;
    PipeWireCapture& operator=(const PipeWireCapture&) = delete;

    /// Delivers NV12 frames of `outputWidth` x `outputHeight`, converted and
    /// scaled from the negotiated `width` x `height` RGB stream.
    void start(int remoteFd, std::uint32_t nodeId, int width, int height, int outputWidth,
               int outputHeight, int fps, FrameCallback callback);
    void stop();
    [[nodiscard]] std::optional<std::string> error() const;

    // PipeWire's C callbacks are public only so the static event table can
    // reference them; callers should use start()/stop().
    static void stateChanged(void* data, pw_stream_state oldState, pw_stream_state state,
                             const char* error);
    static void parameterChanged(void* data, std::uint32_t id, const spa_pod* parameter);
    static void process(void* data);

private:
    /// One dequeued buffer whose pixels have left the PipeWire memory: the
    /// frame properties, plus either the rows staged in the converter or a
    /// chunk that was flagged empty. The NV12 bytes are made afterwards, with
    /// the buffer already back in PipeWire's hands.
    struct StagedFrame {
        CapturedFrame frame;
        bool neutral = false;  ///< the chunk was flagged empty: the picture is black
    };

    void handleProcess();
    /// Copies one dequeued buffer's rows into the converter; nothing for a
    /// buffer that fails the chunk contract or the copy.
    std::optional<StagedFrame> stageBuffer(const spa_buffer& buffer);
    /// Scales and packs a staged frame; nothing when libswscale rejects it.
    std::optional<CapturedFrame> convertStaged(StagedFrame staged);
    void recordConversionFailure(const std::exception& exception);

    pw_thread_loop* loop_ = nullptr;
    pw_context* context_ = nullptr;
    pw_core* core_ = nullptr;
    pw_stream* stream_ = nullptr;
    spa_hook listener_{};
    spa_video_info_raw format_{};
    std::optional<Nv12Converter> converter_;
    int outputWidth_ = 0;
    int outputHeight_ = 0;
    FrameCallback callback_;
    std::atomic<std::uint64_t> sequence_ = 0;
    mutable std::mutex stateMutex_;
    std::condition_variable stateCondition_;
    pw_stream_state state_ = PW_STREAM_STATE_UNCONNECTED;
    std::string error_;
};

}  // namespace od
