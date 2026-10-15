#include "opendisplay/desktop_backend_factory.hpp"

#include "opendisplay/log.hpp"

#ifndef OD_DESKTOP_FACTORY_TEST_ONLY
#include "opendisplay/hyprland_portal.hpp"
#include "opendisplay/gnome_backend.hpp"
#include "opendisplay/kde_portal.hpp"
#endif

#include <QByteArray>

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <string>

namespace od {

CompositorKind detectCompositor(const CompositorKind requested,
                                const std::string_view currentDesktop,
                                const std::string_view hyprlandSignature,
                                const std::string_view kdeSession) {
    if (requested != CompositorKind::Auto) return requested;
    std::string desktop(currentDesktop);
    std::transform(desktop.begin(), desktop.end(), desktop.begin(), [](const unsigned char c) {
        return static_cast<char>(std::tolower(c));
    });
    if (!hyprlandSignature.empty() || desktop.find("hyprland") != std::string::npos) {
        return CompositorKind::Hyprland;
    }
    if (desktop.find("gnome") != std::string::npos) {
        return CompositorKind::Gnome;
    }
    if (!kdeSession.empty() || desktop.find("kde") != std::string::npos
        || desktop.find("plasma") != std::string::npos) {
        return CompositorKind::Kde;
    }
    // Preserve the original KDE-only CLI behavior for unknown environments.
    return CompositorKind::Kde;
}

std::unique_ptr<DesktopBackend> makeDesktopBackend(const CompositorKind requested) {
#ifdef OD_DESKTOP_FACTORY_TEST_ONLY
    static_cast<void>(requested);
    return {};
#else
    const char* desktop = std::getenv("XDG_CURRENT_DESKTOP");
    const char* hyprland = std::getenv("HYPRLAND_INSTANCE_SIGNATURE");
    const char* kde = std::getenv("KDE_FULL_SESSION");
    const auto selected = detectCompositor(
        requested, desktop ? desktop : "", hyprland ? hyprland : "", kde ? kde : "");
    if (selected == CompositorKind::Hyprland) {
        debug("Desktop backend: Hyprland");
        return std::make_unique<HyprlandPortal>();
    }
    if (selected == CompositorKind::Gnome) {
        debug("Desktop backend: GNOME Mutter");
        return std::make_unique<GnomeBackend>();
    }
    debug("Desktop backend: KDE");
    return std::make_unique<KdePortal>();
#endif
}

}  // namespace od
