#pragma once
#include "BendFixtures.h"
#include "ToolFixtures.h"
#include <BRepPrimAPI_MakeBox.hxx>
#include <STEPControl_Writer.hxx>
#include <filesystem>
#include <stdexcept>
namespace end_support_fixture {
inline TopoDS_Shape joint(double firstStart = .46, double firstEnd = 3, double secondStart = 1.25,
                          double secondEnd = 2.5) {
    auto shape = bending::fixtures::jointBend();
    for (const auto &[from, to] :
         std::array<std::pair<double, double>, 4>{{{-1, firstStart},
                                                   {45 - firstEnd, 46},
                                                   {64, 65 + secondStart},
                                                   {120 - secondEnd, 121}}})
        shape = BRepAlgoAPI_Cut(shape,
                                BRepPrimAPI_MakeBox(gp_Pnt(-31, 3, from), 28, 4, to - from).Shape())
                    .Shape();
    return shape;
}
inline void write(const std::filesystem::path &path, double endLoss, bool middle = false,
                  double otherEndLoss = 0) {
    auto shape = bending::fixtures::bentStrip(4, 4, std::numbers::pi / 2, 50, true, 27);
    const auto notch = [&](double from, double length) {
        BRepAlgoAPI_Cut cut(shape, BRepPrimAPI_MakeBox(gp_Pnt(-28, 3, from), 12, 6, length));
        if (!cut.IsDone() || cut.HasErrors())
            throw std::runtime_error("End support fixture cut failed");
        shape = cut.Shape();
    };
    if (endLoss > 0)
        notch(middle ? 24 : 50 - endLoss, middle ? endLoss : endLoss + 1);
    if (otherEndLoss > 0)
        notch(-1, otherEndLoss + 1);
    STEPControl_Writer writer;
    const auto utf8 = path.u8string();
    if (writer.Transfer(shape, STEPControl_AsIs) != IFSelect_RetDone ||
        writer.Write(reinterpret_cast<const char *>(utf8.c_str())) != IFSelect_RetDone)
        throw std::runtime_error("End support fixture write failed");
}
inline bending::core::ToolAssignment tools() {
    return tool_fixture::explicitPair(
        bending::core::PressTools{35, std::numbers::pi / 3, std::numbers::pi / 6, .8, 80, 10});
}
} // namespace end_support_fixture
