#pragma once

#include "bending/core/PressTools.h"
#include "bending/core/ToolSegments.h"

#include <optional>
#include <string>
#include <vector>

namespace bending::core {
enum class ToolKind { Punch, Die };
struct DrawingPoint {
    double x = 0;
    double y = 0;
    bool operator==(const DrawingPoint &) const = default;
};
struct DrawingEdge {
    DrawingPoint start;
    DrawingPoint end;
    std::optional<DrawingPoint> center; // Null: line. Otherwise circular arc.
    double sweepRadians = 0;            // Signed mathematical XY sweep.
    bool operator==(const DrawingEdge &) const = default;
};
// Source contour; simulation use requires an explicit, validated assignment mode.
struct ToolDrawing {
    std::string sourceFile;
    std::string sha256;
    bool millimeters = false;
    std::vector<DrawingEdge> edges; // One ordered closed contour, no annotation/axes.
    bool operator==(const ToolDrawing &) const = default;
};
[[nodiscard]] bool validToolDrawing(const ToolDrawing &drawing);
struct DieGroove {
    double angleRadians = 0;
    double rotationRadians = 0; // Counterclockwise rotation of the captured section.
    bool operator==(const DieGroove &) const = default;
};
// Catalogue facts; the assignment selects idealized or supported drawing geometry.
struct ToolPreset {
    std::string id;
    std::string manufacturer;
    std::string code;
    ToolKind kind = ToolKind::Punch;
    double angleRadians = 0;
    double lengthMm = 0;
    std::optional<double> heightMm;
    std::optional<double> noseRadiusMm;
    std::vector<double> openingsMm;
    std::string source;
    std::string notes;
    bool teaching = false;
    bool parametersConfirmed = true; // False: inventory entry, unavailable for assignment/search.
    std::optional<ToolDrawing> drawing = std::nullopt;
    std::vector<DieGroove> grooveSettings = {}; // Empty: legacy common angle, upright section.
    std::optional<std::vector<ToolSegment>> segments = std::nullopt; // Null: legacy single tool.
    bool operator==(const ToolPreset &) const = default;
};
using ToolCatalog = std::vector<ToolPreset>;
struct ToolAssignment {
    ToolPreset punch;
    ToolPreset die;
    std::size_t groove = 0;
    double clearanceMm = 10;
    std::string machineName;
    bool useDrawingGeometry = false; // Use each known drawing; preserve saved legacy choice.
    std::optional<ToolAssembly> punchAssembly = std::nullopt;
    std::optional<ToolAssembly> dieAssembly = std::nullopt;
    MachineModel machineModel = MachineModel::None;
    bool punchReversed = false;
    bool operator==(const ToolAssignment &) const = default;
};
[[nodiscard]] bool validToolPreset(const ToolPreset &tool);
// Workshop entries require a captured millimetre DXF contour. Legacy project data is separate.
[[nodiscard]] bool hasLibraryProfile(const ToolPreset &tool);
[[nodiscard]] bool validToolCatalog(const ToolCatalog &catalog);
[[nodiscard]] std::optional<DieGroove> dieGroove(const ToolPreset &, std::size_t);
[[nodiscard]] ToolDrawing rotateToolDrawing(const ToolDrawing &, double radians);
[[nodiscard]] std::optional<ToolDrawing> activeDieDrawing(const ToolPreset &, std::size_t);
[[nodiscard]] std::optional<PressTools> toolParameters(const ToolAssignment &assignment);
[[nodiscard]] std::optional<double> toolLength(const ToolPreset &,
                                               const std::optional<ToolAssembly> &);
} // namespace bending::core
