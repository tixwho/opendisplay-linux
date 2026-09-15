#pragma once

#include "opendisplay/types.hpp"

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <functional>
#include <exception>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <vector>

namespace od {

struct EncoderConfig {
    EncoderKind kind = EncoderKind::Auto;
    std::string vaapiDevice = "/dev/dri/renderD128";
    int outputWidth = 0;
    int outputHeight = 0;
    int fps = 60;
    int bitrate = 18'000'000;
};

/// Low-latency FFmpeg subprocess adapter. Capture threads only replace a
/// single pending frame, preventing latency from growing under encoder load.
/// FFmpeg writes packetized NUT, so every access unit is delivered as soon as
/// its packet is complete, without waiting for the next frame.
class FfmpegEncoder {
public:
    using FrameCallback = std::function<void(EncodedFrame)>;

    FfmpegEncoder() = default;
    ~FfmpegEncoder();
    FfmpegEncoder(const FfmpegEncoder&) = delete;
    FfmpegEncoder& operator=(const FfmpegEncoder&) = delete;

    void start(EncoderConfig config, FrameCallback callback);
    void submit(CapturedFrame frame);
    void requestKeyframe();
    void stop();
    [[nodiscard]] std::string selectedEncoder() const;
    /// Set when the encoder gave up after a fatal FFmpeg error; empty otherwise.
    [[nodiscard]] std::string failure() const;

private:
    void run();
    void fail(std::exception_ptr error);
    void startProcess(const VideoFormat& input);
    void stopProcess();
    void readOutput(int fd);
    void emitPacket(std::string annexB, bool keyframe);
    std::vector<std::string> arguments(const VideoFormat& input) const;
    EncoderKind chooseEncoder() const;

    EncoderConfig config_;
    FrameCallback callback_;
    mutable std::mutex mutex_;
    std::condition_variable condition_;
    std::optional<CapturedFrame> pending_;
    std::deque<std::int64_t> timestamps_;
    std::thread worker_;
    std::thread reader_;
    bool running_ = false;
    bool processStopping_ = false;
    bool restartRequested_ = false;
    int inputFd_ = -1;
    int outputFd_ = -1;
    int childPid_ = -1;
    VideoFormat inputFormat_;
    EncoderKind selected_ = EncoderKind::Software;
    std::exception_ptr failure_;
};

}  // namespace od
