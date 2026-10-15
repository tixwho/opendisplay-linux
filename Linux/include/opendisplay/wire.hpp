#pragma once

#include "opendisplay/types.hpp"

#include <QJsonObject>

#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>

namespace od::wire {

constexpr int protocolVersion = 2;
constexpr int minSupportedPeer = 1;
constexpr std::uint32_t maxControlSize = 1U << 20U;

std::string frame(std::string_view payload);
std::optional<std::uint32_t> decodeLength(std::span<const char, 4> header);
std::optional<QJsonObject> parseJson(std::string_view payload);
std::optional<PhoneInfo> parseHello(const QJsonObject& object);
std::string welcome();
std::string pong(double phoneTime, double hostTime);
std::string cursor(double x, double y, bool visible);
std::string cursorImage(double width, double height, double anchorX, double anchorY,
                        std::string_view pngBase64);
std::string videoPayload(const EncodedFrame& frame, std::int64_t sentAtMs);
bool containsAnnexBStartCode(std::string_view bytes);

}  // namespace od::wire

