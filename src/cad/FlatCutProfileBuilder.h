#pragma once

#include "bending/application/FlatCutProfile.h"
#include "bending/cad/occt/FlatPatternAccess.h"
#include <TopoDS_Face.hxx>
#include <map>

namespace bending::cad {
enum class CutJoinPolicy { LinearOnly, SplineLine };

std::shared_ptr<const application::FlatCutProfile>
buildFlatCutProfile(const TopoDS_Face &flat, const TopoDS_Shape &original,
                    const application::ClosedCutouts &cutouts, const SheetLayout &layout,
                    const std::map<std::size_t, std::size_t> &owners, const gp_Trsf &origin,
                    double thickness, double tolerance, std::stop_token stop,
                    bool midThickness = false, CutJoinPolicy joinPolicy = CutJoinPolicy::LinearOnly,
                    TopoDS_Face *verifiedStock = nullptr);
} // namespace bending::cad
