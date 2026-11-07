#pragma once
#include <TopoDS_Face.hxx>
#include <TopoDS_Shape.hxx>
#include <array>
#include <optional>
#include <stop_token>

namespace bending::cad {
// Fallback for a multi-solid envelope intersection. Both complete source skins
// must have one common owner by Boolean history. The caller still proves the
// disjoint sector/wall partition and reconstruction of the WHOLE source.
std::optional<TopoDS_Shape> ownedBendSector(const TopoDS_Shape &source,
                                            const TopoDS_Shape &envelope,
                                            const std::array<TopoDS_Face, 2> &skins,
                                            double tolerance, std::stop_token);
} // namespace bending::cad
