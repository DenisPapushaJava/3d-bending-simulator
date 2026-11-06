#include "BendSectorOwnership.h"
#include <BRepAdaptor_Surface.hxx>
#include <BRepAlgoAPI_Common.hxx>
#include <BRepCheck_Analyzer.hxx>
#include <BRepGProp.hxx>
#include <GProp_GProps.hxx>
#include <TopExp.hxx>
#include <TopTools_IndexedMapOfShape.hxx>
#include <TopTools_ListOfShape.hxx>
#include <algorithm>
#include <cmath>
#include <vector>

namespace bending::cad {
namespace {
double area(const TopoDS_Shape &shape) {
    GProp_GProps properties;
    BRepGProp::SurfaceProperties(shape, properties);
    return properties.Mass();
}
} // namespace
std::optional<TopoDS_Shape> ownedBendSector(const TopoDS_Shape &source,
                                            const TopoDS_Shape &envelope,
                                            const std::array<TopoDS_Face, 2> &skins,
                                            double tolerance, std::stop_token stop) {
    if (stop.stop_requested() || source.IsNull() || envelope.IsNull() || skins[0].IsNull() ||
        skins[1].IsNull() || skins[0].IsSame(skins[1]) || !std::isfinite(tolerance) ||
        tolerance <= 0)
        return {};
    for (const auto &skin : skins)
        if (BRepAdaptor_Surface(skin).GetType() != GeomAbs_Cylinder)
            return {};
    BRepAlgoAPI_Common common;
    TopTools_ListOfShape arguments, tools;
    arguments.Append(envelope);
    tools.Append(source);
    common.SetArguments(arguments);
    common.SetTools(tools);
    common.SetNonDestructive(true);
    common.SetRunParallel(false);
    common.SetToFillHistory(true);
    common.SetFuzzyValue(tolerance);
    common.Build();
    if (stop.stop_requested() || !common.IsDone() || common.HasErrors() ||
        !BRepCheck_Analyzer(common.Shape()).IsValid())
        return {};
    TopTools_IndexedMapOfShape solids, faces;
    TopExp::MapShapes(common.Shape(), TopAbs_SOLID, solids);
    if (solids.Extent() < 2)
        return {};
    std::vector<int> owners{0};
    for (int i = 1; i <= solids.Extent(); ++i) {
        TopTools_IndexedMapOfShape local;
        TopExp::MapShapes(solids(i), TopAbs_FACE, local);
        for (int j = 1; j <= local.Extent(); ++j) {
            const int index = faces.Add(local(j));
            if (index == static_cast<int>(owners.size()))
                owners.push_back(i);
            else if (owners[static_cast<std::size_t>(index)] != i)
                owners[static_cast<std::size_t>(index)] = 0;
        }
    }
    int selected = 0;
    for (const auto &skin : skins) {
        if (stop.stop_requested())
            return {};
        TopTools_IndexedMapOfShape descendants;
        for (TopTools_ListIteratorOfListOfShape it(common.Modified(skin)); it.More(); it.Next()) {
            if (it.Value().ShapeType() != TopAbs_FACE)
                return {};
            descendants.Add(it.Value());
        }
        if (descendants.IsEmpty() && !common.IsDeleted(skin))
            descendants.Add(skin);
        if (descendants.IsEmpty())
            return {};
        double covered = 0;
        for (int i = 1; i <= descendants.Extent(); ++i) {
            const int index = faces.FindIndex(descendants(i));
            if (!index || !owners[static_cast<std::size_t>(index)])
                return {};
            const int owner = owners[static_cast<std::size_t>(index)];
            if (selected && selected != owner)
                return {};
            selected = owner;
            covered += area(descendants(i));
        }
        GProp_GProps perimeter;
        BRepGProp::LinearProperties(skin, perimeter);
        const double expected = area(skin);
        const double budget = std::max(expected * 1e-7, perimeter.Mass() * tolerance * 2);
        if (!std::isfinite(expected) || expected <= 0 || !std::isfinite(covered) ||
            !std::isfinite(budget) || std::abs(expected - covered) > budget)
            return {};
    }
    if (!selected)
        return {};
    return solids(selected);
}
} // namespace bending::cad
