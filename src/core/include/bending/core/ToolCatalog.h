#pragma once

#include "bending/core/PressTools.h"

#include <optional>
#include <string>
#include <vector>

namespace bending::core {
enum class ToolKind { Punch, Die };
// Catalogue facts only. All current simulation bodies remain idealized working profiles.
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
