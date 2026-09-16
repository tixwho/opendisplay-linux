#include "opendisplay/display_layout.hpp"

#include <cassert>
#include <cmath>
#include <stdexcept>
#include <vector>

namespace {

od::DisplayOutput monitor(std::string id, std::string name) {
    return {
        .id = std::move(id),
        .name = std::move(name),
        .connected = true,
        .enabled = true,
        .resolution = {.width = 2560, .height = 1440},
        .logicalGeometry = {.x = 100, .y = 200, .width = 2048, .height = 1152},
        .scale = 1.25,
    };
}

od::PhoneInfo ipad() {
    return {.pixelsWide = 2420, .pixelsHigh = 1668, .scale = 2.0, .device = "iPad"};
}

void selectsReferenceDeterministically() {
    auto first = monitor("1", "eDP-1");
    auto second = monitor("2", "DP-1");
    assert(od::selectReferenceOutput({first}, {}).name == "eDP-1");
    assert(od::selectReferenceOutput({first, second}, "DP-1").id == "2");
    assert(od::selectReferenceOutput({first, second}, "1").name == "eDP-1");
    bool rejected = false;
    try {
        static_cast<void>(od::selectReferenceOutput({first, second}, {}));
    } catch (const std::runtime_error&) {
        rejected = true;
    }
    assert(rejected);
}

void detectsAddedOutputAcrossKscreenRenumbering() {
    const std::vector before{monitor("1", "eDP-1")};
    const std::vector after{
        monitor("1", "Virtual-virtual-xdp-kde-org.kde.konsole"),
        monitor("2", "eDP-1"),
    };
    assert(od::sameDisplayOutput(before.front(), after.back()));
    const auto added = od::addedDisplayOutputs(before, after);
    assert(added.size() == 1);
    assert(added.front().name == "Virtual-virtual-xdp-kde-org.kde.konsole");
    assert(added.front().id == "1");
}

void defaultsToBottomRight() {
    const auto layout = od::planDisplayLayout(monitor("1", "eDP-1"), ipad(), {}, 60);
    assert(layout.resolution.width == 2416);
    assert(layout.resolution.height == 1668);
    assert(std::abs(layout.scale - 2.0) < 0.001);
    assert(layout.logicalGeometry.width == 1208);
    assert(layout.logicalGeometry.height == 834);
    assert(layout.logicalGeometry.x == 2148);
    assert(layout.logicalGeometry.y == 518);
}

void makesFractionalModesLogicallyIntegral() {
    od::DisplayOptions options;
    options.virtualScale = 1.25;
    const auto layout = od::planDisplayLayout(monitor("1", "eDP-1"), ipad(), options, 60);
    assert(layout.resolution.width == 2400);
    assert(layout.resolution.height == 1670);
    assert(layout.logicalGeometry.width == 1920);
    assert(layout.logicalGeometry.height == 1336);
    assert(layout.adjustedResolution);
}

void makesKdeCustomModesCvtCompatible() {
    od::DisplayOptions options;
    options.virtualResolution = od::Size{.width = 2420, .height = 1668};
    options.virtualScale = 1.0;
    const auto layout = od::planDisplayLayout(monitor("1", "eDP-1"), ipad(), options, 60);
    assert(layout.resolution.width == 2416);
    assert(layout.resolution.width % 8 == 0);
    assert(layout.resolution.height == 1668);
}

void keepsHyprlandNativeModeWhenLogicalSizeIsIntegral() {
    od::DisplayOptions options;
    options.virtualResolution = od::Size{.width = 2420, .height = 1668};
    options.virtualScale = 2.0;
    const auto layout = od::planDisplayLayout(
        monitor("1", "eDP-1"), ipad(), options, 60,
        od::DisplayModePolicy::IntegerLogicalSize);
    assert(layout.resolution.width == 2420);
    assert(layout.resolution.height == 1668);
    assert(!layout.adjustedResolution);
}

void supportsEveryPlacementAxis() {
    od::DisplayOptions options;
    options.virtualResolution = od::Size{.width = 1000, .height = 800};
    options.virtualScale = 1.0;
    options.extendTo = od::ExtendDirection::Bottom;
    options.alignTo = od::AlignDirection::Right;
    options.referenceGeometry = od::Rect{.x = 20, .y = 30, .width = 1600, .height = 900};
    const auto bottom = od::planDisplayLayout(monitor("1", "eDP-1"), ipad(), options, 75);
    assert(bottom.logicalGeometry.x == 620);
    assert(bottom.logicalGeometry.y == 930);
    assert(bottom.refreshRate == 75);

    options.extendTo = od::ExtendDirection::Left;
    options.alignTo = od::AlignDirection::Center;
    const auto left = od::planDisplayLayout(monitor("1", "eDP-1"), ipad(), options, 60);
    assert(left.logicalGeometry.x == -980);
    assert(left.logicalGeometry.y == 80);
}

void centersAboveAndBelowReference() {
    od::DisplayOptions options;
    options.virtualResolution = od::Size{.width = 1000, .height = 800};
    options.virtualScale = 1.0;
    options.alignTo = od::AlignDirection::Center;
    options.referenceGeometry = od::Rect{.x = 20, .y = 30, .width = 1600, .height = 900};
    options.extendTo = od::ExtendDirection::Top;
    const auto top = od::planDisplayLayout(monitor("1", "eDP-1"), ipad(), options, 60);
    assert(top.logicalGeometry.x == 320);
    assert(top.logicalGeometry.y == -770);
    options.extendTo = od::ExtendDirection::Bottom;
    const auto bottom = od::planDisplayLayout(monitor("1", "eDP-1"), ipad(), options, 60);
    assert(bottom.logicalGeometry.x == 320);
    assert(bottom.logicalGeometry.y == 930);
}

void derivesScaleFromPhysicalDensity() {
    auto reference = monitor("1", "eDP-1");
    reference.resolution = {.width = 3840, .height = 2160};
    reference.scale = 1.5;
    reference.physicalSize = od::PhysicalSize{.widthMm = 597, .heightMm = 336};
    od::DisplayOptions options;
    options.receiverPhysicalSize = od::PhysicalSize{.widthMm = 263, .heightMm = 181};
    const auto layout = od::planDisplayLayout(reference, ipad(), options, 60);
    assert(layout.usedPhysicalSizing);
    assert(std::abs(layout.scale - 2.15) < 0.001);
    assert(std::fmod(layout.scale * 20.0, 1.0) < 0.001);
}

}  // namespace

int main() {
    selectsReferenceDeterministically();
    detectsAddedOutputAcrossKscreenRenumbering();
    defaultsToBottomRight();
    makesFractionalModesLogicallyIntegral();
    makesKdeCustomModesCvtCompatible();
    keepsHyprlandNativeModeWhenLogicalSizeIsIntegral();
    supportsEveryPlacementAxis();
    centersAboveAndBelowReference();
    derivesScaleFromPhysicalDensity();
}
