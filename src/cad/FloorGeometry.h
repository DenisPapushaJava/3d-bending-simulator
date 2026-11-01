#pragma once
#include "bending/cad/occt/PreviewAccess.h"
#include <Bnd_Box.hxx>
#include <stop_token>
namespace bending::cad {
FloorPlane makeFloorPlane(double yMm, double centreZ);
// Null means clear. The display rectangle never limits the checked half-space.
TopoDS_Shape floorOverlap(const TopoDS_Shape &, const Bnd_Box &, double yMm, double toleranceMm,
                          std::stop_token);
} // namespace bending::cad
