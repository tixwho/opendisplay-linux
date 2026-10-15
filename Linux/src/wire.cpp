#include "opendisplay/wire.hpp"

#include <QJsonDocument>
#include <QJsonValue>

#include <array>
#include <algorithm>
#include <cstring>

namespace od::wire {

std::string frame(const std::string_view payload) {
    const auto size = static_cast<std::uint32_t>(payload.size());
    std::string output(4, '\0');
    output[0] = static_cast<char>((size >> 24U) & 0xffU);
    output[1] = static_cast<char>((size >> 16U) & 0xffU);
    output[2] = static_cast<char>((size >> 8U) & 0xffU);
    output[3] = static_cast<char>(size & 0xffU);
    output.append(payload);
    return output;
}

std::optional<std::uint32_t> decodeLength(const std::span<const char, 4> header) {
    const auto size = (static_cast<std::uint32_t>(static_cast<unsigned char>(header[0])) << 24U)
        | (static_cast<std::uint32_t>(static_cast<unsigned char>(header[1])) << 16U)
        | (static_cast<std::uint32_t>(static_cast<unsigned char>(header[2])) << 8U)
        | static_cast<std::uint32_t>(static_cast<unsigned char>(header[3]));
    if (size == 0 || size >= maxControlSize) {
        return std::nullopt;
    }
    return size;
}

std::optional<QJsonObject> parseJson(const std::string_view payload) {
    QJsonParseError error;
    const auto document = QJsonDocument::fromJson(
        QByteArray(payload.data(), static_cast<qsizetype>(payload.size())), &error);
    if (error.error != QJsonParseError::NoError || !document.isObject()) {
        return std::nullopt;
    }
    return document.object();
}

std::optional<PhoneInfo> parseHello(const QJsonObject& object) {
    if (object.value("type").toString() != QStringLiteral("hello")) {
        return std::nullopt;
    }
    PhoneInfo info;
    info.pixelsWide = object.value("pixelsWide").toInt();
    info.pixelsHigh = object.value("pixelsHigh").toInt();
    info.scale = object.value("scale").toDouble(2.0);
    info.device = object.value("device").toString(QStringLiteral("device")).toStdString();
    info.installId = object.value("id").toString().toStdString();
    info.protocolVersion = object.value("pv").toInt(1);
    if (info.pixelsWide <= 0 || info.pixelsHigh <= 0) {
        return std::nullopt;
    }
    return info;
}

std::string welcome() {
    QJsonObject object{
        {"type", "welcome"},
        {"pv", protocolVersion},
        {"min", minSupportedPeer},
    };
    return QJsonDocument(object).toJson(QJsonDocument::Compact).toStdString();
}

std::string pong(const double phoneTime, const double hostTime) {
    QJsonObject object{{"type", "pong"}, {"t", phoneTime}, {"mt", hostTime}};
    return QJsonDocument(object).toJson(QJsonDocument::Compact).toStdString();
}

std::string cursor(const double x, const double y, const bool visible) {
    QJsonObject object{{QStringLiteral("type"), QStringLiteral("cursor")},
                       {QStringLiteral("x"), x}, {QStringLiteral("y"), y},
                       {QStringLiteral("v"), visible ? 1 : 0}};
    return QJsonDocument(object).toJson(QJsonDocument::Compact).toStdString();
}

std::string cursorImage(const double width, const double height, const double anchorX,
                        const double anchorY, const std::string_view pngBase64) {
    QJsonObject object{{QStringLiteral("type"), QStringLiteral("cursorImg")},
                       {QStringLiteral("nw"), width}, {QStringLiteral("nh"), height},
                       {QStringLiteral("ax"), anchorX}, {QStringLiteral("ay"), anchorY},
                       {QStringLiteral("png"), QString::fromUtf8(
                           pngBase64.data(), static_cast<qsizetype>(pngBase64.size()))}};
    return QJsonDocument(object).toJson(QJsonDocument::Compact).toStdString();
}

std::string videoPayload(const EncodedFrame& encoded, const std::int64_t sentAtMs) {
    const auto telemetry = std::string("{\"cap\":") + std::to_string(encoded.capturedAtMs)
        + ",\"snd\":" + std::to_string(sentAtMs) + "}";
    return telemetry + encoded.annexB;
}

bool containsAnnexBStartCode(const std::string_view bytes) {
    static constexpr std::array<char, 4> start{0, 0, 0, 1};
    return std::search(bytes.begin(), bytes.end(), start.begin(), start.end()) != bytes.end();
}

}  // namespace od::wire
