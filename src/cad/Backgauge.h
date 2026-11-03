#pragma once
#include "CadPreview.h"

namespace bending::cad {
class CadDocument;
// Proven invariant only for parallel bend axes. Unknown geometries return null.
std::optional<double> backgaugeInvariantSpan(const CadDocument &, std::stop_token);
std::optional<std::string> backgaugeContactIssue(const PreviewFrame &, const core::BackgaugeSetup &,
                                                 double thicknessMm, double toolCentreZmm,
                                                 double withdrawalMm, bool filledCutouts,
                                                 double toleranceMm, std::stop_token);
// Shared post-processing of worker-owned frames. A sampled X withdrawal holds
// the contact frame fixed. No continuous movement/physical holding claim.
void prepareBackgauge(CadPreview &, double toleranceMm, std::stop_token);
// Error diagnostics contain a safe prefix and its first failed position only.
void truncateCollisionPreview(CadPreview &, std::size_t failedFrame);
} // namespace bending::cad
