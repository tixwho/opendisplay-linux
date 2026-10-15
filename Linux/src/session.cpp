#include "opendisplay/session.hpp"

#include "opendisplay/log.hpp"
#include "opendisplay/wire.hpp"

#include <QJsonDocument>
#include <QJsonObject>
#include <QBuffer>
#include <QImage>

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <stdexcept>
#include <vector>

#include <unistd.h>

namespace od {

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
    const auto capture = desktop_->start(DesktopRequest{
        .mode = options_.mode,
        .receiver = phone,
        .display = options_.display,
        .refreshRate = options_.fps,
        .requestInput = options_.input,
    });
    // Do not upscale a deliberately smaller virtual mode back to the iPad's
    // native size. That adds CPU work without restoring detail, and makes the
    // GUI's Virtual resolution control ineffective as a performance setting.
    const int outputWidth = std::min(
        std::max(2, static_cast<int>(std::lround(nativeWidth * options_.scale)) & ~1),
        std::max(2, capture.captureWidth & ~1));
    const int outputHeight = std::min(
        std::max(2, static_cast<int>(std::lround(nativeHeight * options_.scale)) & ~1),
        std::max(2, capture.captureHeight & ~1));
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
        captureWidth_ = capture.captureWidth;
        captureHeight_ = capture.captureHeight;
        capture_.start(capture.pipewireFd, capture.stream.nodeId, capture.captureWidth,
                       capture.captureHeight, options_.fps, [this](CapturedFrame frame) {
                           encoder_.submit(std::move(frame));
                       }, capture.cursorMetadata,
                       [this](CursorUpdate update) { queueCursor(std::move(update)); });
        pipelineRunning_ = true;
        activePhone_ = phone;
        log("Streaming " + std::to_string(outputWidth) + 'x' + std::to_string(outputHeight)
            + " at up to " + std::to_string(options_.fps) + " fps");
    } catch (...) {
        if (!pipewireFdHandedOff && capture.pipewireFd >= 0) {
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
    captureWidth_ = 0;
    captureHeight_ = 0;
    pipelineRunning_ = false;
}

bool Session::send(const std::string_view payload) {
    const auto framed = wire::frame(payload);
    std::lock_guard lock(sendMutex_);
    if (!connected_.load() || !socket_.valid()) {
        return false;
    }
    if (!socket_.writeAll(framed)) {
        connected_.store(false);
        return false;
    }
    return true;
}

void Session::queue(Event event) {
    std::lock_guard lock(stateMutex_);
    events_.push_back(std::move(event));
}

void Session::queueCursor(CursorUpdate update) {
    std::lock_guard lock(stateMutex_);
    if (update.bitmap) {
        cursorBitmap_ = std::move(update.bitmap);
        cursorBitmapDirty_ = true;
    }
    update.bitmap.reset();
    pendingCursor_ = std::move(update);
}

void Session::sendCursorUpdate(const CursorUpdate& update) {
    if (update.bitmap) {
        const auto& bitmap = *update.bitmap;
        QImage image(bitmap.width, bitmap.height, QImage::Format_RGBA8888);
        if (!image.isNull() && bitmap.stride >= bitmap.width * 4
            && bitmap.bytes.size() >= static_cast<std::size_t>(bitmap.stride) * bitmap.height) {
            for (int y = 0; y < bitmap.height; ++y) {
                const auto* source = bitmap.bytes.data()
                    + static_cast<std::size_t>(y) * bitmap.stride;
                auto* target = image.scanLine(y);
                for (int x = 0; x < bitmap.width; ++x) {
                    const auto* pixel = source + x * 4;
                    auto* rgba = target + x * 4;
                    switch (bitmap.pixelFormat) {
                    case VideoFormat::PixelFormat::Bgra:
                        rgba[0] = pixel[2]; rgba[1] = pixel[1];
                        rgba[2] = pixel[0]; rgba[3] = pixel[3];
                        break;
                    case VideoFormat::PixelFormat::Bgrx:
                        rgba[0] = pixel[2]; rgba[1] = pixel[1];
                        rgba[2] = pixel[0]; rgba[3] = 255;
                        break;
                    case VideoFormat::PixelFormat::Rgba:
                        std::copy_n(pixel, 4, rgba);
                        break;
                    case VideoFormat::PixelFormat::Rgbx:
                        std::copy_n(pixel, 3, rgba); rgba[3] = 255;
                        break;
                    }
                }
            }
            QByteArray png;
            QBuffer buffer(&png);
            if (buffer.open(QIODevice::WriteOnly) && image.save(&buffer, "PNG")) {
                const auto base64 = png.toBase64();
                send(wire::cursorImage(
                    static_cast<double>(bitmap.width) / captureWidth_,
                    static_cast<double>(bitmap.height) / captureHeight_,
                    bitmap.width > 0 ? static_cast<double>(bitmap.hotspotX) / bitmap.width : 0.0,
                    bitmap.height > 0 ? static_cast<double>(bitmap.hotspotY) / bitmap.height : 0.0,
                    base64.toStdString()));
            }
        }
    }

    const std::pair<int, int> position{update.x, update.y};
    if (cursorVisible_ != update.visible || (update.visible && lastCursorPosition_ != position)) {
        const double x = captureWidth_ > 0
            ? std::clamp(static_cast<double>(update.x) / captureWidth_, 0.0, 1.0) : 0.0;
        const double y = captureHeight_ > 0
            ? std::clamp(static_cast<double>(update.y) / captureHeight_, 0.0, 1.0) : 0.0;
        send(wire::cursor(x, y, update.visible));
        cursorVisible_ = update.visible;
        if (update.visible) lastCursorPosition_ = position;
        else lastCursorPosition_.reset();
    }
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
    if (const auto captureError = capture_.error()) {
        throw std::runtime_error("PipeWire capture failed: " + *captureError);
    }
    std::deque<Event> events;
    std::optional<CursorUpdate> cursor;
    {
        std::lock_guard lock(stateMutex_);
        events.swap(events_);
        cursor.swap(pendingCursor_);
        if (cursor && cursorBitmapDirty_ && cursorBitmap_) {
            cursor->bitmap = cursorBitmap_;
            cursorBitmapDirty_ = false;
        }
    }
    if (cursor) sendCursorUpdate(*cursor);
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
    stopPipeline();
    connected_.store(false);
    {
        std::lock_guard lock(sendMutex_);
        socket_.close();
    }
    helloCondition_.notify_all();
    if (receiver_.joinable()) {
        receiver_.join();
    }
    {
        std::lock_guard lock(stateMutex_);
        phone_.reset();
        activePhone_.reset();
        events_.clear();
        pendingCursor_.reset();
        cursorBitmap_.reset();
        cursorBitmapDirty_ = false;
    }
    cursorVisible_ = false;
    lastCursorPosition_.reset();
    reconfigurePending_ = false;
}

}  // namespace od
