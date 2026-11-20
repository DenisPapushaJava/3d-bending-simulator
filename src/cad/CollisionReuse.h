#pragma once

#include <BRepBndLib.hxx>
#include <BRep_Tool.hxx>
#include <Bnd_Box.hxx>
#include <Precision.hxx>
#include <Standard_Failure.hxx>
#include <TopLoc_Datum3D.hxx>
#include <TopoDS_Shape.hxx>
#include <algorithm>
#include <array>
#include <cmath>
#include <optional>
#include <vector>

namespace bending::cad {
// Exact geometry bounds enlarged by native tolerances and the consuming query's tolerance.
inline std::optional<Bnd_Box> conservativeGeometryBounds(const TopoDS_Shape &shape,
                                                        double tolerance) {
    if (shape.IsNull() || !std::isfinite(tolerance) || tolerance < 0)
        return {};
    try {
        Bnd_Box box;
        BRepBndLib::AddOptimal(shape, box, false, false);
        if (box.IsVoid() || box.IsOpen())
            return {};
        // AddOptimal alone does not enclose every loose vertex tolerance on OCCT 7.6.
        double native = 0;
        for (const auto kind : {TopAbs_FACE, TopAbs_EDGE, TopAbs_VERTEX}) {
            const double value = BRep_Tool::MaxTolerance(shape, kind);
            if (!std::isfinite(value) || value < 0)
                return {};
            native = std::max(native, value);
        }
        const double margin = tolerance + Precision::Confusion() + native;
        if (!std::isfinite(margin))
            return {};
        box.Enlarge(margin);
        return box;
    } catch (const Standard_Failure &) {
        return {};
    }
}

// An overlapping or unknown box leaves the exact collision query in place.
inline bool boundedDisjoint(const TopoDS_Shape &a, const TopoDS_Shape &b, double tolerance) {
    const auto first = conservativeGeometryBounds(a, tolerance);
    if (!first)
        return false;
    const auto second = conservativeGeometryBounds(b, tolerance);
    return second && first->IsOut(*second);
}

// Per-operation memo of already checked clear rigid pairs. No collision is cached:
// the current frame still collects every overlap for its local diagnostic.
class RigidClearanceCache {
    struct Entry {
        std::array<TopoDS_Shape, 2> shapes;
        double tolerance;
    };
    std::vector<Entry> entries_;
    std::size_t hits_ = 0;
    static bool sameIsometry(const gp_Trsf &a, const gp_Trsf &b) {
        if (a.IsNegative() || a.ScaleFactor() != 1 || b.IsNegative() || b.ScaleFactor() != 1)
            return false;
        for (int row = 1; row <= 3; ++row)
            for (int column = 1; column <= 4; ++column)
                if (a.Value(row, column) != b.Value(row, column))
                    return false;
        return true;
    }
    static std::array<TopoDS_Shape, 2> canonical(const TopoDS_Shape &a, const TopoDS_Shape &b) {
        auto first = a.Location(), second = b.Location();
        // Strip only an exactly equal common leading rigid placement. Never round a relative
        // position or infer an attachment from IDs/moving flags. Relative movement is a miss.
        while (!first.IsIdentity() && !second.IsIdentity() && first.FirstPower() == 1 &&
               second.FirstPower() == 1 &&
               sameIsometry(first.FirstDatum()->Transformation(),
                            second.FirstDatum()->Transformation())) {
            first = first.NextLocation();
            second = second.NextLocation();
        }
        return {a.Located(first), b.Located(second)};
    }

  public:
    bool contains(const TopoDS_Shape &a, const TopoDS_Shape &b, double tolerance) {
        const auto key = canonical(a, b);
        const bool found = std::any_of(entries_.begin(), entries_.end(), [&](const auto &e) {
            return e.tolerance == tolerance && e.shapes[0].IsEqual(key[0]) &&
                   e.shapes[1].IsEqual(key[1]);
        });
        hits_ += found;
        return found;
    }
    void remember(const TopoDS_Shape &a, const TopoDS_Shape &b, double tolerance) {
        if (entries_.size() >= 128)
            entries_.erase(entries_.begin());
        entries_.push_back({canonical(a, b), tolerance});
    }
    [[nodiscard]] std::size_t hits() const {
        return hits_;
    }
};
} // namespace bending::cad
