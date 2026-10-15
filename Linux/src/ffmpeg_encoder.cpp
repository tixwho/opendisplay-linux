#include "opendisplay/ffmpeg_encoder.hpp"

#include "opendisplay/log.hpp"

#include <fcntl.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

#include <algorithm>
#include <array>
#include <cerrno>
#include <csignal>
#include <cstdio>
#include <cstring>
#include <stdexcept>
#include <string_view>

namespace od {
namespace {

std::string pixelFormatName(const VideoFormat::PixelFormat format) {
    switch (format) {
    case VideoFormat::PixelFormat::Bgra: return "bgra";
    case VideoFormat::PixelFormat::Bgrx: return "bgr0";
    case VideoFormat::PixelFormat::Rgba: return "rgba";
    case VideoFormat::PixelFormat::Rgbx: return "rgb0";
    }
    return "bgra";
}

std::string encoderName(const EncoderKind kind) {
    switch (kind) {
    case EncoderKind::Vaapi: return "h264_vaapi";
    case EncoderKind::Nvenc: return "h264_nvenc";
    case EncoderKind::Software: return "libx264";
    case EncoderKind::Auto: break;
    }
    return "auto";
}

bool ffmpegHasEncoder(const std::string_view name) {
    std::array<char, 512> buffer{};
    std::string output;
    FILE* process = ::popen("ffmpeg -hide_banner -encoders 2>/dev/null", "r");
    if (process == nullptr) {
        return false;
    }
    while (::fgets(buffer.data(), static_cast<int>(buffer.size()), process) != nullptr) {
        output.append(buffer.data());
    }
    ::pclose(process);
    return output.find(name) != std::string::npos;
}

bool writeAll(const int fd, const std::string_view bytes) {
    std::size_t offset = 0;
    while (offset < bytes.size()) {
        const auto count = ::write(fd, bytes.data() + offset, bytes.size() - offset);
        if (count < 0) {
            if (errno == EINTR) {
                continue;
            }
            return false;
        }
        if (count == 0) {
            return false;
        }
        offset += static_cast<std::size_t>(count);
    }
    return true;
}

std::size_t startCodeLength(const std::string_view bytes, const std::size_t position) {
    if (position + 3 < bytes.size() && bytes[position] == 0 && bytes[position + 1] == 0
        && bytes[position + 2] == 0 && bytes[position + 3] == 1) {
        return 4;
    }
    return 3;
}

std::size_t findStartCode(const std::string_view bytes, const std::size_t from) {
    for (std::size_t index = from; index + 2 < bytes.size(); ++index) {
        if (bytes[index] == 0 && bytes[index + 1] == 0
            && (bytes[index + 2] == 1
                || (index + 3 < bytes.size() && bytes[index + 2] == 0
                    && bytes[index + 3] == 1))) {
            return index;
        }
    }
    return std::string::npos;
}

}  // namespace

FfmpegEncoder::~FfmpegEncoder() { stop(); }

void FfmpegEncoder::start(EncoderConfig config, FrameCallback callback) {
    stop();
    std::signal(SIGPIPE, SIG_IGN);
    config_ = std::move(config);
    callback_ = std::move(callback);
    selected_ = chooseEncoder();
    log("Using FFmpeg encoder " + encoderName(selected_));
    {
        std::lock_guard lock(mutex_);
        running_ = true;
        restartRequested_ = false;
    }
    worker_ = std::thread(&FfmpegEncoder::run, this);
}

void FfmpegEncoder::submit(CapturedFrame frame) {
    {
        std::lock_guard lock(mutex_);
        if (!running_) {
            return;
        }
        pending_ = std::move(frame);
    }
    condition_.notify_one();
}

void FfmpegEncoder::requestKeyframe() {
    {
        std::lock_guard lock(mutex_);
        restartRequested_ = true;
    }
    condition_.notify_one();
}

void FfmpegEncoder::stop() {
    {
        std::lock_guard lock(mutex_);
        running_ = false;
        pending_.reset();
    }
    condition_.notify_all();
    if (worker_.joinable()) {
        worker_.join();
    }
    callback_ = {};
}

std::string FfmpegEncoder::selectedEncoder() const { return encoderName(selected_); }

EncoderKind FfmpegEncoder::chooseEncoder() const {
    if (config_.kind != EncoderKind::Auto) {
        if (!ffmpegHasEncoder(encoderName(config_.kind))) {
            throw std::runtime_error("FFmpeg does not provide " + encoderName(config_.kind));
        }
        return config_.kind;
    }
    if (::access(config_.vaapiDevice.c_str(), R_OK | W_OK) == 0
        && ffmpegHasEncoder("h264_vaapi")) {
        return EncoderKind::Vaapi;
    }
    if (::access("/dev/nvidiactl", F_OK) == 0 && ffmpegHasEncoder("h264_nvenc")) {
        return EncoderKind::Nvenc;
    }
    if (ffmpegHasEncoder("libx264")) {
        return EncoderKind::Software;
    }
    throw std::runtime_error("FFmpeg has no supported H.264 encoder");
}

std::vector<std::string> FfmpegEncoder::arguments(const VideoFormat& input) const {
    const int outputWidth = config_.outputWidth > 0 ? config_.outputWidth : input.width;
    const int outputHeight = config_.outputHeight > 0 ? config_.outputHeight : input.height;
    const std::string size = std::to_string(input.width) + "x" + std::to_string(input.height);
    const std::string rate = std::to_string(std::max(1, config_.fps));
    std::vector<std::string> args{
        "ffmpeg", "-hide_banner", "-loglevel", "warning", "-nostdin",
    };
    if (selected_ == EncoderKind::Vaapi) {
        args.insert(args.end(), {"-vaapi_device", config_.vaapiDevice});
    }
    args.insert(args.end(), {
        "-f", "rawvideo", "-pixel_format", pixelFormatName(input.pixelFormat),
        "-video_size", size, "-framerate", rate, "-i", "pipe:0", "-an",
    });

    const std::string scale = outputWidth == input.width && outputHeight == input.height
        ? std::string{}
        : "scale=" + std::to_string(outputWidth) + ':' + std::to_string(outputHeight)
            + ":flags=fast_bilinear";
    if (selected_ == EncoderKind::Vaapi) {
        // Many Mesa VA-API drivers (including Intel iHD and AMD radeonsi) only
        // expose CQP. Passing -b:v makes FFmpeg infer VBR and the encoder then
        // exits before producing a frame. Use a predictable quality target
        // supported by the common CQP-only path instead.
        const auto filter = (scale.empty() ? std::string{} : scale + ',')
            + "format=nv12,hwupload";
        // Keep the VA-API queue to one frame. FFmpeg defaults to two frames
        // of parallelism, which adds visible latency for an interactive display.
        args.insert(args.end(), {"-vf", filter, "-c:v", "h264_vaapi", "-async_depth", "1",
                                 "-rc_mode", "CQP", "-qp", "23"});
    } else if (selected_ == EncoderKind::Nvenc) {
        args.insert(args.end(), {"-c:v", "h264_nvenc", "-preset", "p1",
                                 "-tune", "ull", "-delay", "0"});
        if (!scale.empty()) {
            args.insert(args.end(), {"-vf", scale});
        }
    } else {
        args.insert(args.end(), {"-c:v", "libx264", "-preset", "ultrafast",
                                 "-tune", "zerolatency",
                                 "-x264-params", "repeat-headers=1:aud=1"});
        if (!scale.empty()) {
            args.insert(args.end(), {"-vf", scale});
        }
    }
    args.insert(args.end(), {"-bf", "0", "-g",
                             std::to_string(std::max(config_.fps * 60, config_.fps))});
    if (selected_ != EncoderKind::Vaapi) {
        args.insert(args.end(), {"-b:v", std::to_string(config_.bitrate), "-maxrate",
                                 std::to_string(config_.bitrate), "-bufsize",
                                 std::to_string(std::max(config_.bitrate / 2, 1))});
    }
    args.insert(args.end(), {"-bsf:v", "h264_metadata=aud=insert,dump_extra=freq=keyframe",
                             "-f", "h264", "pipe:1"});
    return args;
}

void FfmpegEncoder::startProcess(const VideoFormat& input) {
    int inputPipe[2]{};
    int outputPipe[2]{};
    if (::pipe2(inputPipe, O_CLOEXEC) != 0 || ::pipe2(outputPipe, O_CLOEXEC) != 0) {
        if (inputPipe[0] > 0) ::close(inputPipe[0]);
        if (inputPipe[1] > 0) ::close(inputPipe[1]);
        throw std::runtime_error("cannot create FFmpeg pipes");
    }
    const auto args = arguments(input);
    const pid_t pid = ::fork();
    if (pid < 0) {
        ::close(inputPipe[0]); ::close(inputPipe[1]);
        ::close(outputPipe[0]); ::close(outputPipe[1]);
        throw std::runtime_error("cannot fork FFmpeg");
    }
    if (pid == 0) {
        ::dup2(inputPipe[0], STDIN_FILENO);
        ::dup2(outputPipe[1], STDOUT_FILENO);
        ::close(inputPipe[0]); ::close(inputPipe[1]);
        ::close(outputPipe[0]); ::close(outputPipe[1]);
        std::vector<char*> argv;
        argv.reserve(args.size() + 1);
        for (const auto& argument : args) {
            argv.push_back(const_cast<char*>(argument.c_str()));
        }
        argv.push_back(nullptr);
        ::execvp(argv.front(), argv.data());
        _exit(127);
    }
    ::close(inputPipe[0]);
    ::close(outputPipe[1]);
    inputFd_ = inputPipe[1];
    outputFd_ = outputPipe[0];
    childPid_ = static_cast<int>(pid);
    inputFormat_ = input;
    reader_ = std::thread(&FfmpegEncoder::readOutput, this, outputFd_);
}

void FfmpegEncoder::stopProcess() {
    if (inputFd_ >= 0) {
        ::close(inputFd_);
        inputFd_ = -1;
    }
    if (reader_.joinable()) {
        reader_.join();
    }
    outputFd_ = -1;
    if (childPid_ > 0) {
        int status = 0;
        while (::waitpid(childPid_, &status, 0) < 0 && errno == EINTR) {}
        childPid_ = -1;
    }
    inputFormat_ = {};
    {
        std::lock_guard lock(mutex_);
        timestamps_.clear();
    }
}

void FfmpegEncoder::run() {
    try {
        for (;;) {
            CapturedFrame frame;
            bool restart = false;
            {
                std::unique_lock lock(mutex_);
                condition_.wait(lock, [&] { return !running_ || pending_.has_value(); });
                if (!running_) {
                    break;
                }
                frame = std::move(*pending_);
                pending_.reset();
                restart = restartRequested_;
                restartRequested_ = false;
            }
            const bool formatChanged = inputFd_ >= 0
                && (frame.format.width != inputFormat_.width
                    || frame.format.height != inputFormat_.height
                    || frame.format.pixelFormat != inputFormat_.pixelFormat);
            if (restart || formatChanged) {
                stopProcess();
            }
            if (inputFd_ < 0) {
                startProcess(frame.format);
            }
            {
                std::lock_guard lock(mutex_);
                timestamps_.push_back(frame.capturedAtMs);
            }
            if (!writeAll(inputFd_, frame.bytes)) {
                log("FFmpeg stopped accepting frames");
                stopProcess();
                if (config_.kind == EncoderKind::Auto
                    && selected_ == EncoderKind::Vaapi) {
                    selected_ = EncoderKind::Software;
                    log("VA-API failed at runtime; falling back to libx264");
                }
                startProcess(frame.format);
                {
                    std::lock_guard lock(mutex_);
                    timestamps_.push_back(frame.capturedAtMs);
                }
                if (!writeAll(inputFd_, frame.bytes)) {
                    throw std::runtime_error("FFmpeg encoder pipe failed");
                }
            }
        }
    } catch (const std::exception& error) {
        log(std::string("Encoder error: ") + error.what());
        std::lock_guard lock(mutex_);
        running_ = false;
    }
    stopProcess();
}

void FfmpegEncoder::readOutput(const int fd) {
    std::array<char, 64 * 1024> buffer{};
    std::string pending;
    std::string accessUnit;
    bool hasVcl = false;
    for (;;) {
        const auto count = ::read(fd, buffer.data(), buffer.size());
        if (count < 0 && errno == EINTR) {
            continue;
        }
        if (count <= 0) {
            break;
        }
        pending.append(buffer.data(), static_cast<std::size_t>(count));
        for (;;) {
            const auto first = findStartCode(pending, 0);
            if (first == std::string::npos) {
                break;
            }
            if (first > 0) {
                pending.erase(0, first);
            }
            const auto next = findStartCode(pending, startCodeLength(pending, 0));
            if (next == std::string::npos) {
                break;
            }
            auto nal = pending.substr(0, next);
            pending.erase(0, next);
            consumeNal(std::move(nal), accessUnit, hasVcl);
        }
    }
    if (!pending.empty() && findStartCode(pending, 0) == 0) {
        consumeNal(std::move(pending), accessUnit, hasVcl);
    }
    if (hasVcl && !accessUnit.empty()) {
        emitAccessUnit(std::move(accessUnit));
    }
    ::close(fd);
}

void FfmpegEncoder::consumeNal(std::string nal, std::string& accessUnit, bool& hasVcl) {
    const auto prefix = startCodeLength(nal, 0);
    if (nal.size() <= prefix) {
        return;
    }
    const int type = static_cast<unsigned char>(nal[prefix]) & 0x1f;
    // FFmpeg's Annex-B muxer may mix three- and four-byte delimiters. The
    // existing iOS receiver intentionally recognizes only the four-byte form,
    // matching the macOS sender, so normalize every NAL before transmission.
    if (prefix == 3) {
        nal.insert(nal.begin(), '\0');
    }
    if (type == 9 && hasVcl) {
        emitAccessUnit(std::move(accessUnit));
        accessUnit.clear();
        hasVcl = false;
    }
    accessUnit.append(nal);
    hasVcl = hasVcl || (type >= 1 && type <= 5);
}

void FfmpegEncoder::emitAccessUnit(std::string accessUnit) {
    std::int64_t timestamp = wallClockMs();
    {
        std::lock_guard lock(mutex_);
        if (!timestamps_.empty()) {
            timestamp = timestamps_.front();
            timestamps_.pop_front();
        }
    }
    bool keyframe = false;
    for (std::size_t position = findStartCode(accessUnit, 0); position != std::string::npos;) {
        const auto prefix = startCodeLength(accessUnit, position);
        if (position + prefix < accessUnit.size()
            && (static_cast<unsigned char>(accessUnit[position + prefix]) & 0x1f) == 5) {
            keyframe = true;
            break;
        }
        position = findStartCode(accessUnit, position + prefix);
    }
    if (callback_) {
        callback_(EncodedFrame{.capturedAtMs = timestamp, .keyframe = keyframe,
                               .annexB = std::move(accessUnit)});
    }
}

}  // namespace od
