#pragma once

#include <BOPAlgo_PaveFiller.hxx>
#include <BRepAlgoAPI_Cut.hxx>
#include <BRepCheck_Analyzer.hxx>
#include <BRepGProp.hxx>
#include <GProp_GProps.hxx>
#include <cmath>
#include <stop_token>

namespace bending::cad {
// Validate each directed difference using the same intersection data.
template <class Check>
bool checkDirectedVolumeDifferences(const TopoDS_Shape &a, const TopoDS_Shape &b, double tolerance,
                                    std::stop_token stop, Check check) {
    if (stop.stop_requested())
        return false;
    TopTools_ListOfShape operands;
    operands.Append(a);
    operands.Append(b);
    BOPAlgo_PaveFiller filler;
    filler.SetArguments(operands);
    filler.SetNonDestructive(true);
    filler.SetFuzzyValue(tolerance);
    filler.Perform();
    if (stop.stop_requested() || filler.HasErrors())
        return false;
    for (const bool reverse : {false, true}) {
        TopTools_ListOfShape arguments, tools;
        arguments.Append(reverse ? b : a);
        tools.Append(reverse ? a : b);
        BRepAlgoAPI_Cut cut(filler);
        cut.SetArguments(arguments);
        cut.SetTools(tools);
        cut.SetNonDestructive(true);
        cut.SetFuzzyValue(tolerance);
        cut.Build();
        if (stop.stop_requested() || !cut.IsDone() || cut.HasErrors() ||
            !BRepCheck_Analyzer(cut.Shape()).IsValid())
            return false;
        GProp_GProps properties;
        BRepGProp::VolumeProperties(cut.Shape(), properties);
        const auto residual = std::abs(properties.Mass());
        if (!std::isfinite(residual) || !check(reverse, residual))
            return false;
    }
    return true;
}

inline bool sameVolumeWithin(const TopoDS_Shape &a, const TopoDS_Shape &b, double tolerance,
                             double budget, std::stop_token stop) {
    return checkDirectedVolumeDifferences(
        a, b, tolerance, stop, [budget](bool, double residual) { return !(residual > budget); });
}
} // namespace bending::cad
