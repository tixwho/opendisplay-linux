#include "opendisplay/display_layout.hpp"

#include <algorithm>
#include <cmath>
#include <iterator>
#include <numeric>
#include <sstream>
#include <stdexcept>
#include <utility>

namespace od {
namespace {

constexpr double millimetresPerInch = 25.4;

int nearestMultiple(const int value, const int multiple) {
    const int lower = std::max(multiple, (value / multiple) * multiple);
    const int upper = lower + multiple;
    return value - lower <= upper - value ? lower : upper;
}

Size compatibleResolution(const Size requested, const double scale,
                          const DisplayModePolicy policy) {
    // Plasma exposes scale in 0.05 increments. Expressing the scale as a
    // reduced p/20 fraction lets us choose pixel dimensions whose logical
    // dimensions (pixels / scale) are exact integers. KWin generates custom
    // modelines with CVT, which quantizes horizontal active pixels to groups
    // of eight. The vertical dimension only needs to remain even for H.264.
    const int twentieths = std::max(1, static_cast<int>(std::lround(scale * 20.0)));
    const int divisor = std::gcd(twentieths, 20);
    const int numerator = twentieths / divisor;
    const int horizontalMultiple = policy == DisplayModePolicy::KdeCvt ? 8 : 2;
    return {
        .width = nearestMultiple(std::max(horizontalMultiple, requested.width),
                                 std::lcm(numerator, horizontalMultiple)),
        .height = nearestMultiple(std::max(2, requested.height), std::lcm(numerator, 2)),
    };
}

PhysicalSize orientedPhysicalSize(PhysicalSize size, const Size pixels) {
    const bool pixelsLandscape = pixels.width >= pixels.height;
    const bool physicalLandscape = size.widthMm >= size.heightMm;
    if (pixelsLandscape != physicalLandscape) {
        std::swap(size.widthMm, size.heightMm);
    }
    return size;
}

double diagonalPpi(const Size pixels, PhysicalSize physical) {
    physical = orientedPhysicalSize(physical, pixels);
    const double pixelDiagonal = std::hypot(static_cast<double>(pixels.width),
                                            static_cast<double>(pixels.height));
    const double millimetreDiagonal = std::hypot(physical.widthMm, physical.heightMm);
    if (pixelDiagonal <= 0 || millimetreDiagonal <= 0) {
        throw std::runtime_error("physical monitor sizes must be positive");
    }
    return pixelDiagonal / (millimetreDiagonal / millimetresPerInch);
}

double quantizedScale(const double scale) {
    return std::clamp(std::round(scale * 20.0) / 20.0, 0.5, 4.0);
}

std::string outputNames(const std::vector<DisplayOutput>& outputs) {
    std::ostringstream result;
    bool first = true;
    for (const auto& output : outputs) {
        if (!output.connected || !output.enabled) continue;
        if (!first) result << ", ";
        result << output.name << " (" << output.id << ')';
        first = false;
    }
    return result.str();
}

void validateAlignment(const ExtendDirection extendTo, const AlignDirection alignTo) {
    const bool horizontalExtension = extendTo == ExtendDirection::Left
        || extendTo == ExtendDirection::Right;
    const bool verticalAlignment = alignTo == AlignDirection::Top
        || alignTo == AlignDirection::Bottom || alignTo == AlignDirection::Center;
    if (alignTo != AlignDirection::Center && horizontalExtension != verticalAlignment) {
        throw std::runtime_error(
            "--align-to must be top, bottom, or center for left/right extension, and "
            "left, right, or center for top/bottom extension");
    }
}

}  // namespace

bool sameDisplayOutput(const DisplayOutput& left, const DisplayOutput& right) {
    if (!left.name.empty() && !right.name.empty()) {
        return left.name == right.name;
    }
    return !left.id.empty() && left.id == right.id;
}

std::vector<DisplayOutput> addedDisplayOutputs(const std::vector<DisplayOutput>& before,
                                               const std::vector<DisplayOutput>& after) {
    std::vector<DisplayOutput> added;
    std::copy_if(after.begin(), after.end(), std::back_inserter(added),
                 [&](const auto& candidate) {
                     return candidate.connected
                         && std::none_of(before.begin(), before.end(), [&](const auto& old) {
                                return sameDisplayOutput(candidate, old);
                            });
                 });
    return added;
}

DisplayOutput selectReferenceOutput(const std::vector<DisplayOutput>& outputs,
                                    const std::string& requested) {
    std::vector<DisplayOutput> active;
    std::copy_if(outputs.begin(), outputs.end(), std::back_inserter(active),
                 [](const auto& output) { return output.connected && output.enabled; });
    if (active.empty()) {
        throw std::runtime_error("the compositor reports no enabled monitor to use as a reference");
    }
    if (requested.empty()) {
        if (active.size() != 1) {
            throw std::runtime_error(
                "more than one monitor is enabled; choose --reference-monitor from: "
                + outputNames(active));
        }
        return active.front();
    }
    const auto match = std::find_if(active.begin(), active.end(), [&](const auto& output) {
        return output.name == requested || output.id == requested;
    });
    if (match == active.end()) {
        throw std::runtime_error("--reference-monitor '" + requested
                                 + "' is not enabled; available monitors: "
                                 + outputNames(active));
    }
    return *match;
}

DisplayLayout planDisplayLayout(const DisplayOutput& detectedReference,
                                const PhoneInfo& receiver,
                                const DisplayOptions& options,
                                const int defaultRefreshRate,
                                const DisplayModePolicy modePolicy) {
    validateAlignment(options.extendTo, options.alignTo);
    DisplayOutput reference = detectedReference;
    if (options.referenceResolution) reference.resolution = *options.referenceResolution;
    if (options.referenceScale) reference.scale = *options.referenceScale;
    if (options.referencePhysicalSize) reference.physicalSize = options.referencePhysicalSize;
    if (reference.scale <= 0) {
        throw std::runtime_error("reference monitor scale must be positive");
    }
    if (options.referenceGeometry) {
        reference.logicalGeometry = *options.referenceGeometry;
    } else if (options.referenceResolution || options.referenceScale
               || reference.logicalGeometry.width <= 0
               || reference.logicalGeometry.height <= 0) {
        reference.logicalGeometry.width = static_cast<int>(
            std::lround(static_cast<double>(reference.resolution.width) / reference.scale));
        reference.logicalGeometry.height = static_cast<int>(
            std::lround(static_cast<double>(reference.resolution.height) / reference.scale));
    }
    if (reference.logicalGeometry.width <= 0 || reference.logicalGeometry.height <= 0) {
        throw std::runtime_error("reference monitor geometry must be positive");
    }

    const Size receiverPixels{
        .width = std::max(2, receiver.pixelsWide),
        .height = std::max(2, receiver.pixelsHigh),
    };
    double scale = receiver.scale > 0 ? receiver.scale : 1.0;
    bool usedPhysicalSizing = false;
    if (reference.physicalSize && options.receiverPhysicalSize) {
        const double referencePpi = diagonalPpi(reference.resolution, *reference.physicalSize);
        const double receiverPpi = diagonalPpi(receiverPixels, *options.receiverPhysicalSize);
        scale = receiverPpi / (referencePpi / reference.scale);
        usedPhysicalSizing = true;
    }
    if (options.virtualScale) {
        scale = *options.virtualScale;
        usedPhysicalSizing = false;
    }
    scale = quantizedScale(scale);

    const Size requestedResolution = options.virtualResolution.value_or(receiverPixels);
    const Size resolution = compatibleResolution(requestedResolution, scale, modePolicy);
    const int logicalWidth = static_cast<int>(std::lround(resolution.width / scale));
    const int logicalHeight = static_cast<int>(std::lround(resolution.height / scale));
    const auto& ref = reference.logicalGeometry;
    Rect geometry{.width = logicalWidth, .height = logicalHeight};

    if (options.extendTo == ExtendDirection::Right) {
        geometry.x = ref.x + ref.width;
    } else if (options.extendTo == ExtendDirection::Left) {
        geometry.x = ref.x - geometry.width;
    } else if (options.extendTo == ExtendDirection::Bottom) {
        geometry.y = ref.y + ref.height;
    } else {
        geometry.y = ref.y - geometry.height;
    }

    if (options.extendTo == ExtendDirection::Left
        || options.extendTo == ExtendDirection::Right) {
        if (options.alignTo == AlignDirection::Bottom) {
            geometry.y = ref.y + ref.height - geometry.height;
        } else if (options.alignTo == AlignDirection::Center) {
            geometry.y = ref.y + (ref.height - geometry.height) / 2;
        } else {
            geometry.y = ref.y;
        }
    } else {
        if (options.alignTo == AlignDirection::Right) {
            geometry.x = ref.x + ref.width - geometry.width;
        } else if (options.alignTo == AlignDirection::Center) {
            geometry.x = ref.x + (ref.width - geometry.width) / 2;
        } else {
            geometry.x = ref.x;
        }
    }

    return {
        .reference = std::move(reference),
        .resolution = resolution,
        .scale = scale,
        .logicalGeometry = geometry,
        .refreshRate = options.refreshRate.value_or(defaultRefreshRate),
        .usedPhysicalSizing = usedPhysicalSizing,
        .adjustedResolution = resolution.width != requestedResolution.width
            || resolution.height != requestedResolution.height,
    };
}

}  // namespace od
