#pragma once
#include "bending/cad/occt/PreviewAccess.h"
#include <stop_token>
namespace bending::cad {
struct CollisionEvidence {
    std::string firstId, secondId;
    TopoDS_Shape overlap; // Already computed, confirmed forbidden volume; no new Boolean.
};
// Read existing display triangulations only. Never meshes/mutates shared bodies.
void buildCollisionHighlights(PreviewFrame &, const std::vector<CollisionEvidence> &,
                              double thicknessMm, std::stop_token);
} // namespace bending::cad
