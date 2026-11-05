#pragma once
#include "bending/application/Document.h"
#include <optional>
#include <span>

namespace bending::cad {
// A candidate envelope only. Direction, material reconstruction, support and the
// complete press cycle must still be checked on the current intermediate solid.
std::optional<application::RecognizedBend> jointBendEnvelope(const application::BendAnalysis &,
                                                             std::span<const std::size_t>);
} // namespace bending::cad
