#include "opendisplay/discovery.hpp"
#include "opendisplay/desktop_backend_factory.hpp"
#include "opendisplay/log.hpp"
#include "opendisplay/session.hpp"
#include "opendisplay/types.hpp"

#include <QCommandLineOption>
#include <QCommandLineParser>
#include <QGuiApplication>
#include <QRegularExpression>
#include <QTimer>

#include <atomic>
#include <chrono>
#include <csignal>
#include <cstdint>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <string>

namespace {

std::atomic_bool interrupted = false;

void handleSignal(int) { interrupted.store(true); }

od::TransportKind transport(const QString& value) {
    if (value == QStringLiteral("auto")) return od::TransportKind::Auto;
    if (value == QStringLiteral("wifi")) return od::TransportKind::Wifi;
    if (value == QStringLiteral("usb")) return od::TransportKind::Usb;
    throw std::runtime_error("--transport must be auto, wifi, or usb");
}

od::CaptureMode mode(const QString& value) {
    if (value == QStringLiteral("extend")) return od::CaptureMode::Extend;
    if (value == QStringLiteral("mirror")) return od::CaptureMode::Mirror;
    throw std::runtime_error("--mode must be extend or mirror");
}

od::EncoderKind encoder(const QString& value) {
    if (value == QStringLiteral("auto")) return od::EncoderKind::Auto;
    if (value == QStringLiteral("vaapi")) return od::EncoderKind::Vaapi;
    if (value == QStringLiteral("nvenc")) return od::EncoderKind::Nvenc;
    if (value == QStringLiteral("software")) return od::EncoderKind::Software;
    throw std::runtime_error("--encoder must be auto, vaapi, nvenc, or software");
}

od::CompositorKind compositor(const QString& value) {
    if (value == QStringLiteral("auto")) return od::CompositorKind::Auto;
    if (value == QStringLiteral("kde")) return od::CompositorKind::Kde;
    if (value == QStringLiteral("hyprland")) return od::CompositorKind::Hyprland;
    if (value == QStringLiteral("gnome")) return od::CompositorKind::Gnome;
    throw std::runtime_error("--compositor must be auto, kde, hyprland, or gnome");
}

od::ExtendDirection extendDirection(const QString& value) {
    if (value == QStringLiteral("left")) return od::ExtendDirection::Left;
    if (value == QStringLiteral("right")) return od::ExtendDirection::Right;
    if (value == QStringLiteral("top")) return od::ExtendDirection::Top;
    if (value == QStringLiteral("bottom")) return od::ExtendDirection::Bottom;
    throw std::runtime_error("--extend-to must be left, right, top, or bottom");
}

od::AlignDirection alignDirection(const QString& value) {
    if (value == QStringLiteral("left")) return od::AlignDirection::Left;
    if (value == QStringLiteral("right")) return od::AlignDirection::Right;
    if (value == QStringLiteral("top")) return od::AlignDirection::Top;
    if (value == QStringLiteral("bottom")) return od::AlignDirection::Bottom;
    if (value == QStringLiteral("center")) return od::AlignDirection::Center;
    throw std::runtime_error("--align-to must be left, right, top, bottom, or center");
}

od::Size size(const QString& value, const std::string& option) {
    static const QRegularExpression pattern(QStringLiteral(R"(^(\d+)[xX](\d+)$)"));
    const auto match = pattern.match(value);
    if (!match.hasMatch()) {
        throw std::runtime_error(option + " must use WIDTHxHEIGHT");
    }
    const od::Size result{.width = match.captured(1).toInt(),
                          .height = match.captured(2).toInt()};
    if (result.width < 2 || result.height < 2 || result.width > 65'535
        || result.height > 65'535) {
        throw std::runtime_error(option + " dimensions must be between 2 and 65535");
    }
    return result;
}

od::PhysicalSize physicalSize(const QString& value, const std::string& option) {
    static const QRegularExpression pattern(
        QStringLiteral(R"(^([0-9]+(?:\.[0-9]+)?)[xX]([0-9]+(?:\.[0-9]+)?)$)"));
    const auto match = pattern.match(value);
    if (!match.hasMatch()) {
        throw std::runtime_error(option + " must use WIDTHxHEIGHT in millimetres");
    }
    const od::PhysicalSize result{.widthMm = match.captured(1).toDouble(),
                                  .heightMm = match.captured(2).toDouble()};
    if (result.widthMm <= 0 || result.heightMm <= 0) {
        throw std::runtime_error(option + " dimensions must be positive");
    }
    return result;
}

od::Rect geometry(const QString& value) {
    static const QRegularExpression pattern(
        QStringLiteral(R"(^(\d+)[xX](\d+)([+-]\d+)([+-]\d+)$)"));
    const auto match = pattern.match(value);
    if (!match.hasMatch()) {
        throw std::runtime_error("--reference-geometry must use WIDTHxHEIGHT+X+Y");
    }
    const od::Rect result{.x = match.captured(3).toInt(),
                          .y = match.captured(4).toInt(),
                          .width = match.captured(1).toInt(),
                          .height = match.captured(2).toInt()};
    if (result.width <= 0 || result.height <= 0) {
        throw std::runtime_error("--reference-geometry dimensions must be positive");
    }
    return result;
}

double displayScale(const QString& value, const std::string& option) {
    bool valid = false;
    const double result = value.toDouble(&valid);
    if (!valid || result < 0.5 || result > 4.0) {
        throw std::runtime_error(option + " must be between 0.5 and 4.0");
    }
    return result;
}

int positiveInt(const QCommandLineParser& parser, const QCommandLineOption& option) {
    bool valid = false;
    const int result = parser.value(option).toInt(&valid);
    if (!valid || result <= 0) {
        throw std::runtime_error(("invalid --" + option.names().front()).toStdString());
    }
    return result;
}

void printEndpoint(const od::Endpoint& endpoint) {
    if (endpoint.kind == od::TransportKind::Usb) {
        std::cout << "usb   " << endpoint.udid << "  " << endpoint.name << '\n';
    } else {
        std::cout << "wifi  " << endpoint.host << ':' << endpoint.port << "  "
                  << endpoint.name << '\n';
    }
}

}  // namespace

int main(int argc, char* argv[]) {
    QGuiApplication::setDesktopSettingsAware(false);
    QGuiApplication application(argc, argv);
    std::signal(SIGPIPE, SIG_IGN);
    QCoreApplication::setApplicationName(QStringLiteral("opendisplay-linux"));
    QCoreApplication::setApplicationVersion(QStringLiteral("0.1.0"));
    QCommandLineParser parser;
    parser.setApplicationDescription(QStringLiteral(
        "Use an unchanged OpenDisplay iOS app as a Wayland display."));
    parser.addHelpOption();
    parser.addVersionOption();

    const QCommandLineOption transportOption({"t", "transport"},
        "Connection method: auto, wifi, or usb.", "kind", "auto");
    const QCommandLineOption hostOption("host", "Connect directly to a Wi-Fi host.", "address");
    const QCommandLineOption portOption({"p", "port"}, "Receiver TCP port.", "port", "9000");
    const QCommandLineOption serviceOption("service", "Select a Bonjour service name.", "name");
    const QCommandLineOption udidOption("udid", "Select a USB device by UDID.", "udid");
    const QCommandLineOption modeOption({"m", "mode"}, "Display mode: extend or mirror.",
                                        "mode", "extend");
    const QCommandLineOption encoderOption({"e", "encoder"},
        "FFmpeg encoder: auto, vaapi, nvenc, or software.", "encoder", "auto");
    const QCommandLineOption compositorOption("compositor",
        "Desktop backend: auto, kde, hyprland, or gnome.", "name", "auto");
    const QCommandLineOption vaapiOption("vaapi-device", "VA-API render node.", "path",
                                         "/dev/dri/renderD128");
    const QCommandLineOption fpsOption("fps", "Maximum frame rate.", "fps", "30");
    const QCommandLineOption bitrateOption("bitrate", "H.264 bitrate in bits/second.",
                                            "bps", "18000000");
    const QCommandLineOption scaleOption("scale", "Encoded resolution multiplier.", "factor",
                                          "1.0");
    const QCommandLineOption referenceOption("reference-monitor",
        "Reference monitor name or backend id (required when multiple are enabled).", "output");
    const QCommandLineOption extendOption("extend-to",
        "Place the virtual monitor on this side: left, right, top, or bottom.",
        "side", "right");
    const QCommandLineOption alignOption("align-to",
        "Align its perpendicular edge: top/bottom/left/right/center.", "edge", "bottom");
    const QCommandLineOption virtualResolutionOption("virtual-resolution",
        "Override the receiver-native virtual mode.", "WIDTHxHEIGHT");
    const QCommandLineOption displayScaleOption({"display-scale", "virtual-scale"},
        "Override the auto-selected virtual output scale.", "factor");
    const QCommandLineOption virtualRefreshOption("virtual-refresh",
        "Override virtual-output refresh (defaults to --fps).", "hz");
    const QCommandLineOption referenceGeometryOption("reference-geometry",
        "Override detected reference logical geometry.", "WIDTHxHEIGHT+X+Y");
    const QCommandLineOption referenceResolutionOption("reference-resolution",
        "Override detected reference pixel resolution for scale calculations.", "WIDTHxHEIGHT");
    const QCommandLineOption referenceScaleOption("reference-scale",
        "Override detected reference scale for calculations.", "factor");
    const QCommandLineOption referenceSizeOption("reference-size-mm",
        "Override detected reference physical dimensions.", "WIDTHxHEIGHT");
    const QCommandLineOption receiverSizeOption({"ipad-size-mm", "receiver-size-mm"},
        "Receiver panel dimensions for physical-DPI auto scaling.", "WIDTHxHEIGHT");
    const QCommandLineOption noInputOption("no-input", "Do not request pointer control.");
    const QCommandLineOption listOption({"l", "list"}, "List visible Wi-Fi and USB receivers.");
    const QCommandLineOption verboseOption("verbose", "Enable diagnostic logging.");
    parser.addOptions({transportOption, hostOption, portOption, serviceOption, udidOption,
                       modeOption, encoderOption, compositorOption, vaapiOption, fpsOption, bitrateOption,
                       scaleOption, referenceOption, extendOption, alignOption,
                       virtualResolutionOption, displayScaleOption, virtualRefreshOption,
                       referenceGeometryOption, referenceResolutionOption,
                       referenceScaleOption, referenceSizeOption, receiverSizeOption,
                       noInputOption, listOption, verboseOption});
    parser.process(application);

    try {
        od::Options options;
        options.transport = transport(parser.value(transportOption));
        options.mode = mode(parser.value(modeOption));
        options.encoder = encoder(parser.value(encoderOption));
        options.compositor = compositor(parser.value(compositorOption));
        options.host = parser.value(hostOption).toStdString();
        const int port = positiveInt(parser, portOption);
        if (port > 65'535) {
            throw std::runtime_error("--port must be between 1 and 65535");
        }
        options.port = static_cast<std::uint16_t>(port);
        options.serviceName = parser.value(serviceOption).toStdString();
        options.udid = parser.value(udidOption).toStdString();
        options.vaapiDevice = parser.value(vaapiOption).toStdString();
        options.fps = positiveInt(parser, fpsOption);
        options.bitrate = positiveInt(parser, bitrateOption);
        bool scaleValid = false;
        options.scale = parser.value(scaleOption).toDouble(&scaleValid);
        if (!scaleValid || options.scale <= 0 || options.scale > 1) {
            throw std::runtime_error("--scale must be greater than 0 and at most 1");
        }
        options.display.referenceMonitor = parser.value(referenceOption).toStdString();
        options.display.extendTo = extendDirection(parser.value(extendOption));
        if (parser.isSet(alignOption)) {
            options.display.alignTo = alignDirection(parser.value(alignOption));
        } else if (options.display.extendTo == od::ExtendDirection::Top
                   || options.display.extendTo == od::ExtendDirection::Bottom) {
            // Keep the default corner at bottom-right when only the extension
            // axis is changed: vertical extensions align their right edges.
            options.display.alignTo = od::AlignDirection::Right;
        }
        if (parser.isSet(virtualResolutionOption)) {
            options.display.virtualResolution = size(
                parser.value(virtualResolutionOption), "--virtual-resolution");
        }
        if (parser.isSet(displayScaleOption)) {
            options.display.virtualScale = displayScale(
                parser.value(displayScaleOption), "--display-scale");
        }
        if (parser.isSet(virtualRefreshOption)) {
            options.display.refreshRate = positiveInt(parser, virtualRefreshOption);
        }
        if (parser.isSet(referenceGeometryOption)) {
            options.display.referenceGeometry = geometry(parser.value(referenceGeometryOption));
        }
        if (parser.isSet(referenceResolutionOption)) {
            options.display.referenceResolution = size(
                parser.value(referenceResolutionOption), "--reference-resolution");
        }
        if (parser.isSet(referenceScaleOption)) {
            options.display.referenceScale = displayScale(
                parser.value(referenceScaleOption), "--reference-scale");
        }
        if (parser.isSet(referenceSizeOption)) {
            options.display.referencePhysicalSize = physicalSize(
                parser.value(referenceSizeOption), "--reference-size-mm");
        }
        if (parser.isSet(receiverSizeOption)) {
            options.display.receiverPhysicalSize = physicalSize(
                parser.value(receiverSizeOption), "--ipad-size-mm");
        }
        options.input = !parser.isSet(noInputOption);
        options.listDevices = parser.isSet(listOption);
        options.verbose = parser.isSet(verboseOption);
        od::verboseLogging = options.verbose;

        if (options.listDevices) {
            try {
                for (const auto& endpoint : od::discoverUsb(std::chrono::seconds(2))) {
                    printEndpoint(endpoint);
                }
            } catch (const std::exception& error) {
                od::log(error.what());
            }
            try {
                for (const auto& endpoint : od::discoverWifi(std::chrono::seconds(3))) {
                    printEndpoint(endpoint);
                }
            } catch (const std::exception& error) {
                od::log(error.what());
            }
            return 0;
        }

        const auto endpoint = od::chooseEndpoint(options);
        od::Session session(options, od::makeDesktopBackend(options.compositor));
        session.start(endpoint);
        std::signal(SIGINT, handleSignal);
        std::signal(SIGTERM, handleSignal);
        QTimer timer;
        QObject::connect(&timer, &QTimer::timeout, &application, [&] {
            try {
                if (interrupted.load() || !session.tick()) {
                    application.quit();
                }
            } catch (const std::exception& error) {
                od::log(std::string("Session error: ") + error.what());
                application.exit(1);
            }
        });
        timer.start(20);
        const int result = application.exec();
        session.stop();
        return result;
    } catch (const std::exception& error) {
        std::cerr << "opendisplay-linux: " << error.what() << '\n';
        return 1;
    }
}
