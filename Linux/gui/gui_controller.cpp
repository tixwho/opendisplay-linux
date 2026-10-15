#include "gui_controller.hpp"

#include "session_worker.hpp"

#include "opendisplay/log.hpp"
#include "opendisplay/types.hpp"

#include <QAction>
#include <QApplication>
#include <QCoreApplication>
#include <QIcon>
#include <QMenu>
#include <QMetaObject>
#include <QRegularExpression>
#include <QStyle>
#include <QSystemTrayIcon>
#include <QTimer>

#include <cmath>
#include <cstdint>
#include <stdexcept>
#include <string>

namespace od::gui {
namespace {

QString text(const QVariantMap& values, const char* key, const char* fallback = "") {
    return values.value(QString::fromLatin1(key), QString::fromLatin1(fallback))
        .toString().trimmed();
}

od::TransportKind transport(const QString& value) {
    if (value == QStringLiteral("auto")) return od::TransportKind::Auto;
    if (value == QStringLiteral("wifi")) return od::TransportKind::Wifi;
    if (value == QStringLiteral("usb")) return od::TransportKind::Usb;
    throw std::runtime_error("Transport must be Auto, Wi-Fi, or USB.");
}

od::CaptureMode mode(const QString& value) {
    if (value == QStringLiteral("extend")) return od::CaptureMode::Extend;
    if (value == QStringLiteral("mirror")) return od::CaptureMode::Mirror;
    throw std::runtime_error("Display mode must be Extend or Mirror.");
}

od::EncoderKind encoder(const QString& value) {
    if (value == QStringLiteral("auto")) return od::EncoderKind::Auto;
    if (value == QStringLiteral("vaapi")) return od::EncoderKind::Vaapi;
    if (value == QStringLiteral("nvenc")) return od::EncoderKind::Nvenc;
    if (value == QStringLiteral("software")) return od::EncoderKind::Software;
    throw std::runtime_error("Encoder must be Auto, VA-API, NVENC, or Software.");
}

od::CompositorKind compositor(const QString& value) {
    if (value == QStringLiteral("auto")) return od::CompositorKind::Auto;
    if (value == QStringLiteral("kde")) return od::CompositorKind::Kde;
    if (value == QStringLiteral("hyprland")) return od::CompositorKind::Hyprland;
    if (value == QStringLiteral("gnome")) return od::CompositorKind::Gnome;
    throw std::runtime_error("Compositor must be Auto, KDE, Hyprland, or GNOME.");
}

od::ExtendDirection extendDirection(const QString& value) {
    if (value == QStringLiteral("left")) return od::ExtendDirection::Left;
    if (value == QStringLiteral("right")) return od::ExtendDirection::Right;
    if (value == QStringLiteral("top")) return od::ExtendDirection::Top;
    if (value == QStringLiteral("bottom")) return od::ExtendDirection::Bottom;
    throw std::runtime_error("Extension direction is invalid.");
}

od::AlignDirection alignDirection(const QString& value) {
    if (value == QStringLiteral("left")) return od::AlignDirection::Left;
    if (value == QStringLiteral("right")) return od::AlignDirection::Right;
    if (value == QStringLiteral("top")) return od::AlignDirection::Top;
    if (value == QStringLiteral("bottom")) return od::AlignDirection::Bottom;
    if (value == QStringLiteral("center")) return od::AlignDirection::Center;
    throw std::runtime_error("Monitor alignment is invalid.");
}

od::PhysicalSize physicalSize(const QString& value, const char* label) {
    static const QRegularExpression pattern(
        QStringLiteral(R"(^([0-9]+(?:\.[0-9]+)?)[xX]([0-9]+(?:\.[0-9]+)?)$)"));
    const auto match = pattern.match(value);
    if (!match.hasMatch()) {
        throw std::runtime_error(std::string(label) + " must use WIDTHxHEIGHT in millimetres.");
    }
    return od::PhysicalSize{.widthMm = match.captured(1).toDouble(),
                            .heightMm = match.captured(2).toDouble()};
}

od::Size pixelSize(const QString& value) {
    static const QRegularExpression pattern(QStringLiteral(R"(^(\d+)[xX](\d+)$)"));
    const auto match = pattern.match(value);
    if (!match.hasMatch()) {
        throw std::runtime_error("Virtual resolution must use WIDTHxHEIGHT.");
    }
    const od::Size result{.width = match.captured(1).toInt(),
                          .height = match.captured(2).toInt()};
    if (result.width < 2 || result.height < 2 || result.width > 65'535
        || result.height > 65'535) {
        throw std::runtime_error("Virtual resolution dimensions must be between 2 and 65535.");
    }
    return result;
}

od::Options optionsFrom(const QVariantMap& values) {
    od::Options options;
    options.transport = transport(text(values, "transport", "auto"));
    options.mode = mode(text(values, "mode", "extend"));
    options.encoder = encoder(text(values, "encoder", "auto"));
    options.compositor = compositor(text(values, "compositor", "auto"));
    options.host = text(values, "host").toStdString();
    options.udid = text(values, "udid").toStdString();
    options.display.referenceMonitor = text(values, "referenceMonitor").toStdString();
    options.display.extendTo = extendDirection(text(values, "extendTo", "right"));
    options.display.alignTo = alignDirection(text(values, "alignTo", "bottom"));
    options.fps = values.value(QStringLiteral("fps"), 30).toInt();
    const double bitrateMbps = values.value(QStringLiteral("bitrateMbps"), 18.0).toDouble();
    options.bitrate = static_cast<int>(std::lround(bitrateMbps * 1'000'000.0));
    options.input = values.value(QStringLiteral("input"), true).toBool();
    options.verbose = values.value(QStringLiteral("verbose"), false).toBool();

    if (options.fps < 1 || options.fps > 240) {
        throw std::runtime_error("Frame rate must be between 1 and 240.");
    }
    if (bitrateMbps < 1.0 || bitrateMbps > 200.0) {
        throw std::runtime_error("Bitrate must be between 1 and 200 Mbps.");
    }
    const QString referenceSize = text(values, "referenceSizeMm");
    if (!referenceSize.isEmpty()) {
        options.display.referencePhysicalSize = physicalSize(referenceSize, "Reference size");
    }
    const QString receiverSize = text(values, "receiverSizeMm");
    if (!receiverSize.isEmpty()) {
        options.display.receiverPhysicalSize = physicalSize(receiverSize, "Receiver size");
    }
    const QString virtualResolution = text(values, "virtualResolution");
    if (!virtualResolution.isEmpty()) {
        options.display.virtualResolution = pixelSize(virtualResolution);
    }
    const QString displayScale = text(values, "displayScale");
    if (!displayScale.isEmpty()) {
        bool valid = false;
        const double value = displayScale.toDouble(&valid);
        if (!valid || value < 0.5 || value > 4.0) {
            throw std::runtime_error("Display scale must be between 0.5 and 4.0.");
        }
        options.display.virtualScale = value;
    }
    return options;
}

}  // namespace

GuiController::GuiController(QObject* parent)
    : QObject(parent),
      settings_(QStringLiteral("OpenDisplay"), QStringLiteral("OpenDisplay")),
      worker_(new SessionWorker) {
    worker_->moveToThread(&workerThread_);
    connect(&workerThread_, &QThread::finished, worker_, &QObject::deleteLater);
    connect(worker_, &SessionWorker::stateChanged,
            this, &GuiController::applyWorkerState);
    workerThread_.start();
    createTray();
}

GuiController::~GuiController() {
    if (workerThread_.isRunning()) {
        QMetaObject::invokeMethod(worker_, &SessionWorker::stop,
                                  Qt::BlockingQueuedConnection);
        workerThread_.quit();
        workerThread_.wait();
    }
    tray_->setContextMenu(nullptr);
    delete trayMenu_;
    trayMenu_ = nullptr;
}

QVariantMap GuiController::savedSettings() const {
    return settings_.value(QStringLiteral("connection")).toMap();
}

void GuiController::connectDevice(const QVariantMap& values) {
    if (busy_ || connected_) return;
    try {
        auto options = optionsFrom(values);
        od::verboseLogging = options.verbose;
        pendingSettings_ = values;
        status_ = QStringLiteral("Connecting");
        detail_ = QStringLiteral("Preparing the connection…");
        busy_ = true;
        emit stateChanged();
        QMetaObject::invokeMethod(worker_, [worker = worker_, options = std::move(options)]() mutable {
            worker->start(std::move(options));
        }, Qt::QueuedConnection);
    } catch (const std::exception& error) {
        setLocalError(QString::fromUtf8(error.what()));
    }
}

void GuiController::connectLast() {
    QVariantMap values = savedSettings();
    if (values.isEmpty()) {
        values.insert(QStringLiteral("transport"), QStringLiteral("auto"));
        values.insert(QStringLiteral("mode"), QStringLiteral("extend"));
        values.insert(QStringLiteral("encoder"), QStringLiteral("auto"));
        values.insert(QStringLiteral("compositor"), QStringLiteral("auto"));
        values.insert(QStringLiteral("extendTo"), QStringLiteral("right"));
        values.insert(QStringLiteral("alignTo"), QStringLiteral("bottom"));
        values.insert(QStringLiteral("fps"), 30);
        values.insert(QStringLiteral("bitrateMbps"), 18.0);
        values.insert(QStringLiteral("input"), true);
    }
    connectDevice(values);
}

void GuiController::disconnectDevice() {
    if (!connected_ && !busy_) return;
    status_ = QStringLiteral("Disconnecting");
    detail_ = QStringLiteral("Stopping capture and removing the virtual monitor…");
    busy_ = true;
    emit stateChanged();
    QMetaObject::invokeMethod(worker_, &SessionWorker::stop, Qt::QueuedConnection);
}

void GuiController::quit() {
    if (quitting_) return;
    quitting_ = true;
    emit quittingChanged();
    QMetaObject::invokeMethod(worker_, &SessionWorker::stop, Qt::QueuedConnection);
    QCoreApplication::quit();
}

void GuiController::applyWorkerState(const QString& status, const QString& detail,
                                     const bool connected, const bool busy) {
    const bool wasConnected = connected_;
    status_ = status;
    detail_ = detail;
    connected_ = connected;
    busy_ = busy;
    if (connected_ && !pendingSettings_.isEmpty()) {
        settings_.setValue(QStringLiteral("connection"), pendingSettings_);
        pendingSettings_.clear();
    } else if (!busy_ && !connected_) {
        pendingSettings_.clear();
    }
    connectAction_->setEnabled(!connected_ && !busy_);
    disconnectAction_->setEnabled(connected_ || busy_);
    tray_->setToolTip(connected_ ? QStringLiteral("OpenDisplay — Connected")
                                 : QStringLiteral("OpenDisplay — %1").arg(status_));
    emit stateChanged();

    if (trayAvailable_ && connected_ && !wasConnected) {
        tray_->showMessage(QStringLiteral("OpenDisplay"), detail_,
                           QSystemTrayIcon::Information, 3000);
    } else if (trayAvailable_ && status_ == QStringLiteral("Connection failed")) {
        tray_->showMessage(QStringLiteral("OpenDisplay connection failed"), detail_,
                           QSystemTrayIcon::Warning, 5000);
    }
}

void GuiController::createTray() {
    tray_ = new QSystemTrayIcon(this);
    QIcon trayIcon = QIcon::fromTheme(QStringLiteral("video-display"));
    if (trayIcon.isNull()) {
        trayIcon = QApplication::style()->standardIcon(QStyle::SP_ComputerIcon);
    }
    tray_->setIcon(trayIcon);
    tray_->setToolTip(QStringLiteral("OpenDisplay — Disconnected"));
    trayMenu_ = new QMenu;
    auto* showAction = trayMenu_->addAction(QIcon::fromTheme(QStringLiteral("window")),
                                            QStringLiteral("Show OpenDisplay"));
    trayMenu_->addSeparator();
    connectAction_ = trayMenu_->addAction(QStringLiteral("Connect"));
    disconnectAction_ = trayMenu_->addAction(QStringLiteral("Disconnect"));
    disconnectAction_->setEnabled(false);
    trayMenu_->addSeparator();
    auto* quitAction = trayMenu_->addAction(QStringLiteral("Quit"));
    tray_->setContextMenu(trayMenu_);

    connect(showAction, &QAction::triggered, this, &GuiController::showWindowRequested);
    connect(connectAction_, &QAction::triggered, this, &GuiController::connectLast);
    connect(disconnectAction_, &QAction::triggered, this, &GuiController::disconnectDevice);
    connect(quitAction, &QAction::triggered, this, &GuiController::quit);
    connect(tray_, &QSystemTrayIcon::activated, this,
            [this](const QSystemTrayIcon::ActivationReason reason) {
                if (reason == QSystemTrayIcon::Trigger
                    || reason == QSystemTrayIcon::DoubleClick) {
                    emit showWindowRequested();
                }
            });

    // Keep the icon visible even if the StatusNotifier host starts later. Qt
    // registers it automatically once a compatible tray becomes available.
    tray_->show();
    trayPoll_ = new QTimer(this);
    trayPoll_->setInterval(2000);
    connect(trayPoll_, &QTimer::timeout, this, &GuiController::updateTrayAvailability);
    trayPoll_->start();
    updateTrayAvailability();
}

void GuiController::updateTrayAvailability() {
    const bool available = QSystemTrayIcon::isSystemTrayAvailable();
    if (available == trayAvailable_) return;
    const bool disappeared = trayAvailable_ && !available;
    trayAvailable_ = available;
    emit trayAvailableChanged();
    if (disappeared) emit showWindowRequested();
}

void GuiController::setLocalError(const QString& message) {
    status_ = QStringLiteral("Invalid settings");
    detail_ = message;
    connected_ = false;
    busy_ = false;
    emit stateChanged();
}

}  // namespace od::gui
