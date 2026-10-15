#pragma once

#include "opendisplay/desktop_backend.hpp"
#include "opendisplay/kde_portal.hpp"

#include <QDBusConnection>
#include <QDBusMessage>
#include <QObject>
#include <QString>
#include <QVariantList>

#include <cstdint>
#include <memory>
#include <optional>

namespace od {

/// GNOME's Mutter backend creates a real virtual output with RecordVirtual.
/// Mutter's ScreenCast D-Bus API is private and versioned with GNOME Shell.
class GnomeBackend final : public QObject, public DesktopBackend {
    Q_OBJECT

public:
    explicit GnomeBackend(QObject* parent = nullptr);
    ~GnomeBackend() override;

    PortalCapture start(const DesktopRequest& request) override;
    void stop() override;
    void pointer(std::string_view phase, double normalizedX, double normalizedY) override;
    void scroll(double dx, double dy) override;

private slots:
    void pipeWireStreamAdded(uint nodeId);

private:
    QDBusMessage call(const QString& service, const QString& path,
                      const QString& interface, const QString& method,
                      const QVariantList& arguments = {});
    void notify(const QString& method, const QVariantList& arguments);

    QDBusConnection bus_;
    QString remoteSessionPath_;
    QString screenCastSessionPath_;
    QString streamPath_;
    std::optional<std::uint32_t> nodeId_;
    int logicalWidth_ = 0;
    int logicalHeight_ = 0;
    bool inputEnabled_ = false;
    bool pointerDown_ = false;
    std::unique_ptr<KdePortal> portalMirror_;
};

}  // namespace od
