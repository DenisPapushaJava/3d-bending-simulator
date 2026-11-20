#pragma once

#include "bending/core/ToolSelection.h"

namespace bending::core {
// Candidates refer to the validated catalogue. Shape can only reorder the list.
void evaluateToolCandidateShapes(const ToolCatalog &, const ToolSelectionRequest &,
                                 std::vector<ToolCandidate> &, std::stop_token);
} // namespace bending::core
