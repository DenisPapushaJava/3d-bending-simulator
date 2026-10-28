#pragma once
#include "bending/core/ToolCatalog.h"
#include <stop_token>
namespace bending::cad {
// Uses the same analytic section construction as simulation; no SDK types escape.
[[nodiscard]] std::optional<std::string> validateToolProfile(const core::ToolDrawing &,
                                                             std::stop_token);
} // namespace bending::cad
