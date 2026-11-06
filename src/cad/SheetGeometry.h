#pragma once
#include "bending/cad/occt/FlatPatternAccess.h"
#include <memory>
#include <stop_token>
#include <variant>

namespace bending::cad {
// The same complete partition proof as unfolding, without flat drawing/meshing.
// Original coordinates, independent geometry owned by this immutable snapshot.
struct VerifiedSheetGeometry {
    application::DocumentPtr source;
    std::shared_ptr<const SheetLayout> layout;
};
using SheetGeometryResult = std::variant<VerifiedSheetGeometry, application::FlatPatternError>;
SheetGeometryResult prepareSheetGeometry(application::DocumentPtr, std::stop_token);
} // namespace bending::cad
