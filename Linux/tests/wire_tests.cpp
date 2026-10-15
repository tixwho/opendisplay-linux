#include "opendisplay/wire.hpp"

#include <QJsonObject>

#include <array>
#include <cassert>
#include <cstdint>
#include <string>

int main() {
    const std::string payload = R"({"type":"ping","t":12})";
    const auto framed = od::wire::frame(payload);
    assert(framed.size() == payload.size() + 4);
    std::array<char, 4> header{framed[0], framed[1], framed[2], framed[3]};
    const auto decoded = od::wire::decodeLength(header);
    assert(decoded && *decoded == payload.size());
    assert(framed.substr(4) == payload);

    const auto hello = od::wire::parseJson(
        R"({"type":"hello","pixelsWide":2732,"pixelsHigh":2048,"scale":2,"device":"iPad","id":"test","pv":2})");
    assert(hello);
    const auto phone = od::wire::parseHello(*hello);
    assert(phone);
    assert(phone->pixelsWide == 2732);
    assert(phone->pixelsHigh == 2048);
    assert(phone->device == "iPad");
    assert(phone->protocolVersion == 2);

    assert(!od::wire::parseHello(QJsonObject{{"type", "hello"}}));
    assert(od::wire::containsAnnexBStartCode(std::string("abc\0\0\0\1xyz", 10)));
    assert(!od::wire::containsAnnexBStartCode("not h264"));

    od::EncodedFrame encoded{.capturedAtMs = 100, .annexB = std::string("\0\0\0\1x", 5)};
    const auto video = od::wire::videoPayload(encoded, 120);
    assert(video.starts_with("{\"cap\":100,\"snd\":120}"));
    assert(video.ends_with(encoded.annexB));

    const auto cursor = od::wire::parseJson(od::wire::cursor(0.25, 0.75, true));
    assert(cursor && cursor->value("type").toString() == "cursor");
    assert(cursor->value("x").toDouble() == 0.25);
    assert(cursor->value("y").toDouble() == 0.75);
    assert(cursor->value("v").toInt() == 1);

    const auto cursorImage = od::wire::parseJson(
        od::wire::cursorImage(0.02, 0.03, 0.1, 0.2, "aGVsbG8="));
    assert(cursorImage && cursorImage->value("type").toString() == "cursorImg");
    assert(cursorImage->value("png").toString() == "aGVsbG8=");
    assert(cursorImage->value("nw").toDouble() == 0.02);
    assert(cursorImage->value("ax").toDouble() == 0.1);
    return 0;
}
