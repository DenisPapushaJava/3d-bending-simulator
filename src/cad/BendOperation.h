#pragma once
#include "bending/application/SingleBendPreview.h"
#include <gp_Dir.hxx>
#include <span>

namespace bending::cad {
class CadDocument;
gp_Dir normalBendAxis(const CadDocument &, std::size_t);
// Shared CAD transition for an isolated operation and a route's intermediate state.
application::BendPreviewResult
buildBendOperation(application::DocumentPtr, std::size_t, core::BendTarget, bool fixedSecond,
                   std::stop_token, std::optional<core::PressTools>, bool withMotion = false,
                   std::optional<core::ToolAssignment> = {}, bool allowPartialEndSupport = false,
                   std::optional<double> maximumEndSupportLossMm = {},
                   std::optional<gp_Dir> referenceAxis = {}, bool prepareDiagnostic = true,
                   application::BackgaugeSearchStatistics *statistics = nullptr,
                   std::span<const std::size_t> jointBends = {});
} // namespace bending::cad
