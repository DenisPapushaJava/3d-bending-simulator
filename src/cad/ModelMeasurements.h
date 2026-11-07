#pragma once
#include "bending/application/ModelMeasurements.h"
#include <TopoDS_Shape.hxx>
#include <gp_Trsf.hxx>
#include <map>
#include <memory>
#include <span>
#include <stop_token>

namespace bending::cad {
struct SheetWallRegion;
[[nodiscard]] std::shared_ptr<const application::ModelMeasurements>
flatModelMeasurements(const application::ModelMeasurements &, const TopoDS_Shape &source,
                      const TopoDS_Shape &calculation, const TopoDS_Shape &stock,
                      std::span<const SheetWallRegion>, const gp_Trsf &origin,
                      bool differentTopology, double tolerance, std::stop_token,
                      std::map<std::size_t, std::size_t> &sourceWallOwners);

[[nodiscard]] std::shared_ptr<const application::ModelMeasurements>
prepareModelMeasurements(const TopoDS_Shape &, const application::DocumentMetadata &,
                         std::stop_token,
                         std::span<const application::FaceInfo> preparedFaces = {});
// Rigid unfolding of a complete planar feature; no inference from a polygonal display skin.
[[nodiscard]] application::MeasuredOpening transformOpening(const application::MeasuredOpening &,
                                                            const gp_Trsf &);
} // namespace bending::cad
