#pragma once

#include "bending/application/SingleBendPreview.h"
#include "bending/core/ToolCatalog.h"

namespace bending::ui {
using PlacementInterval = std::array<double, 2>;

// Nominal axial coverage in the machine frame, independent of the forming check.
struct BendPlacementCoverage {
    std::vector<PlacementInterval> bendMaterial;
    std::array<std::vector<PlacementInterval>, 2> workingSections; // Die, punch.

    static BendPlacementCoverage prepare(const application::SingleBendPreview &preview,
                                         const core::ToolAssignment *assignment) {
        BendPlacementCoverage result;
        const auto &tools = preview.toolSetup()->tools;
        const double width = preview.model().widthMm;
        const double origin = (tools.lengthMm - width) / 2;
        const auto material = preview.endSupportIntervals();
        const auto draftMaterial = preview.draftAxialBendIntervalsMm();
        if (!draftMaterial.empty())
            for (const auto &[from, to] : draftMaterial)
                result.bendMaterial.push_back({origin + from, origin + to});
        else if (preview.activeBendCount() == 1 && material.empty())
            result.bendMaterial.push_back({origin, origin + width});
        else if (material.size() == preview.activeBendCount())
            for (const auto &interval : material)
                result.bendMaterial.push_back(
                    {origin + interval.fromMm,
                     origin + interval.fromMm + interval.material.bendLengthMm});

        const auto fill = [&](std::vector<PlacementInterval> &sections, double start, double length,
                              const core::ToolPreset *preset,
                              const std::optional<core::ToolAssembly> &assembly, bool reversed) {
            if (!assembly) {
                sections.push_back({start, start + length});
                return;
            }
            if (!preset || !preset->segments)
                return;
            const auto parts = core::assemblyIntervals(*preset->segments, *assembly);
            if (!parts || parts->empty() || std::abs(parts->back().toMm - length) > 1e-5)
                return;
            for (const auto &part : *parts)
                sections.push_back(reversed
                                       ? PlacementInterval{start + length - part.toMm,
                                                           start + length - part.fromMm}
                                       : PlacementInterval{start + part.fromMm, start + part.toMm});
            if (reversed)
                std::reverse(sections.begin(), sections.end());
        };
        const double punch = core::punchLengthMm(tools);
        const bool currentAssignment = assignment && core::toolParameters(*assignment) == tools;
        const auto gaps = preview.toolSegmentGaps();
        if (currentAssignment || gaps[0] == 0)
            fill(result.workingSections[0], 0, tools.lengthMm,
                 currentAssignment ? &assignment->die : nullptr,
                 currentAssignment ? assignment->dieAssembly : std::nullopt, false);
        if (currentAssignment || gaps[1] == 0)
            fill(result.workingSections[1], (tools.lengthMm - punch) / 2, punch,
                 currentAssignment ? &assignment->punch : nullptr,
                 currentAssignment ? assignment->punchAssembly : std::nullopt, tools.punchReversed);
        return result;
    }

    struct AtOffset {
        std::array<double, 2> missingMm{};
        std::vector<PlacementInterval> uncovered;
    };
    [[nodiscard]] AtOffset at(double offset) const {
        AtOffset result;
        for (std::size_t i = 0; i < workingSections.size(); ++i) {
            const auto &sections = workingSections[i];
            if (sections.empty())
                continue;
            const auto gap = [&](double left, double right) {
                if (right - left > 1e-5) {
                    result.missingMm[i] += right - left;
                    result.uncovered.push_back({left, right});
                }
            };
            auto section = sections.begin();
            for (const auto &[from, to] : bendMaterial) {
                double left = from + offset;
                const double right = to + offset;
                while (section != sections.end() && (*section)[1] <= left)
                    ++section;
                while (section != sections.end() && (*section)[0] < right) {
                    gap(left, (*section)[0]);
                    left = std::max(left, (*section)[1]);
                    if (left >= right)
                        break;
                    ++section;
                }
                gap(left, right);
            }
        }
        return result;
    }
};
} // namespace bending::ui
