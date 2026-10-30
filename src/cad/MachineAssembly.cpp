#include "bending/cad/occt/MachineAssembly.h"
#include "bending/core/ToolGeometry.h"
#include <BRepAlgoAPI_Common.hxx>
#include <BRepBndLib.hxx>
#include <BRepBuilderAPI_MakeFace.hxx>
#include <BRepBuilderAPI_MakePolygon.hxx>
#include <BRepCheck_Analyzer.hxx>
#include <BRepGProp.hxx>
#include <BRepMesh_IncrementalMesh.hxx>
#include <BRepPrimAPI_MakePrism.hxx>
#include <Bnd_Box.hxx>
#include <GProp_GProps.hxx>
#include <TopTools_ListOfShape.hxx>
#include <algorithm>
#include <cmath>
#include <stdexcept>

namespace bending::cad {
namespace {
void checkStop(std::stop_token stop) {
    if (stop.stop_requested())
        throw std::runtime_error("Machine check cancelled");
}
Bnd_Box bounds(const TopoDS_Shape &s) {
    if (s.IsNull())
        throw std::runtime_error("Missing body in machine check");
    Bnd_Box b;
    BRepBndLib::AddOptimal(s, b, false, false);
    if (b.IsVoid() || b.IsOpen())
        throw std::runtime_error("Invalid machine bounds");
    return b;
}
bool intersects(const TopoDS_Shape &a, const TopoDS_Shape &b, double tolerance,
                std::stop_token stop) {
    checkStop(stop);
    auto first = bounds(a), second = bounds(b);
    first.Enlarge(tolerance);
    second.Enlarge(tolerance);
    if (first.IsOut(second))
        return false;
    BRepAlgoAPI_Common common;
    TopTools_ListOfShape arguments, tools;
    arguments.Append(a);
    tools.Append(b);
    common.SetArguments(arguments);
    common.SetTools(tools);
    common.SetNonDestructive(true);
    common.SetRunParallel(false);
    common.SetFuzzyValue(tolerance);
    common.Build();
    checkStop(stop);
    if (!common.IsDone() || common.HasErrors() || !BRepCheck_Analyzer(common.Shape()).IsValid())
        throw std::runtime_error("Machine collision Boolean failed; position is not verified");
    GProp_GProps p;
    BRepGProp::VolumeProperties(common.Shape(), p);
    if (!std::isfinite(p.Mass()))
        throw std::runtime_error("Invalid machine intersection volume");
    // Numerical volume allowance only; never use the forming-zone volume budget.
    return std::abs(p.Mass()) > std::max(1.e-5, 8 * tolerance * tolerance * tolerance);
}
TopoDS_Shape prism(const std::vector<std::array<double, 2>> &points, double from, double to) {
    BRepBuilderAPI_MakePolygon polygon;
    for (const auto &p : points)
        polygon.Add(gp_Pnt(p[0], p[1], from));
    polygon.Close();
    auto shape = BRepPrimAPI_MakePrism(BRepBuilderAPI_MakeFace(polygon.Wire()).Face(),
                                       gp_Vec(0, 0, to - from))
                     .Shape();
    if (!BRepCheck_Analyzer(shape).IsValid())
        throw std::runtime_error("Invalid demonstration machine component");
    BRepMesh_IncrementalMesh mesh(shape, .1, false, .35, false);
    if (!mesh.IsDone())
        throw std::runtime_error("Machine component meshing failed");
    return shape;
}
} // namespace
MachineAssembly::MachineAssembly(const core::PressTools &tools,
                                 const std::array<TopoDS_Shape, 2> &bodies, std::stop_token stop,
                                 const std::optional<core::ToolAssignment> &assignment) {
    if (tools.machineModel == core::MachineModel::None)
        return;
    if (tools.machineModel != core::MachineModel::DemoPressV1 &&
        tools.machineModel != core::MachineModel::DemoPressV2)
        throw std::runtime_error("Unknown machine profile revision");
    checkStop(stop);
    const double centre = tools.lengthMm / 2;
    // Die top is the scene datum. Seat on the actual bodies, so different die
    // heights and punch profiles retain the same stack without bounding-box centring.
    const double dieSeat = bounds(bodies[0]).CornerMin().Y();
    const double punchSeat = bounds(bodies[1]).CornerMax().Y();
    const auto mount = tools.machineModel == core::MachineModel::DemoPressV2 && assignment
                           ? core::punchMountReference(*assignment)
                           : std::nullopt;
    seated_ = mount.has_value();
    const double shoulder = mount ? punchSeat - mount->heightMm : punchSeat;
    const double cx = mount ? mount->centreX : 0;
    const bool pocketDie = tools.machineModel == core::MachineModel::DemoPressV2 && assignment &&
                           assignment->useDrawingGeometry && assignment->die.drawing;
    // A known die bottom seats on the pocket floor. An idealized working wedge
    // has no known mounting foot: rest it on the lips without inventing a shank.
    const double adapterTop = dieSeat + (pocketDie ? 10 : 0);
    const double tableTop = adapterTop - 30, floor = tableTop - 900;
    const auto add = [&](std::string id, std::string name,
                         std::vector<std::array<double, 2>> points, double from, double to,
                         bool moving = false) {
        checkStop(stop);
        components_.push_back(
            {std::move(id), std::move(name), prism(points, centre + from, centre + to), moving});
    };
    add("table", "Стол", {{-45, floor}, {45, floor}, {45, tableTop}, {-45, tableTop}}, -1550, 1550);
    // Retain V1's flat adapter. V2 uses the measured nominal pocket below;
    // its rails are checked against the full actual die, without pair exemptions.
    add("die-adapter", "Адаптер матрицы",
        {{-40, tableTop},
         {40, tableTop},
         {40, dieSeat - 1},
         {39, dieSeat},
         {-39, dieSeat},
         {-40, dieSeat - 1}},
        -1550, 1550);
    components_.back().allowDieContact = tools.machineModel == core::MachineModel::DemoPressV1;
    if (tools.machineModel == core::MachineModel::DemoPressV2) {
        // 9 mm is the straight inner wall; .8/.2 chamfers make 10 mm total depth.
        const double bottom = tableTop, top = adapterTop, pocket = top - 10;
        components_.back().shape = prism({{-39, bottom},
                                          {39, bottom},
                                          {40, bottom + 1},
                                          {40, top - .8},
                                          {39.2, top},
                                          {31.05, top},
                                          {30.25, top - .8},
                                          {30.25, pocket + .2},
                                          {30.05, pocket},
                                          {-30.05, pocket},
                                          {-30.25, pocket + .2},
                                          {-30.25, top - .8},
                                          {-31.05, top},
                                          {-39.2, top},
                                          {-40, top - .8},
                                          {-40, bottom + 1}},
                                         centre - 1550, centre + 1550);
    }
    const double holderTop = shoulder + 120;
    add("ram", "Верхняя траверса",
        {{-27.5, holderTop},
         {27.5, holderTop},
         {27.5, holderTop + 1190},
         {-27.5, holderTop + 1190}},
        -1550, 1550, true);
    for (int i = -3; i <= 3; ++i) {
        const double z = i * 400.;
        std::vector<std::array<double, 2>> profile{{-22, punchSeat},      {22, punchSeat},
                                                   {22, punchSeat + 40},  {40, punchSeat + 40},
                                                   {40, punchSeat + 100}, {18, punchSeat + 100},
                                                   {18, holderTop},       {-18, holderTop},
                                                   {-18, punchSeat + 70}, {-22, punchSeat + 60}};
        if (tools.machineModel == core::MachineModel::DemoPressV2) {
            profile = {{cx - 22, shoulder},
                       {cx + 40, shoulder},
                       {cx + 40, shoulder + 80},
                       {cx + 18, holderTop},
                       {cx - 18, holderTop}};
            if (mount) {
                const double left = mount->minX - .15, right = mount->maxX + .15;
                profile = {{cx - 22, shoulder},      {left, shoulder},     {left, punchSeat + .15},
                           {right, punchSeat + .15}, {right, shoulder},    {cx + 40, shoulder},
                           {cx + 40, shoulder + 80}, {cx + 18, holderTop}, {cx - 18, holderTop}};
            }
        }
        add("holder-" + std::to_string(i + 4), "Держатель пуансона " + std::to_string(i + 4),
            profile, z - 50, z + 50, true);
        components_.back().allowPunchContact =
            tools.machineModel == core::MachineModel::DemoPressV1;
    }
    const std::vector<std::array<double, 2>> frame{
        {80, floor},           {1450, floor},      {1450, floor + 1950},
        {1300, floor + 2100},  {80, floor + 2100}, {80, tableTop + 550},
        {410, tableTop + 550}, {410, tableTop},    {80, tableTop}};
    add("frame-left", "Левая боковая рама", frame, -1450, -1300);
    add("frame-right", "Правая боковая рама", frame, 1300, 1450);
}
std::vector<MachineDisplayComponent> MachineAssembly::at(double lift) const {
    auto result = components_;
    gp_Trsf translation;
    translation.SetTranslation(gp_Vec(0, lift, 0));
    for (auto &c : result)
        if (c.moving)
            c.shape = c.shape.Moved(TopLoc_Location(translation));
    return result;
}
std::optional<application::MachineCollision>
MachineAssembly::check(const PreviewFrame &frame, double tolerance, std::stop_token stop) {
    for (const auto &c : frame.machine) {
        if (intersects(frame.shape, c.shape, tolerance, stop))
            return application::MachineCollision{"sheet", "Деталь", c.id, c.name};
        for (std::size_t t = 0; t < 2; ++t) {
            // Only named mating pairs. The sheet has no corresponding exemption.
            if ((t == 0 && c.allowDieContact) || (t == 1 && c.allowPunchContact))
                continue;
            if (intersects(frame.tools[t], c.shape, tolerance, stop))
                return application::MachineCollision{t == 0 ? "die" : "punch",
                                                     t == 0 ? "Матрица" : "Пуансон", c.id, c.name};
        }
    }
    for (const auto &upper : frame.machine)
        if (upper.moving)
            for (const auto &lower : frame.machine)
                if (!lower.moving && intersects(upper.shape, lower.shape, tolerance, stop))
                    return application::MachineCollision{upper.id, upper.name, lower.id,
                                                         lower.name};
    return {};
}
} // namespace bending::cad
