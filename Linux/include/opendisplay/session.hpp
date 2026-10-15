#pragma once

#include "opendisplay/desktop_backend.hpp"
#include "opendisplay/ffmpeg_encoder.hpp"
#include "opendisplay/pipewire_capture.hpp"
#include "opendisplay/socket.hpp"
#include "opendisplay/types.hpp"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <deque>
#include <mutex>
#include <memory>
#include <optional>
#include <string>
#include <thread>
#include <utility>

namespace od {

class Session {
public:
    Session(Options options, std::unique_ptr<DesktopBackend> desktop);
    ~Session();
    Session(const Session&) = delete;
    Session& operator=(const Session&) = delete;

    void start(const Endpoint& endpoint);
    /// Called by the Qt main loop. Returns false after disconnection.
    bool tick();
    void stop();

private:
    enum class EventKind { Hello, Touch, Scroll };
    struct Event {
        EventKind kind = EventKind::Touch;
        PhoneInfo phone;
        std::string phase;
        double x = 0;
        double y = 0;
    };

    void receiveLoop();
    void startPipeline(const PhoneInfo& phone);
    void stopPipeline();
    bool send(std::string_view payload);
    void queue(Event event);
    void queueCursor(CursorUpdate update);
    void sendCursorUpdate(const CursorUpdate& update);

    Options options_;
    Socket socket_;
    std::unique_ptr<DesktopBackend> desktop_;
    PipeWireCapture capture_;
    FfmpegEncoder encoder_;
    std::thread receiver_;
    std::atomic_bool connected_ = false;
    std::mutex sendMutex_;
    std::mutex stateMutex_;
    std::condition_variable helloCondition_;
    std::optional<PhoneInfo> phone_;
    std::optional<PhoneInfo> activePhone_;
    std::deque<Event> events_;
    std::optional<CursorUpdate> pendingCursor_;
    std::optional<CursorBitmap> cursorBitmap_;
    bool cursorBitmapDirty_ = false;
    std::optional<std::pair<int, int>> lastCursorPosition_;
    bool cursorVisible_ = false;
    int captureWidth_ = 0;
    int captureHeight_ = 0;
    std::chrono::steady_clock::time_point lastPing_{};
    std::chrono::steady_clock::time_point reconfigureAfter_{};
    bool pipelineRunning_ = false;
    bool reconfigurePending_ = false;
};

}  // namespace od
