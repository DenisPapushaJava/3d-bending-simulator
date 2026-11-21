#pragma once

#include "bending/core/ToolGeometry.h"

namespace bending::core {
[[nodiscard]] std::optional<ToolGeometry> workingPunchGeometry(const ToolPreset &);
// The caller validates the die and supplies its drawing in the selected groove orientation.
[[nodiscard]] std::optional<ToolGeometry> workingDieGeometry(const ToolPreset &, std::size_t groove,
                                                             const ToolDrawing &rotatedDrawing);
} // namespace bending::core
