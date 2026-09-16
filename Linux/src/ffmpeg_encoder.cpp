#include "opendisplay/ffmpeg_encoder.hpp"

#include "opendisplay/log.hpp"

extern "C" {
#include <libavcodec/packet.h>
#include <libavformat/avformat.h>
#include <libavformat/avio.h>
#include <libavutil/mem.h>
}

#include <fcntl.h>
#include <spawn.h>
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

extern char** environ;

namespace od {
namespace {

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

// FFmpeg's bitstream filters mix three- and four-byte delimiters. The iOS
// receiver recognizes only the four-byte form, matching the macOS sender.
std::string withFourByteStartCodes(const std::string_view packet) {
    std::string annexB;
    annexB.reserve(packet.size() + 16);
    std::size_t position = findStartCode(packet, 0);
    while (position != std::string::npos) {
        if (packet[position + 2] == 1) {
            annexB.push_back('\0');
        }
        const auto next = findStartCode(packet, position + 3);
        annexB.append(packet.substr(position, next - position));
        position = next;
    }
    return annexB;
}

/// Starts `argv` with `stdinFd` and `stdoutFd` as its standard streams. Every
/// other descriptor of this process is close-on-exec. Returns an errno value.
int spawn(char* const argv[], const int stdinFd, const int stdoutFd, pid_t& pid) {
    posix_spawn_file_actions_t actions;
    int error = ::posix_spawn_file_actions_init(&actions);
    if (error != 0) {
        return error;
    }
    // A pipe end that already sits on 0 or 1 keeps its number; dup2 onto the
    // same descriptor only clears close-on-exec.
    error = ::posix_spawn_file_actions_adddup2(&actions, stdinFd, STDIN_FILENO);
    if (error == 0) {
        error = ::posix_spawn_file_actions_adddup2(&actions, stdoutFd, STDOUT_FILENO);
    }
    if (error == 0) {
        error = ::posix_spawnp(&pid, argv[0], &actions, nullptr, argv, environ);
    }
    ::posix_spawn_file_actions_destroy(&actions);
    return error;
}

int readPipe(void* opaque, std::uint8_t* buffer, const int size) {
    const int fd = static_cast<int>(reinterpret_cast<std::intptr_t>(opaque));
    for (;;) {
        const auto count = ::read(fd, buffer, static_cast<std::size_t>(size));
        if (count < 0 && errno == EINTR) {
            continue;
        }
        if (count < 0) {
            return AVERROR(errno);
        }
        if (count == 0) {
            return AVERROR_EOF;
        }
        return static_cast<int>(count);
    }
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
        failure_ = nullptr;
    }
    worker_ = std::thread(&FfmpegEncoder::run, this);
}

void FfmpegEncoder::submit(CapturedFrame frame) {
    {
        std::lock_guard lock(mutex_);
        if (!running_) {
            return;
        }
        if (pending_) {
            ++dropped_;
        }
        pending_ = std::move(frame);
    }
    condition_.notify_one();
}

std::uint64_t FfmpegEncoder::droppedFrames() const {
    std::lock_guard lock(mutex_);
    return dropped_;
}

int FfmpegEncoder::pendingFrames() const {
    std::lock_guard lock(mutex_);
    return static_cast<int>(timestamps_.size()) + (pending_ ? 1 : 0);
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
        if (childPid_ > 0) {
            ::kill(childPid_, SIGKILL);
        }
    }
    condition_.notify_all();
    if (worker_.joinable()) {
        worker_.join();
    }
    callback_ = {};
}

std::string FfmpegEncoder::selectedEncoder() const { return encoderName(selected_); }

std::string FfmpegEncoder::failure() const {
    std::exception_ptr error;
    {
        std::lock_guard lock(mutex_);
        error = failure_;
    }
    if (!error) {
        return {};
    }
    try {
        std::rethrow_exception(error);
    } catch (const std::exception& failure) {
        return failure.what();
    } catch (...) {
        return "Unknown encoder error";
    }
}

void FfmpegEncoder::fail(std::exception_ptr error) {
    {
        std::lock_guard lock(mutex_);
        if (!running_ || processStopping_) {
            return;
        }
        // Preserve the exception without allocating from a failing reader thread.
        failure_ = std::move(error);
        running_ = false;
        pending_.reset();
        if (childPid_ > 0) {
            ::kill(childPid_, SIGKILL);
        }
    }
    condition_.notify_all();
}

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
    const std::string size = std::to_string(input.width) + "x" + std::to_string(input.height);
    const std::string rate = std::to_string(std::max(1, config_.fps));
    std::vector<std::string> args{
        "ffmpeg", "-hide_banner", "-loglevel", "warning", "-nostdin",
    };
    if (selected_ == EncoderKind::Vaapi) {
        args.insert(args.end(), {"-vaapi_device", config_.vaapiDevice});
    }
    // Frames arrive as NV12 at the output size, the 8-bit 4:2:0 layout every
    // encoder takes directly and the iPad hardware decoder requires.
    args.insert(args.end(), {
        "-f", "rawvideo", "-pixel_format", "nv12", "-video_size", size, "-framerate", rate,
        "-i", "pipe:0", "-an",
    });
    if (selected_ == EncoderKind::Vaapi) {
        args.insert(args.end(), {"-vf", "hwupload", "-c:v", "h264_vaapi", "-async_depth", "1"});
    } else if (selected_ == EncoderKind::Nvenc) {
        args.insert(args.end(), {"-c:v", "h264_nvenc", "-preset", "p1", "-tune", "ull",
                                 "-delay", "0"});
    } else {
        args.insert(args.end(), {"-c:v", "libx264", "-preset", "ultrafast", "-tune",
                                 "zerolatency"});
    }
    // NUT carries packet boundaries, so the reader emits each access unit as
    // soon as FFmpeg flushes it instead of waiting for the next start code.
    // NUT takes SPS and PPS as global headers. libx264 and NVENC then leave
    // them out of the stream, so dump_extra puts that one copy in front of
    // every keyframe; VA-API writes its own copy in front of every IDR.
    std::string filters = "h264_metadata=aud=insert";
    if (selected_ != EncoderKind::Vaapi) {
        filters += ",dump_extra=freq=keyframe";
    }
    args.insert(args.end(), {
        "-bf", "0", "-g", std::to_string(std::max(config_.fps * 60, config_.fps)),
        "-b:v", std::to_string(config_.bitrate), "-maxrate", std::to_string(config_.bitrate),
        "-bufsize", std::to_string(std::max(config_.bitrate / 2, 1)),
        "-bsf:v", filters, "-f", "nut", "-flush_packets", "1", "pipe:1",
    });
    return args;
}

void FfmpegEncoder::startProcess(const VideoFormat& input) {
    std::lock_guard lock(mutex_);
    if (!running_) {
        return;
    }
    processStopping_ = false;
    int inputPipe[2]{};
    int outputPipe[2]{};
    if (::pipe2(inputPipe, O_CLOEXEC) != 0) {
        throw std::runtime_error("cannot create FFmpeg input pipe");
    }
    if (::pipe2(outputPipe, O_CLOEXEC) != 0) {
        ::close(inputPipe[0]); ::close(inputPipe[1]);
        throw std::runtime_error("cannot create FFmpeg output pipe");
    }
    const auto args = arguments(input);
    std::vector<char*> argv;
    argv.reserve(args.size() + 1);
    for (const auto& argument : args) {
        argv.push_back(const_cast<char*>(argument.c_str()));
    }
    argv.push_back(nullptr);
    pid_t pid = -1;
    const int error = spawn(argv.data(), inputPipe[0], outputPipe[1], pid);
    ::close(inputPipe[0]);
    ::close(outputPipe[1]);
    if (error != 0) {
        ::close(inputPipe[1]); ::close(outputPipe[0]);
        throw std::runtime_error(std::string("cannot start ffmpeg: ") + std::strerror(error));
    }
    inputFd_ = inputPipe[1];
    outputFd_ = outputPipe[0];
    childPid_ = static_cast<int>(pid);
    inputFormat_ = input;
    reader_ = std::thread(&FfmpegEncoder::readOutput, this, outputFd_);
}

void FfmpegEncoder::stopProcess() {
    {
        std::lock_guard lock(mutex_);
        processStopping_ = true;
        if (childPid_ > 0) {
            ::kill(childPid_, SIGKILL);
        }
    }
    if (inputFd_ >= 0) {
        ::close(inputFd_);
        inputFd_ = -1;
    }
    if (reader_.joinable()) {
        reader_.join();
    }
    outputFd_ = -1;
    {
        std::lock_guard lock(mutex_);
        if (childPid_ > 0) {
            int status = 0;
            while (::waitpid(childPid_, &status, 0) < 0 && errno == EINTR) {}
            childPid_ = -1;
        }
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
                    || frame.format.height != inputFormat_.height);
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
                throw std::runtime_error("FFmpeg encoder pipe failed");
            }
        }
    } catch (...) {
        fail(std::current_exception());
    }
    stopProcess();
}

void FfmpegEncoder::readOutput(const int fd) {
    constexpr int bufferSize = 64 * 1024;
    unsigned char* buffer = nullptr;
    AVIOContext* io = nullptr;
    AVFormatContext* format = nullptr;
    AVPacket* packet = nullptr;
    try {
        buffer = static_cast<unsigned char*>(av_malloc(bufferSize));
        if (buffer == nullptr) {
            throw std::runtime_error("cannot allocate FFmpeg input buffer");
        }
        io = avio_alloc_context(buffer, bufferSize, 0,
                                reinterpret_cast<void*>(static_cast<std::intptr_t>(fd)),
                                &readPipe, nullptr, nullptr);
        if (io == nullptr) {
            throw std::runtime_error("cannot allocate FFmpeg input context");
        }
        format = avformat_alloc_context();
        packet = av_packet_alloc();
        if (format == nullptr || packet == nullptr) {
            throw std::runtime_error("cannot allocate FFmpeg demuxer");
        }
        format->pb = io;
        // NUT headers name the codec. Parsing would hold the last frame back.
        format->flags |= AVFMT_FLAG_CUSTOM_IO | AVFMT_FLAG_NOPARSE | AVFMT_FLAG_NOFILLIN;
        int result = avformat_open_input(&format, nullptr, av_find_input_format("nut"), nullptr);
        if (result < 0) {
            throw std::runtime_error("FFmpeg produced no NUT stream (error "
                                     + std::to_string(result) + ')');
        }
        while ((result = av_read_frame(format, packet)) >= 0) {
            const std::string_view payload(reinterpret_cast<const char*>(packet->data),
                                          static_cast<std::size_t>(packet->size));
            emitPacket(withFourByteStartCodes(payload), (packet->flags & AV_PKT_FLAG_KEY) != 0);
            av_packet_unref(packet);
        }
        throw std::runtime_error("FFmpeg output ended (error " + std::to_string(result) + ')');
    } catch (...) {
        fail(std::current_exception());
    }
    avformat_close_input(&format);
    av_packet_free(&packet);
    if (io != nullptr) {
        av_freep(&io->buffer);
        avio_context_free(&io);
    } else {
        av_free(buffer);
    }
    ::close(fd);
}

void FfmpegEncoder::emitPacket(std::string annexB, const bool keyframe) {
    if (annexB.empty()) {
        return;
    }
    std::int64_t timestamp = wallClockMs();
    {
        std::lock_guard lock(mutex_);
        if (!timestamps_.empty()) {
            timestamp = timestamps_.front();
            timestamps_.pop_front();
        }
    }
    if (callback_) {
        callback_(EncodedFrame{.capturedAtMs = timestamp, .keyframe = keyframe,
                               .annexB = std::move(annexB)});
    }
}

}  // namespace od
