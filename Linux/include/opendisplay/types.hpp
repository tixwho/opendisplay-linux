#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace od {

enum class TransportKind { Auto, Wifi, Usb };
enum class CaptureMode { Extend, Mirror };
enum class EncoderKind { Auto, Vaapi, Nvenc, Software };
enum class CompositorKind { Auto, Kde, Hyprland, Gnome };
enum class ExtendDirection { Left, Right, Top, Bottom };
enum class AlignDirection { Left, Right, Top, Bottom, Center };

struct Size {
    int width = 0;
    int height = 0;
};

struct Rect {
    int x = 0;
    int y = 0;
    int width = 0;
    int height = 0;
};

struct PhysicalSize {
    double widthMm = 0;
    double heightMm = 0;
};

struct DisplayOptions {
    std::string referenceMonitor;
    ExtendDirection extendTo = ExtendDirection::Right;
    AlignDirection alignTo = AlignDirection::Bottom;
    std::optional<Size> virtualResolution;
    std::optional<double> virtualScale;
    std::optional<Rect> referenceGeometry;
    std::optional<Size> referenceResolution;
    std::optional<double> referenceScale;
    std::optional<PhysicalSize> referencePhysicalSize;
    std::optional<PhysicalSize> receiverPhysicalSize;
    std::optional<int> refreshRate;
};

struct PhoneInfo {
    int pixelsWide = 0;
    int pixelsHigh = 0;
    double scale = 2.0;
    std::string device = "device";
    std::string installId;
    int protocolVersion = 1;
};

struct Endpoint {
    TransportKind kind = TransportKind::Wifi;
    std::string name;
    std::string host;
    std::uint16_t port = 9000;
    std::string udid;
    int usbHandle = -1;
};

struct VideoFormat {
    enum class PixelFormat { Bgra, Bgrx, Rgba, Rgbx };

    int width = 0;
    int height = 0;
    int stride = 0;
    int fps = 30;
    PixelFormat pixelFormat = PixelFormat::Bgra;
};

struct CapturedFrame {
    VideoFormat format;
    std::int64_t capturedAtMs = 0;
    std::uint64_t sequence = 0;
    std::string bytes;
};

struct CursorBitmap {
    int width = 0;
    int height = 0;
    int stride = 0;
    int hotspotX = 0;
    int hotspotY = 0;
    VideoFormat::PixelFormat pixelFormat = VideoFormat::PixelFormat::Bgra;
    std::vector<std::uint8_t> bytes;
};

struct CursorUpdate {
    int x = 0;
    int y = 0;
    bool visible = false;
    std::optional<CursorBitmap> bitmap;
};

struct EncodedFrame {
    std::int64_t capturedAtMs = 0;
    bool keyframe = false;
    std::string annexB;
};

struct Options {
    TransportKind transport = TransportKind::Auto;
    CaptureMode mode = CaptureMode::Extend;
    EncoderKind encoder = EncoderKind::Auto;
    CompositorKind compositor = CompositorKind::Auto;
    std::string host;
    std::uint16_t port = 9000;
    std::string serviceName;
    std::string udid;
    std::string vaapiDevice = "/dev/dri/renderD128";
    int fps = 30;
    int bitrate = 18'000'000;
    double scale = 1.0;
    DisplayOptions display;
    bool input = true;
    bool listDevices = false;
    bool verbose = false;
};

}  // namespace od
