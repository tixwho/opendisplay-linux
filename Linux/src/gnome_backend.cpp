#include "opendisplay/gnome_backend.hpp"

#include "opendisplay/log.hpp"

#include <QDBusArgument>
#include <QDBusInterface>
#include <QDBusObjectPath>
#include <QEventLoop>
#include <QTimer>
#include <QVariantMap>

#include <linux/input-event-codes.h>

#include <algorithm>
#include <cmath>
#include <stdexcept>

namespace od {
namespace {

constexpr auto remoteDesktopService = "org.gnome.Mutter.RemoteDesktop";
constexpr auto screenCastService = "org.gnome.Mutter.ScreenCast";
constexpr auto remoteDesktopInterface = "org.gnome.Mutter.RemoteDesktop";
constexpr auto screenCastInterface = "org.gnome.Mutter.ScreenCast";
constexpr auto screenCastSessionInterface = "org.gnome.Mutter.ScreenCast.Session";
constexpr auto streamInterface = "org.gnome.Mutter.ScreenCast.Stream";
constexpr auto remoteSessionInterface = "org.gnome.Mutter.RemoteDesktop.Session";
constexpr auto sessionInterface = "org.gnome.Mutter.Session";
constexpr auto rootPath = "/org/gnome/Mutter/ScreenCast";
constexpr auto remoteRootPath = "/org/gnome/Mutter/RemoteDesktop";

QVariantMap mode(const int width, const int height, const int refreshRate,
                 const double scale) {
    QDBusArgument size;
    size.beginStructure();
    size << static_cast<uint>(width) << static_cast<uint>(height);
    size.endStructure();
    return {{QStringLiteral("size"), QVariant::fromValue(size)},
            {QStringLiteral("refresh-rate"), static_cast<double>(refreshRate)},
            {QStringLiteral("is-preferred"), true},
            {QStringLiteral("preferred-scale"), scale}};
}

QDBusArgument modeArray(const QVariantMap& preferredMode) {
    QDBusArgument modes;
    modes.beginArray(qMetaTypeId<QVariantMap>());
    modes << preferredMode;
    modes.endArray();
    return modes;
}

QString objectPath(const QDBusMessage& reply, const QString& method) {
    if (reply.type() == QDBusMessage::ErrorMessage) {
        throw std::runtime_error((method + QStringLiteral(" failed: ")
                                  + reply.errorMessage()).toStdString());
    }
    if (reply.arguments().isEmpty()) {
        throw std::runtime_error((method + QStringLiteral(" returned no object path")).toStdString());
    }
    const auto value = reply.arguments().front();
    const auto path = value.canConvert<QDBusObjectPath>()
        ? value.value<QDBusObjectPath>().path() : value.toString();
    if (path.isEmpty()) {
        throw std::runtime_error((method + QStringLiteral(" returned an empty object path")).toStdString());
    }
    return path;
}

}  // namespace

GnomeBackend::GnomeBackend(QObject* parent)
    : QObject(parent), bus_(QDBusConnection::sessionBus()) {
    if (!bus_.isConnected()) {
        throw std::runtime_error("cannot connect to the D-Bus session bus");
    }
}

GnomeBackend::~GnomeBackend() { stop(); }

QDBusMessage GnomeBackend::call(const QString& service, const QString& path,
                                const QString& interface, const QString& method,
                                const QVariantList& arguments) {
    auto message = QDBusMessage::createMethodCall(service, path, interface, method);
    message.setArguments(arguments);
    const auto reply = bus_.call(message, QDBus::Block, 30'000);
    if (reply.type() == QDBusMessage::ErrorMessage) {
        throw std::runtime_error((method + QStringLiteral(" failed: ")
                                  + reply.errorMessage()).toStdString());
    }
    return reply;
}

PortalCapture GnomeBackend::start(const DesktopRequest& request) {
    stop();
    if (request.mode == CaptureMode::Mirror) {
        portalMirror_ = std::make_unique<KdePortal>();
        try {
            return portalMirror_->start(request);
        } catch (...) {
            portalMirror_.reset();
            throw;
        }
    }
    const int width = std::max(2, request.display.virtualResolution
                                      ? request.display.virtualResolution->width
                                      : request.receiver.pixelsWide) & ~1;
    const int height = std::max(2, request.display.virtualResolution
                                       ? request.display.virtualResolution->height
                                       : request.receiver.pixelsHigh) & ~1;
    const int fps = request.display.refreshRate.value_or(request.refreshRate);
    const double scale = request.display.virtualScale.value_or(request.receiver.scale);
    if (fps <= 0 || !std::isfinite(scale) || scale <= 0.0) {
        throw std::runtime_error("GNOME virtual monitor mode has invalid refresh rate or scale");
    }

    try {
        log("Requesting a GNOME Mutter virtual monitor session…");
        remoteSessionPath_ = objectPath(call(
            remoteDesktopService, remoteRootPath, remoteDesktopInterface,
            QStringLiteral("CreateSession")), QStringLiteral("CreateSession"));

        QDBusInterface remoteSession(remoteDesktopService, remoteSessionPath_,
                                     remoteSessionInterface, bus_);
        const auto sessionId = remoteSession.property("SessionId").toString();
        if (sessionId.isEmpty()) {
            throw std::runtime_error("Mutter did not expose a remote desktop session ID");
        }

        QVariantMap sessionProperties;
        sessionProperties.insert(QStringLiteral("remote-desktop-session-id"), sessionId);
        screenCastSessionPath_ = objectPath(call(
            screenCastService, rootPath, screenCastInterface,
            QStringLiteral("CreateSession"), {sessionProperties}),
            QStringLiteral("ScreenCast.CreateSession"));

        QVariantMap properties;
        properties.insert(QStringLiteral("cursor-mode"), 2U);  // metadata
        properties.insert(QStringLiteral("modes"), QVariant::fromValue(modeArray(
            mode(width, height, fps, scale))));
        streamPath_ = objectPath(call(
            screenCastService, screenCastSessionPath_, screenCastSessionInterface,
            QStringLiteral("RecordVirtual"), {properties}),
            QStringLiteral("ScreenCast.RecordVirtual"));

        if (!bus_.connect(screenCastService, streamPath_, streamInterface,
                          QStringLiteral("PipeWireStreamAdded"), this,
                          SLOT(pipeWireStreamAdded(uint)))) {
            throw std::runtime_error("cannot subscribe to Mutter's PipeWire stream signal");
        }

        call(remoteDesktopService, remoteSessionPath_, remoteSessionInterface,
             QStringLiteral("Start"));

        if (!nodeId_) {
            QEventLoop loop;
            QTimer timeout;
            timeout.setSingleShot(true);
            connect(&timeout, &QTimer::timeout, &loop, &QEventLoop::quit);
            connect(this, &GnomeBackend::destroyed, &loop, &QEventLoop::quit);
            timeout.start(10'000);
            while (!nodeId_ && timeout.isActive()) {
                QTimer::singleShot(20, &loop, &QEventLoop::quit);
                loop.exec();
            }
            if (!nodeId_) {
                throw std::runtime_error("Mutter did not publish the virtual monitor PipeWire node");
            }
        }
        logicalWidth_ = std::max(1, static_cast<int>(std::lround(width / scale)));
        logicalHeight_ = std::max(1, static_cast<int>(std::lround(height / scale)));
        inputEnabled_ = request.requestInput;
        log("GNOME virtual monitor ready: " + std::to_string(width) + 'x'
            + std::to_string(height) + '@' + std::to_string(fps) + ", scale "
            + std::to_string(scale) + "; PipeWire node " + std::to_string(*nodeId_));
        return {.sessionPath = screenCastSessionPath_.toStdString(),
                .stream = {.nodeId = *nodeId_, .logicalWidth = logicalWidth_,
                           .logicalHeight = logicalHeight_},
                .pipewireFd = -1,
                .captureWidth = width,
                .captureHeight = height,
                .cursorMetadata = true};
    } catch (...) {
        stop();
        throw;
    }
}

void GnomeBackend::pipeWireStreamAdded(const uint nodeId) { nodeId_ = nodeId; }

void GnomeBackend::stop() {
    if (portalMirror_) {
        portalMirror_->stop();
        portalMirror_.reset();
    }
    inputEnabled_ = false;
    if (!streamPath_.isEmpty()) {
        bus_.disconnect(screenCastService, streamPath_, streamInterface,
                        QStringLiteral("PipeWireStreamAdded"), this,
                        SLOT(pipeWireStreamAdded(uint)));
    }
    if (!remoteSessionPath_.isEmpty()) {
        try {
            call(remoteDesktopService, remoteSessionPath_, remoteSessionInterface,
                 QStringLiteral("Stop"));
        } catch (const std::exception& error) {
            debug(std::string("Cannot stop GNOME remote desktop session: ") + error.what());
        }
        try {
            call(remoteDesktopService, remoteSessionPath_, sessionInterface,
                 QStringLiteral("Close"));
        } catch (const std::exception& error) {
            debug(std::string("Cannot close GNOME remote desktop session: ") + error.what());
        }
    } else if (!screenCastSessionPath_.isEmpty()) {
        try {
            call(screenCastService, screenCastSessionPath_, sessionInterface,
                 QStringLiteral("Close"));
        } catch (const std::exception& error) {
            debug(std::string("Cannot close GNOME screen cast session: ") + error.what());
        }
    }
    remoteSessionPath_.clear();
    screenCastSessionPath_.clear();
    streamPath_.clear();
    nodeId_.reset();
    logicalWidth_ = 0;
    logicalHeight_ = 0;
    pointerDown_ = false;
}

void GnomeBackend::notify(const QString& method, const QVariantList& arguments) {
    if (!inputEnabled_ || remoteSessionPath_.isEmpty()) return;
    auto message = QDBusMessage::createMethodCall(remoteDesktopService,
                                                   remoteSessionPath_,
                                                   remoteSessionInterface, method);
    message.setArguments(arguments);
    bus_.call(message, QDBus::NoBlock);
}

void GnomeBackend::pointer(const std::string_view phase, const double normalizedX,
                           const double normalizedY) {
    if (portalMirror_) {
        portalMirror_->pointer(phase, normalizedX, normalizedY);
        return;
    }
    const double x = std::clamp(normalizedX, 0.0, 1.0) * logicalWidth_;
    const double y = std::clamp(normalizedY, 0.0, 1.0) * logicalHeight_;
    notify(QStringLiteral("NotifyPointerMotionAbsolute"),
           {streamPath_, x, y});
    if (phase == "began") {
        pointerDown_ = true;
        notify(QStringLiteral("NotifyPointerButton"), {BTN_LEFT, true});
    } else if ((phase == "ended" || phase == "cancelled") && pointerDown_) {
        pointerDown_ = false;
        notify(QStringLiteral("NotifyPointerButton"), {BTN_LEFT, false});
    }
}

void GnomeBackend::scroll(const double dx, const double dy) {
    if (portalMirror_) {
        portalMirror_->scroll(dx, dy);
        return;
    }
    notify(QStringLiteral("NotifyPointerAxis"), {dx, dy, 0U});
}

}  // namespace od
