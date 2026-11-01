#pragma once
#include "bending/cad/occt/PreviewAccess.h"
#include <stop_token>

namespace bending::cad {
// A long straight boundary of a slender confirmed overlap. Never joins separate solids.
std::optional<std::array<gp_Pnt, 2>> collisionStrip(const TopoDS_Shape &, double radiusMm,
                                                    std::stop_token);
// Clip an existing triangle to the confirmed axial interval; no new surface is created.
std::vector<gp_Pnt> clipCollisionStripRange(const std::array<gp_Pnt, 3> &,
                                            const std::array<gp_Pnt, 2> &);
// True when a parallel surface triangle was handled (including a clear/empty one).
// False requests the existing point-marker fallback on the clipped source triangle.
bool appendCollisionStrip(CollisionHighlightPatch &, const std::array<gp_Pnt, 3> &,
                          std::size_t vertexLimit);
} // namespace bending::cad
