#include "opendisplay/session.hpp"

#include "opendisplay/log.hpp"
#include "opendisplay/wire.hpp"

#include <QJsonDocument>
#include <QJsonObject>

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <stdexcept>
#include <vector>

#include <unistd.h>

namespace od {
namespace {

/// A receiver that has not drained a frame for this long is gone or unreachable.
constexpr auto sendTimeout = std::chrono::seconds(2);

}  // namespace

Session::Session(Options options, std::unique_ptr<DesktopBackend> desktop)
    : options_(std::move(options)), desktop_(std::move(desktop)) {
    if (!desktop_) {
        throw std::invalid_argument("a desktop backend is required");
    }
}

Session::~Session() { stop(); }

void Session::start(const Endpoint& endpoint) {
    stop();
    if (endpoint.kind == TransportKind::Usb) {
        log("Connecting through usbmuxd to " + endpoint.udid + "…");
        socket_ = connectUsb(endpoint.usbHandle, endpoint.port);
    } else {
        log("Connecting over Wi-Fi to " + endpoint.host + ':' + std::to_string(endpoint.port)
            + "…");
        socket_ = connectTcp(endpoint.host, endpoint.port);
    }
    connected_.store(true);
    receiver_ = std::thread(&Session::receiveLoop, this);

    PhoneInfo initial;
    {
        std::unique_lock lock(stateMutex_);
        const bool announced = helloCondition_.wait_for(lock, std::chrono::seconds(15), [&] {
            return phone_.has_value() || !connected_.load();
        });
        if (!announced || !phone_) {
            lock.unlock();
            stop();
            throw std::runtime_error("receiver did not send a hello message");
        }
        initial = *phone_;
        events_.clear();
    }
    if (initial.protocolVersion < wire::minSupportedPeer) {
        stop();
        throw std::runtime_error("receiver protocol is too old");
    }
    log("Receiver: " + initial.device + " " + std::to_string(initial.pixelsWide) + 'x'
        + std::to_string(initial.pixelsHigh) + " (protocol "
        + std::to_string(initial.protocolVersion) + ')');
    startPipeline(initial);
    lastPing_ = std::chrono::steady_clock::now();
}

void Session::startPipeline(const PhoneInfo& phone) {
    const int nativeWidth = std::max(2, phone.pixelsWide) & ~1;
    const int nativeHeight = std::max(2, phone.pixelsHigh) & ~1;
    const int outputWidth = std::max(2, static_cast<int>(std::lround(nativeWidth * options_.scale)))
        & ~1;
    const int outputHeight = std::max(2, static_cast<int>(std::lround(nativeHeight * options_.scale)))
        & ~1;
    const auto capture = desktop_->start(DesktopRequest{
        .mode = options_.mode,
        .receiver = phone,
        .display = options_.display,
        .refreshRate = options_.fps,
        .requestInput = options_.input,
    });
    bool pipewireFdHandedOff = false;
    try {
        encoder_.start(EncoderConfig{
            .kind = options_.encoder,
            .vaapiDevice = options_.vaapiDevice,
            .outputWidth = outputWidth,
            .outputHeight = outputHeight,
            .fps = options_.fps,
            .bitrate = options_.bitrate,
        }, [this](EncodedFrame frame) {
            if (frame.annexB.empty() || !wire::containsAnnexBStartCode(frame.annexB)) {
                return;
            }
            send(wire::videoPayload(frame, wallClockMs()));
        });
        pipewireFdHandedOff = true;
        capture_.start(capture.pipewireFd, capture.stream.nodeId, capture.captureWidth,
                       capture.captureHeight, options_.fps, [this](CapturedFrame frame) {
                           encoder_.submit(std::move(frame));
                       });
        pipelineRunning_ = true;
        activePhone_ = phone;
        log("Streaming " + std::to_string(outputWidth) + 'x' + std::to_string(outputHeight)
            + " at up to " + std::to_string(options_.fps) + " fps");
    } catch (...) {
        if (!pipewireFdHandedOff) {
            ::close(capture.pipewireFd);
        }
        encoder_.stop();
        desktop_->stop();
        throw;
    }
}

void Session::stopPipeline() {
    capture_.stop();
    encoder_.stop();
    desktop_->stop();
    pipelineRunning_ = false;
}

bool Session::send(const std::string_view payload) {
    const auto framed = wire::frame(payload);
    std::lock_guard lock(sendMutex_);
    if (!connected_.load() || !socket_.valid()) {
        return false;
    }
    if (!socket_.writeAll(framed, sendTimeout)) {
        if (connected_.exchange(false)) {
            log("Receiver stopped reading; disconnecting");
        }
        return false;
    }
    return true;
}

void Session::queue(Event event) {
    std::lock_guard lock(stateMutex_);
    events_.push_back(std::move(event));
}

void Session::receiveLoop() {
    std::array<char, 4> header{};
    while (connected_.load() && socket_.readExact(header)) {
        const auto length = wire::decodeLength(header);
        if (!length) {
            log("Receiver sent an invalid control frame");
            break;
        }
        std::string bytes(*length, '\0');
        if (!socket_.readExact(std::span<char>(bytes.data(), bytes.size()))) {
            break;
        }
        const auto object = wire::parseJson(bytes);
        if (!object) {
            debug("Ignoring invalid JSON from receiver");
            continue;
        }
        const auto type = object->value(QStringLiteral("type")).toString();
        if (type == QStringLiteral("hello")) {
            const auto announced = wire::parseHello(*object);
            if (!announced) {
                continue;
            }
            {
                std::lock_guard lock(stateMutex_);
                phone_ = *announced;
                events_.push_back(Event{.kind = EventKind::Hello, .phone = *announced,
                                        .phase = {}, .x = 0, .y = 0});
            }
            helloCondition_.notify_all();
            send(wire::welcome());
        } else if (type == QStringLiteral("ping")) {
            send(wire::pong(object->value(QStringLiteral("t")).toDouble(),
                            static_cast<double>(wallClockMs())));
        } else if (type == QStringLiteral("touch")) {
            queue(Event{
                .kind = EventKind::Touch,
                .phone = {},
                .phase = object->value(QStringLiteral("phase")).toString().toStdString(),
                .x = object->value(QStringLiteral("x")).toDouble(),
                .y = object->value(QStringLiteral("y")).toDouble(),
            });
        } else if (type == QStringLiteral("scroll")) {
            queue(Event{.kind = EventKind::Scroll, .phone = {}, .phase = {},
                        .x = object->value(QStringLiteral("dx")).toDouble(),
                        .y = object->value(QStringLiteral("dy")).toDouble()});
        } else if (type == QStringLiteral("kf")) {
            log("Receiver requested a keyframe");
            encoder_.requestKeyframe();
        } else if (type == QStringLiteral("stats")) {
            debug("Receiver stats: "
                  + QJsonDocument(*object).toJson(QJsonDocument::Compact).toStdString());
        } else if (type == QStringLiteral("sleeping") || type == QStringLiteral("closing")) {
            log(type == QStringLiteral("sleeping") ? "Receiver went to sleep"
                                                    : "Receiver app closed");
            break;
        } else {
            debug("Ignoring control message " + type.toStdString());
        }
    }
    connected_.store(false);
    helloCondition_.notify_all();
}

bool Session::tick() {
    if (const auto encoderError = encoder_.failure(); !encoderError.empty()) {
        stop();
        throw std::runtime_error("FFmpeg encoder failed: " + encoderError);
    }
    if (const auto captureError = capture_.error()) {
        throw std::runtime_error("PipeWire capture failed: " + *captureError);
    }
    std::deque<Event> events;
    {
        std::lock_guard lock(stateMutex_);
        events.swap(events_);
    }
    for (const auto& event : events) {
        if (event.kind == EventKind::Touch) {
            desktop_->pointer(event.phase, event.x, event.y);
        } else if (event.kind == EventKind::Scroll) {
            desktop_->scroll(event.x, event.y);
        } else if (event.kind == EventKind::Hello) {
            std::lock_guard lock(stateMutex_);
            if (activePhone_ && pipelineRunning_
                && (event.phone.pixelsWide != activePhone_->pixelsWide
                    || event.phone.pixelsHigh != activePhone_->pixelsHigh)) {
                reconfigurePending_ = true;
                reconfigureAfter_ = std::chrono::steady_clock::now()
                    + std::chrono::milliseconds(300);
            }
            phone_ = event.phone;
        }
    }

    const auto now = std::chrono::steady_clock::now();
    if (reconfigurePending_ && now >= reconfigureAfter_) {
        PhoneInfo current;
        {
            std::lock_guard lock(stateMutex_);
            current = *phone_;
            reconfigurePending_ = false;
        }
        log("Reconfiguring for receiver rotation");
        stopPipeline();
        startPipeline(current);
    }
    if (connected_.load() && now - lastPing_ >= std::chrono::seconds(2)) {
        const QJsonObject ping{{QStringLiteral("type"), QStringLiteral("ping")},
                               {QStringLiteral("encDrops"), 0},
                               {QStringLiteral("netDrops"), 0},
                               {QStringLiteral("pending"), 0}};
        send(QJsonDocument(ping).toJson(QJsonDocument::Compact).toStdString());
        lastPing_ = now;
    }
    return connected_.load();
}

void Session::stop() {
    // Wake blocked reads and writes first, so the encoder and receiver threads can be joined
    // even when the peer stopped draining. The descriptor stays open until they have.
    connected_.store(false);
    socket_.shutdown();
    helloCondition_.notify_all();
    stopPipeline();
    if (receiver_.joinable()) {
        receiver_.join();
    }
    socket_.close();
    {
        std::lock_guard lock(stateMutex_);
        phone_.reset();
        activePhone_.reset();
        events_.clear();
    }
    reconfigurePending_ = false;
}

}  // namespace od
