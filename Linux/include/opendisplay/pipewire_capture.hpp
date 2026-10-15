#pragma once

#include "opendisplay/types.hpp"

#include <pipewire/pipewire.h>
#include <spa/param/video/format-utils.h>

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <functional>
#include <mutex>
#include <optional>
#include <string>

namespace od {

class PipeWireCapture {
public:
    using FrameCallback = std::function<void(CapturedFrame)>;
    using CursorCallback = std::function<void(CursorUpdate)>;

    PipeWireCapture() = default;
    ~PipeWireCapture();
    PipeWireCapture(const PipeWireCapture&) = delete;
    PipeWireCapture& operator=(const PipeWireCapture&) = delete;

    void start(int remoteFd, std::uint32_t nodeId, int width, int height, int fps,
               FrameCallback callback, bool cursorMetadata = false,
               CursorCallback cursorCallback = {});
    void stop();
    [[nodiscard]] std::optional<std::string> error() const;

    // PipeWire's C callbacks are public only so the static event table can
    // reference them; callers should use start()/stop().
    static void stateChanged(void* data, pw_stream_state oldState, pw_stream_state state,
                             const char* error);
    static void parameterChanged(void* data, std::uint32_t id, const spa_pod* parameter);
    static void process(void* data);

private:
    void handleProcess();
    void requestCursorMetadata();

    pw_thread_loop* loop_ = nullptr;
    pw_context* context_ = nullptr;
    pw_core* core_ = nullptr;
    pw_stream* stream_ = nullptr;
    spa_hook listener_{};
    spa_video_info_raw format_{};
    FrameCallback callback_;
    CursorCallback cursorCallback_;
    bool cursorMetadataRequested_ = false;
    bool cursorMetadataEnabled_ = false;
    bool cursorMetadataSeen_ = false;
    bool bufferStorageLogged_ = false;
    int captureWidth_ = 0;
    int captureHeight_ = 0;
    std::atomic<std::uint64_t> sequence_ = 0;
    mutable std::mutex stateMutex_;
    std::condition_variable stateCondition_;
    pw_stream_state state_ = PW_STREAM_STATE_UNCONNECTED;
    std::string error_;
};

}  // namespace od
