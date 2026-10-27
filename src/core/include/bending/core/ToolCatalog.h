#pragma once

#include "bending/core/PressTools.h"

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
// Source drawing for inspection only; never silently substituted for the simulation body.
struct ToolDrawing {
    std::string sourceFile;
    std::string sha256;
    bool millimeters = false;
    std::vector<DrawingEdge> edges; // One ordered closed contour, no annotation/axes.
    bool operator==(const ToolDrawing &) const = default;
};
[[nodiscard]] bool validToolDrawing(const ToolDrawing &drawing);
// Catalogue facts only. Simulation bodies remain idealized working profiles.
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
    bool operator==(const ToolPreset &) const = default;
};
using ToolCatalog = std::vector<ToolPreset>;
struct ToolAssignment {
    ToolPreset punch;
    ToolPreset die;
    std::size_t groove = 0;
    double clearanceMm = 10;
    std::string machineName;
    bool operator==(const ToolAssignment &) const = default;
};
[[nodiscard]] bool validToolPreset(const ToolPreset &tool);
[[nodiscard]] bool validToolCatalog(const ToolCatalog &catalog);
[[nodiscard]] std::optional<PressTools> toolParameters(const ToolAssignment &assignment);
} // namespace bending::core
