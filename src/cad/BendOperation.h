#pragma once
#include "bending/application/SingleBendPreview.h"

namespace bending::cad {
// Shared CAD transition for an isolated operation and a route's intermediate state.
application::BendPreviewResult
buildBendOperation(application::DocumentPtr, std::size_t, core::BendTarget, bool fixedSecond,
                   std::stop_token, std::optional<core::PressTools>, bool withMotion = false,
                   std::optional<core::ToolAssignment> = {}, bool allowPartialEndSupport = false);
} // namespace bending::cad
