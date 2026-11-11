#pragma once
#include <TopTools_IndexedDataMapOfShapeListOfShape.hxx>
#include <TopoDS_Face.hxx>
#include <TopoDS_Shape.hxx>
#include <array>
#include <optional>
#include <stop_token>

class BRepAlgoAPI_Common;

namespace bending::cad {
enum class BendSectorSelection { MultipleComponents, PreserveSourceSkins };

// Select from an already validated intersection with history enabled.
std::optional<TopoDS_Shape> selectOwnedBendSector(BRepAlgoAPI_Common &,
                                                  const std::array<TopoDS_Face, 2> &skins,
                                                  double tolerance, std::stop_token);

// Fallback for a multi-solid envelope intersection. Both complete source skins
// must have one common owner by Boolean history. The caller still proves the
// disjoint sector/wall partition and reconstruction of the WHOLE source.
// PreserveSourceSkins retries a single-solid intersection with the source first.
std::optional<TopoDS_Shape>
ownedBendSector(const TopoDS_Shape &source, const TopoDS_Shape &envelope,
                const std::array<TopoDS_Face, 2> &skins, double tolerance, std::stop_token,
                BendSectorSelection selection = BendSectorSelection::MultipleComponents);

// Builds a sector from the two original cylindrical faces, clipped original
// caps and planar interfaces. The caller must still prove the whole partition.
TopoDS_Shape sectorFromSourceCaps(const TopTools_IndexedDataMapOfShapeListOfShape &adjacent,
                                  const TopoDS_Shape &envelope,
                                  const std::array<TopoDS_Face, 2> &skins, double tolerance,
                                  std::stop_token stop);
} // namespace bending::cad
