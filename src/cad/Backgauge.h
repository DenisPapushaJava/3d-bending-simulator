#pragma once
#include "CadPreview.h"

namespace bending::cad {
// Shared post-processing of worker-owned frames. A sampled X withdrawal holds
// the contact frame fixed. No continuous movement/physical holding claim.
void prepareBackgauge(CadPreview &, double toleranceMm, std::stop_token);
// Error diagnostics contain a safe prefix and its first failed position only.
void truncateCollisionPreview(CadPreview &, std::size_t failedFrame);
} // namespace bending::cad
