#include "bending/cad/occt/MachineAssembly.h"
#include "CollisionReuse.h"
#include "FloorGeometry.h"
#include "GeometryCacheInternal.h"
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
bool intersects(const TopoDS_Shape &a, const TopoDS_Shape &b, const Bnd_Box &first,
                const Bnd_Box &second, double tolerance, std::stop_token stop,
                TopoDS_Shape *overlap) {
    checkStop(stop);
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
    const bool hit = std::abs(p.Mass()) > std::max(1.e-5, 8 * tolerance * tolerance * tolerance);
    if (hit && overlap)
        *overlap = common.Shape();
    return hit;
}
TopoDS_Shape prism(const std::vector<std::array<double, 2>> &points, double from, double to) {
    const auto origin = points.front();
    auto key = geometryKey("machine-prism");
    key << to - from << ' ' << points.size();
    for (const auto &p : points)
        key << ' ' << p[0] - origin[0] << ' ' << p[1] - origin[1];
    auto shape = cachedToolShape(
        key.str(),
        [&] {
            BRepBuilderAPI_MakePolygon polygon;
            for (const auto &p : points)
                polygon.Add(gp_Pnt(p[0] - origin[0], p[1] - origin[1], 0));
            polygon.Close();
            return BRepPrimAPI_MakePrism(BRepBuilderAPI_MakeFace(polygon.Wire()).Face(),
                                         gp_Vec(0, 0, to - from))
                .Shape();
        },
        .1);
    gp_Trsf translation;
    translation.SetTranslation(gp_Vec(origin[0], origin[1], from));
    return shape.Moved(TopLoc_Location(translation));
}
} // namespace
MachineAssembly::MachineAssembly(const core::PressTools &tools,
                                 const std::array<TopoDS_Shape, 2> &bodies, std::stop_token stop,
                                 const std::optional<core::ToolAssignment> &assignment) {
    if (tools.machineModel == core::MachineModel::None)
        return;
    if (tools.machineModel != core::MachineModel::DemoPressV1 &&
        tools.machineModel != core::MachineModel::DemoPressV2 &&
        tools.machineModel != core::MachineModel::DemoPressV3)
        throw std::runtime_error("Unknown machine profile revision");
    checkStop(stop);
    const double centre = tools.lengthMm / 2;
    // Die top is the scene datum. Seat on the actual bodies, so different die
    // heights and punch profiles retain the same stack without bounding-box centring.
    const double dieSeat = bounds(bodies[0]).CornerMin().Y();
    const double punchSeat = bounds(bodies[1]).CornerMax().Y();
    const bool modern = tools.machineModel != core::MachineModel::DemoPressV1;
    const auto mount = modern && assignment ? core::punchMountReference(*assignment) : std::nullopt;
    seated_ = mount.has_value();
    const double shoulder = mount ? punchSeat - mount->heightMm : punchSeat;
    const double cx = mount ? mount->centreX : 0;
    const bool pocketDie =
        modern && assignment && assignment->useDrawingGeometry && assignment->die.drawing;
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
    if (modern) {
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
        if (modern) {
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
    if (tools.machineModel == core::MachineModel::DemoPressV3)
        floor_ = makeFloorPlane(floor, centre);
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
namespace {
std::vector<MachineCollisionEvidence> collectCollisions(const PreviewFrame &frame, double tolerance,
                                                        std::stop_token stop, bool firstOnly,
                                                        RigidClearanceCache *cache = nullptr) {
    std::vector<MachineCollisionEvidence> result;
    std::optional<Bnd_Box> sheetBounds;
    if (frame.floor) {
        sheetBounds = bounds(frame.shape);
        const auto overlap =
            floorOverlap(frame.shape, *sheetBounds, frame.floor->yMm, tolerance, stop);
        if (!overlap.IsNull()) {
            result.push_back({{"sheet", "Деталь", "floor", "Пол"}, overlap});
            if (firstOnly)
                return result;
        }
    }
    if (frame.machine.empty())
        return result;
    std::vector<TopoDS_Shape> shapes{frame.shape, frame.tools[0], frame.tools[1]};
    shapes.reserve(frame.machine.size() + 3);
    for (const auto &c : frame.machine)
        shapes.push_back(c.shape);
    std::vector<Bnd_Box> boxes;
    boxes.reserve(shapes.size());
    for (const auto &shape : shapes) {
        checkStop(stop);
        auto b = boxes.empty() && sheetBounds ? *sheetBounds : bounds(shape);
        b.Enlarge(tolerance);
        boxes.push_back(b);
    }
    const auto test = [&](std::size_t a, std::size_t b, application::MachineCollision pair,
                          bool rigidPair = false) {
        checkStop(stop);
        if (rigidPair && cache && cache->contains(shapes[a], shapes[b], tolerance))
            return false;
        TopoDS_Shape overlap;
        if (!intersects(shapes[a], shapes[b], boxes[a], boxes[b], tolerance, stop, &overlap)) {
            if (rigidPair && cache)
                cache->remember(shapes[a], shapes[b], tolerance);
            return false;
        }
        result.push_back({std::move(pair), std::move(overlap)});
        return firstOnly;
    };
    for (std::size_t i = 0; i < frame.machine.size(); ++i) {
        const auto &c = frame.machine[i];
        if (test(0, i + 3, {"sheet", "Деталь", c.id, c.name}))
            return result;
        for (std::size_t t = 0; t < 2; ++t) {
            if ((t == 0 && c.allowDieContact) || (t == 1 && c.allowPunchContact))
                continue;
            if (test(t + 1, i + 3,
                     {t == 0 ? "die" : "punch", t == 0 ? "Матрица" : "Пуансон", c.id, c.name},
                     (t == 1) == c.moving))
                return result;
        }
    }
    for (std::size_t a = 0; a < frame.machine.size(); ++a)
        if (frame.machine[a].moving)
            for (std::size_t b = 0; b < frame.machine.size(); ++b)
                if (!frame.machine[b].moving && test(a + 3, b + 3,
                                                     {frame.machine[a].id, frame.machine[a].name,
                                                      frame.machine[b].id, frame.machine[b].name}))
                    return result;
    return result;
}
} // namespace
std::optional<application::MachineCollision> MachineAssembly::check(const PreviewFrame &frame,
                                                                    double tolerance,
                                                                    std::stop_token stop,
                                                                    TopoDS_Shape *overlap) {
    const auto hits = collectCollisions(frame, tolerance, stop, true);
    if (overlap)
        *overlap = hits.empty() ? TopoDS_Shape{} : hits.front().overlap;
    return hits.empty() ? std::nullopt : std::optional{hits.front().pair};
}
std::vector<MachineCollisionEvidence> MachineAssembly::collisions(const PreviewFrame &frame,
                                                                  double tolerance,
                                                                  std::stop_token stop,
                                                                  RigidClearanceCache *cache) {
    return collectCollisions(frame, tolerance, stop, false, cache);
}
} // namespace bending::cad
