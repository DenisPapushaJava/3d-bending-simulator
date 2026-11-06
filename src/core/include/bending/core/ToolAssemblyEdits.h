#pragma once
#include "bending/core/ToolSegments.h"
namespace bending::core {
// Edit an actual outer end, retaining untouched middle sections and physical joints.
// Empty input/output is an editor draft, not a valid installed assembly.
[[nodiscard]] std::optional<ToolAssembly> withPunchEnd(const std::vector<ToolSegment> &,
                                                       const ToolAssembly &, bool right,
                                                       std::optional<std::size_t> row);
} // namespace bending::core
