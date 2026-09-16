#include "opendisplay/ffmpeg_encoder.hpp"
#include "opendisplay/wire.hpp"

#include <fcntl.h>
#include <unistd.h>
#include <sys/stat.h>
#include <cerrno>
#include <cstdlib>
#include <filesystem>
#include <stdexcept>
#include <thread>

#include <array>
#include <cassert>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <fstream>
#include <mutex>
#include <string>
#include <string_view>
#include <vector>

namespace {

bool hasOnlyFourByteStartCodes(const std::string_view bytes) {
    for (std::size_t index = 0; index + 2 < bytes.size(); ++index) {
        if (bytes[index] == 0 && bytes[index + 1] == 0 && bytes[index + 2] == 1
            && (index == 0 || bytes[index - 1] != 0)) {
            return false;
        }
    }
    return true;
}

int nalCount(const std::string_view bytes, const unsigned char type) {
    int count = 0;
    for (std::size_t index = 0; index + 4 < bytes.size(); ++index) {
        if (bytes[index] == 0 && bytes[index + 1] == 0 && bytes[index + 2] == 0
            && bytes[index + 3] == 1
            && (static_cast<unsigned char>(bytes[index + 4]) & 0x1fU) == type) {
            ++count;
        }
    }
    return count;
}

// A keyframe carries exactly one SPS and PPS (types 7 and 8); the receiver
// compares every copy against its decoder format. One AUD (9), at least one
// IDR slice (5).
void assertSingleHeaders(const od::EncodedFrame& keyframe) {
    assert(keyframe.keyframe);
    assert(nalCount(keyframe.annexB, 7) == 1);
    assert(nalCount(keyframe.annexB, 8) == 1);
    assert(nalCount(keyframe.annexB, 9) == 1);
    assert(nalCount(keyframe.annexB, 5) >= 1);
}

// NV12 test frame: 64x64 luma followed by the interleaved 32x32 chroma plane.
constexpr std::size_t testFrameBytes = 64 * 64 * 3 / 2;

od::CapturedFrame testFrame(const int index) {
    od::CapturedFrame frame;
    frame.format = od::VideoFormat{.width = 64, .height = 64, .stride = 64, .fps = 30};
    frame.capturedAtMs = 1000 + index;
    frame.sequence = static_cast<unsigned>(index);
    frame.bytes.assign(testFrameBytes, static_cast<char>(index * 20));
    return frame;
}

// Decodes the delivered stream with ffprobe and returns its pixel format.
std::string probePixelFormat(const std::vector<od::EncodedFrame>& frames) {
    char path[] = "/tmp/opendisplay-encoder-test-XXXXXX.h264";
    const int fd = ::mkstemps(path, 5);
    assert(fd >= 0);
    ::close(fd);
    {
        std::ofstream file(path, std::ios::binary);
        for (const auto& frame : frames) {
            file.write(frame.annexB.data(), static_cast<std::streamsize>(frame.annexB.size()));
        }
    }
    const std::string command = "ffprobe -v error -select_streams v:0 -show_entries "
                                "stream=pix_fmt -of csv=p=0 " + std::string(path);
    FILE* process = ::popen(command.c_str(), "r");
    assert(process != nullptr);
    std::array<char, 256> buffer{};
    std::string output;
    while (::fgets(buffer.data(), static_cast<int>(buffer.size()), process) != nullptr) {
        output.append(buffer.data());
    }
    ::pclose(process);
    ::unlink(path);
    while (!output.empty() && (output.back() == '\n' || output.back() == '\r')) {
        output.pop_back();
    }
    return output;
}

void waitForFailure(od::FfmpegEncoder& encoder) {
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(3);
    while (encoder.failure().empty() && std::chrono::steady_clock::now() < deadline) {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    assert(!encoder.failure().empty());
    const auto started = std::chrono::steady_clock::now();
    encoder.stop();
    assert(std::chrono::steady_clock::now() - started < std::chrono::seconds(1));
}

void readerContainsCallbackExceptions() {
    for (const bool standardException : {true, false}) {
        od::FfmpegEncoder encoder;
        encoder.start(od::EncoderConfig{.kind = od::EncoderKind::Software},
                      [standardException](od::EncodedFrame) {
            if (standardException) throw std::runtime_error("callback failed");
            throw 42;
        });
        encoder.submit(testFrame(0));
        waitForFailure(encoder);
        assert(encoder.failure() == (standardException ? "callback failed"
                                                       : "Unknown encoder error"));
    }
}

void invalidOutputReportsFailureAndVaapiUsesOneAsyncFrame() {
    char directory[] = "/tmp/opendisplay-ffmpeg-test-XXXXXX";
    assert(::mkdtemp(directory) != nullptr);
    const std::string executable = std::string(directory) + "/ffmpeg";
    const std::string argumentFile = std::string(directory) + "/arguments";
    const std::string countFile = std::string(directory) + "/count";
    {
        std::ofstream script(executable);
        script << "#!/bin/sh\n"
                  "case \" $* \" in *' -encoders '*) echo h264_vaapi; exit 0;; esac\n"
                  "printf '%s\\n' \"$@\" > " << argumentFile << "\n"
                  "head -c " << testFrameBytes << " | wc -c > " << countFile << "\n"
                  "printf invalid-nut\n";
    }
    assert(::chmod(executable.c_str(), 0700) == 0);
    const std::string oldPath = std::getenv("PATH");
    assert(::setenv("PATH", (std::string(directory) + ':' + oldPath).c_str(), 1) == 0);
    // With stdin closed, the input pipe lands on descriptor 0, which the child
    // must keep as its stdin instead of closing it.
    const int savedStdin = ::dup(STDIN_FILENO);
    assert(savedStdin >= 0);
    ::close(STDIN_FILENO);
    od::FfmpegEncoder encoder;
    encoder.start(od::EncoderConfig{.kind = od::EncoderKind::Vaapi}, [](od::EncodedFrame) {
        assert(false && "invalid NUT must not emit a frame");
    });
    encoder.submit(testFrame(0));
    waitForFailure(encoder);
    assert(encoder.failure().find("NUT stream") != std::string::npos);
    assert(::dup2(savedStdin, STDIN_FILENO) == STDIN_FILENO);
    ::close(savedStdin);
    {
        std::ifstream file(argumentFile);
        const std::string arguments((std::istreambuf_iterator<char>(file)), {});
        assert(arguments.find("-async_depth\n1\n") != std::string::npos);
        assert(arguments.find("-pixel_format\nnv12\n") != std::string::npos);
        assert(arguments.find("-vf\nhwupload\n") != std::string::npos);
        assert(arguments.find("scale=") == std::string::npos);
        assert(arguments.find("dump_extra") == std::string::npos);
        std::ifstream count(countFile);
        const std::string received((std::istreambuf_iterator<char>(count)), {});
        assert(received == std::to_string(testFrameBytes) + "\n");
    }
    // A child that never drains stdin must not hold stop() in write or join.
    const std::string readyFile = std::string(directory) + "/ready";
    {
        std::ofstream script(executable);
        script << "#!/bin/sh\n"
                  "case \" $* \" in *' -encoders '*) echo h264_vaapi; exit 0;; esac\n"
                  "touch " << readyFile << "\nexec sleep 30\n";
    }
    encoder.start(od::EncoderConfig{.kind = od::EncoderKind::Vaapi}, [](od::EncodedFrame) {});
    assert(encoder.failure().empty());
    auto largeFrame = testFrame(0);
    largeFrame.format.width = 2752;
    largeFrame.format.height = 2064;
    largeFrame.format.stride = 2752;
    largeFrame.bytes.resize(2752 * 2064 * 3 / 2);
    encoder.submit(std::move(largeFrame));
    const auto readyDeadline = std::chrono::steady_clock::now() + std::chrono::seconds(3);
    while (!std::filesystem::exists(readyFile)
           && std::chrono::steady_clock::now() < readyDeadline) {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    assert(std::filesystem::exists(readyFile));
    const auto stopped = std::chrono::steady_clock::now();
    encoder.stop();
    assert(std::chrono::steady_clock::now() - stopped < std::chrono::seconds(1));
    assert(encoder.failure().empty());
    assert(::setenv("PATH", oldPath.c_str(), 1) == 0);
    std::filesystem::remove_all(directory);
}

// Waits for `ready` to observe the state it reads; the deadline only catches
// a hang and never orders the threads.
template <class Predicate>
bool waitUntil(Predicate ready) {
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
    while (!ready()) {
        if (std::chrono::steady_clock::now() >= deadline) {
            return false;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    return true;
}

// A capture thread that outruns FFmpeg replaces the single pending frame.
// Only the replaced frame is lost: its successor keeps its own timestamp, the
// drop total counts it, and the pending depth reports the slot plus the frame
// FFmpeg has not returned yet.
void replacementDropsOnlyThePendingFrame() {
    std::string realFfmpeg;
    {
        FILE* process = ::popen("command -v ffmpeg", "r");
        assert(process != nullptr);
        std::array<char, 512> buffer{};
        if (::fgets(buffer.data(), static_cast<int>(buffer.size()), process) != nullptr) {
            realFfmpeg = buffer.data();
        }
        ::pclose(process);
        while (!realFfmpeg.empty() && realFfmpeg.back() == '\n') realFfmpeg.pop_back();
        assert(!realFfmpeg.empty());
    }
    char directory[] = "/tmp/opendisplay-ffmpeg-test-XXXXXX";
    assert(::mkdtemp(directory) != nullptr);
    const std::string executable = std::string(directory) + "/ffmpeg";
    const std::string gate = std::string(directory) + "/gate";
    assert(::mkfifo(gate.c_str(), 0600) == 0);
    {
        // Opens the gate as soon as it runs, leaves the first frame stuck in
        // the input pipe until the test writes a line through the gate, then
        // encodes whatever arrived with the real ffmpeg.
        std::ofstream script(executable);
        script << "#!/bin/sh\n"
                  "case \" $* \" in *' -encoders '*) exec " << realFfmpeg << " \"$@\";; esac\n"
                  "read _ < " << gate << "\nexec " << realFfmpeg << " \"$@\"\n";
    }
    assert(::chmod(executable.c_str(), 0700) == 0);
    const std::string oldPath = std::getenv("PATH");
    assert(::setenv("PATH", (std::string(directory) + ':' + oldPath).c_str(), 1) == 0);

    std::mutex mutex;
    std::condition_variable condition;
    std::vector<od::EncodedFrame> output;
    od::FfmpegEncoder encoder;
    encoder.start(od::EncoderConfig{.kind = od::EncoderKind::Software, .fps = 30,
                                    .bitrate = 300'000},
                  [&](od::EncodedFrame frame) {
        {
            std::lock_guard lock(mutex);
            output.push_back(std::move(frame));
        }
        condition.notify_all();
    });
    // Larger than the 64 KiB pipe, so writing the first frame blocks the worker.
    const auto bigFrame = [](const int index) {
        od::CapturedFrame frame;
        frame.format = od::VideoFormat{.width = 256, .height = 256, .stride = 256, .fps = 30};
        frame.capturedAtMs = 1000 + index;
        frame.sequence = static_cast<unsigned>(index);
        frame.bytes.assign(256 * 256 * 3 / 2, static_cast<char>(index * 60));
        return frame;
    };
    encoder.submit(bigFrame(0));
    // The child opens the gate for reading once the worker has spawned it,
    // which comes after the worker took frame 0 out of the pending slot.
    // Opening the write side without a reader fails with ENXIO.
    int gateFd = -1;
    assert(waitUntil([&] {
        gateFd = ::open(gate.c_str(), O_WRONLY | O_NONBLOCK);
        return gateFd >= 0 || errno != ENXIO;
    }));
    assert(gateFd >= 0);
    // The worker registers frame 0 as in flight right before the pipe write
    // that blocks on it: the frame exceeds the pipe and nothing reads it
    // until the gate opens, so the worker cannot reach the pending slot.
    assert(waitUntil([&] { return encoder.pendingFrames() == 1; }));
    assert(encoder.droppedFrames() == 0);
    encoder.submit(bigFrame(1));
    encoder.submit(bigFrame(2));
    assert(encoder.droppedFrames() == 1);
    assert(encoder.pendingFrames() == 2);
    // Release the child: ffmpeg drains frame 0, then the worker hands it frame 2.
    assert(::write(gateFd, "\n", 1) == 1);
    ::close(gateFd);
    {
        std::unique_lock lock(mutex);
        assert(condition.wait_for(lock, std::chrono::seconds(5), [&] {
            return output.size() == 2;
        }));
    }
    assert(output[0].capturedAtMs == 1000);
    assert(output[1].capturedAtMs == 1002);
    assert(encoder.pendingFrames() == 0);
    encoder.stop();
    assert(encoder.failure().empty());
    assert(encoder.droppedFrames() == 1);
    assert(::setenv("PATH", oldPath.c_str(), 1) == 0);
    std::filesystem::remove_all(directory);
}

}  // namespace

int main(int argc, char** argv) {
    const bool vaapi = argc == 2 && std::string_view(argv[1]) == "--vaapi";
    std::mutex mutex;
    std::condition_variable condition;
    std::vector<od::EncodedFrame> output;
    od::FfmpegEncoder encoder;
    encoder.start(od::EncoderConfig{
        .kind = vaapi ? od::EncoderKind::Vaapi : od::EncoderKind::Software,
        .fps = 30,
        .bitrate = 300'000,
    }, [&](od::EncodedFrame frame) {
        {
            std::lock_guard lock(mutex);
            output.push_back(std::move(frame));
        }
        condition.notify_all();
    });

    // Every frame must be delivered on its own, before the next one is
    // submitted and without stopping the encoder to flush it.
    constexpr int frameCount = 8;
    for (int index = 0; index < frameCount; ++index) {
        encoder.submit(testFrame(index));
        std::unique_lock lock(mutex);
        const bool delivered = condition.wait_for(lock, std::chrono::seconds(5), [&] {
            return output.size() > static_cast<std::size_t>(index);
        });
        assert(delivered);
        assert(output.size() == static_cast<std::size_t>(index) + 1);
        assert(output.back().capturedAtMs == 1000 + index);
    }
    encoder.requestKeyframe();
    encoder.submit(testFrame(frameCount));
    {
        std::unique_lock lock(mutex);
        assert(condition.wait_for(lock, std::chrono::seconds(5), [&] {
            return output.size() == frameCount + 1;
        }));
        assertSingleHeaders(output.back());
        assert(output.back().capturedAtMs == 1000 + frameCount);
        output.pop_back();
    }
    encoder.stop();
    assert(output.size() == frameCount);
    assert(encoder.failure().empty());

    assert(od::wire::containsAnnexBStartCode(output.front().annexB));
    assertSingleHeaders(output.front());
    for (std::size_t index = 1; index < output.size(); ++index) {
        assert(!output[index].keyframe);
        assert(nalCount(output[index].annexB, 7) == 0);
        assert(nalCount(output[index].annexB, 8) == 0);
        assert(nalCount(output[index].annexB, 9) == 1);
        assert(nalCount(output[index].annexB, 1) >= 1);  // non-IDR slice
    }
    for (const auto& encoded : output) {
        assert(hasOnlyFourByteStartCodes(encoded.annexB));
    }
    // iPad hardware decoders need 8-bit 4:2:0; NV12 input must stay 4:2:0 on the wire.
    assert(probePixelFormat(output) == "yuv420p");
    if (!vaapi) {
        readerContainsCallbackExceptions();
        invalidOutputReportsFailureAndVaapiUsesOneAsyncFrame();
        replacementDropsOnlyThePendingFrame();
    }
    return 0;
}
