#include "FlatDxfExport.h"
#include "FlatDxfGeometry.h"

#include <QRegularExpression>
#include <QSaveFile>
#include <algorithm>
#include <array>
#include <cmath>
#include <limits>
#include <numbers>
#include <numeric>

namespace bending::ui {
namespace {
using application::PointMm;
constexpr double axisTolerance = 1.e-8;
constexpr double markingGapMm = .3;
constexpr double fullTurn = 2 * std::numbers::pi;
constexpr auto cutLayer = "0";
bool finite(const PointMm &p) {
    return std::isfinite(p.x) && std::isfinite(p.y);
}

class Dxf {
  public:
    void tag(int code, const QByteArray &value) {
        bytes_ += QByteArray::number(code).rightJustified(3, ' ');
        bytes_ += "\r\n";
        bytes_ += value;
        bytes_ += "\r\n";
    }
    void integer(int code, int value) {
        tag(code, QByteArray::number(value));
    }
    void real(int code, double value) {
        tag(code, QByteArray::number(value == 0 ? 0. : value, 'g', 17));
    }
    void xy(int code, const PointMm &p) {
        real(code, p.x);
        real(code + 10, p.y);
    }
    void section(const QByteArray &name) {
        tag(0, "SECTION");
        tag(2, name);
    }
    void table(const QByteArray &name, int count) {
        tag(0, "TABLE");
        tag(2, name);
        tableHandle_ = handle();
        tag(330, "0");
        tag(100, "AcDbSymbolTable");
        integer(70, count);
    }
    void entity(const QByteArray &type, const QByteArray &layer, const QByteArray &subclass) {
        ownedEntity(type, layer, subclass, model_.block);
    }
    void line(const QByteArray &layer, const PointMm &start, const PointMm &end) {
        include(start);
        include(end);
        entity("LINE", layer, "AcDbLine");
        xy(10, start);
        real(30, 0);
        xy(11, end);
        real(31, 0);
    }
    void include(const PointMm &p) {
        minimum_.x = std::min(minimum_.x, p.x);
        minimum_.y = std::min(minimum_.y, p.y);
        maximum_.x = std::max(maximum_.x, p.x);
        maximum_.y = std::max(maximum_.y, p.y);
    }
    void circleGeometry(const PointMm &centre, double radius) {
        point(10, centre.x, centre.y, 0);
        real(40, radius);
    }

    void begin(const QByteArray &bendLayer, int bendColor) {
        section("CLASSES");
        objectClass("ACDBDICTIONARYWDFLT", "AcDbDictionaryWithDefault", 0, 1);
        objectClass("MATERIAL", "AcDbMaterial", 1153, 3);
        tag(0, "ENDSEC");
        section("TABLES");
        table("VPORT", 1);
        tableRecord("VPORT", "AcDbViewportTableRecord", "*Active");
        integer(70, 0);
        xy(10, {0, 0, 0});
        xy(11, {1, 1, 0});
        point(16, 0, 0, 1);
        point(17, 0, 0, 0);
        real(40, 1000);
        real(41, 1.34);
        tag(0, "ENDTAB");
        table("LTYPE", 3);
        for (const auto *name : {"ByBlock", "ByLayer", "Continuous"}) {
            tableRecord("LTYPE", "AcDbLinetypeTableRecord", name);
            integer(70, 0);
            tag(3, "");
            integer(72, 65);
            integer(73, 0);
            real(40, 0);
        }
        tag(0, "ENDTAB");
        table("LAYER", bendLayer.isEmpty() ? 1 : 2);
        layer(cutLayer, 7);
        if (!bendLayer.isEmpty())
            layer(bendLayer, bendColor);
        tag(0, "ENDTAB");
        table("STYLE", 1);
        tableRecord("STYLE", "AcDbTextStyleTableRecord", "Standard", textStyle_);
        integer(70, 0);
        real(40, 0);
        real(41, 1);
        real(50, 0);
        integer(71, 0);
        real(42, 2.5);
        tag(3, "txt");
        tag(4, "");
        tag(0, "ENDTAB");
        table("VIEW", 0);
        tag(0, "ENDTAB");
        table("UCS", 0);
        tag(0, "ENDTAB");
        table("APPID", 1);
        tableRecord("APPID", "AcDbRegAppTableRecord", "ACAD");
        integer(70, 0);
        tag(0, "ENDTAB");
        table("DIMSTYLE", 1);
        tag(100, "AcDbDimStyleTable");
        integer(71, 1);
        tag(340, dimensionStyle_);
        tableRecord("DIMSTYLE", "AcDbDimStyleTableRecord", "Standard", dimensionStyle_, 105);
        integer(70, 0);
        tag(340, textStyle_);
        tag(0, "ENDTAB");
        table("BLOCK_RECORD", 2);
        blockRecord(model_);
        blockRecord(paper_);
        tag(0, "ENDTAB");
        tag(0, "ENDSEC");

        section("BLOCKS");
        block(model_);
        block(paper_);
        tag(0, "ENDSEC");
        section("ENTITIES");
    }

    bool writeTo(QIODevice &file) {
        tag(0, "ENDSEC");
        section("OBJECTS");
        dictionary("DICTIONARY", root_, "0");
        dictionaryEntry("ACAD_GROUP", groups_);
        dictionaryEntry("ACAD_LAYOUT", layouts_);
        dictionaryEntry("ACAD_PLOTSTYLENAME", plotStyles_);
        dictionaryEntry("ACAD_MATERIAL", materials_);
        dictionary("DICTIONARY", groups_, root_);
        dictionary("DICTIONARY", layouts_, root_);
        dictionaryEntry(model_.layoutName, model_.layout);
        dictionaryEntry(paper_.layoutName, paper_.layout);
        dictionary("ACDBDICTIONARYWDFLT", plotStyles_, root_);
        dictionaryEntry("Normal", normalPlotStyle_);
        tag(100, "AcDbDictionaryWithDefault");
        tag(340, normalPlotStyle_);
        tag(0, "ACDBPLACEHOLDER");
        tag(5, normalPlotStyle_);
        tag(330, plotStyles_);
        dictionary("DICTIONARY", materials_, root_);
        dictionaryEntry("ByBlock", byBlockMaterial_);
        dictionaryEntry("ByLayer", byLayerMaterial_);
        dictionaryEntry("Global", globalMaterial_);
        material("ByBlock", byBlockMaterial_);
        material("ByLayer", byLayerMaterial_);
        material("Global", globalMaterial_);
        layout(model_, 0);
        layout(paper_, 1);
        tag(0, "ENDSEC");
        tag(0, "EOF");

        Dxf header;
        header.section("HEADER");
        header.tag(9, "$ACADVER");
        header.tag(1, "AC1027");
        header.tag(9, "$ACADMAINTVER");
        header.integer(70, 105);
        header.tag(9, "$DWGCODEPAGE");
        header.tag(3, "ANSI_1251");
        header.tag(9, "$EXTMIN");
        header.point(10, minimum_.x, minimum_.y, 0);
        header.tag(9, "$EXTMAX");
        header.point(10, maximum_.x, maximum_.y, 0);
        header.tag(9, "$LUNITS");
        header.integer(70, 2);
        header.tag(9, "$INSUNITS");
        header.integer(70, 4);
        header.tag(9, "$MEASUREMENT");
        header.integer(70, 1);
        header.tag(9, "$TILEMODE");
        header.integer(70, 1);
        header.tag(9, "$DIMSTYLE");
        header.tag(2, "Standard");
        header.tag(9, "$CMATERIAL");
        header.tag(347, byLayerMaterial_);
        header.tag(9, "$HANDSEED");
        header.tag(5, nextHandle());
        header.tag(0, "ENDSEC");
        return file.write(header.bytes_) == header.bytes_.size() &&
               file.write(bytes_) == bytes_.size();
    }

  private:
    struct Space {
        QByteArray blockName;
        QByteArray layoutName;
        QByteArray block;
        QByteArray layout;
    };

    void objectClass(const QByteArray &name, const QByteArray &className, int flags,
                     int instances) {
        tag(0, "CLASS");
        tag(1, name);
        tag(2, className);
        tag(3, "ObjectDBX Classes");
        integer(90, flags);
        integer(91, instances);
        integer(280, 0);
        integer(281, 0);
    }

    void ownedEntity(const QByteArray &type, const QByteArray &layer, const QByteArray &subclass,
                     const QByteArray &owner) {
        tag(0, type);
        handle();
        tag(330, owner);
        tag(100, "AcDbEntity");
        tag(8, layer);
        tag(100, subclass);
    }
    void tableRecord(const QByteArray &type, const QByteArray &subclass, const QByteArray &name,
                     const QByteArray &recordHandle = {}, int handleCode = 5) {
        tag(0, type);
        tag(handleCode, recordHandle.isEmpty() ? nextHandle() : recordHandle);
        tag(330, tableHandle_);
        tag(100, "AcDbSymbolTableRecord");
        tag(100, subclass);
        tag(2, name);
    }
    void layer(const QByteArray &name, int color) {
        tableRecord("LAYER", "AcDbLayerTableRecord", name);
        integer(70, 0);
        integer(62, color);
        tag(6, "Continuous");
        integer(370, -3);
        tag(390, normalPlotStyle_);
        tag(347, globalMaterial_);
    }
    void blockRecord(const Space &space) {
        tableRecord("BLOCK_RECORD", "AcDbBlockTableRecord", space.blockName, space.block);
        tag(340, space.layout);
        integer(70, 0);
        integer(280, 1);
        integer(281, 0);
    }
    void block(const Space &space) {
        ownedEntity("BLOCK", "0", "AcDbBlockBegin", space.block);
        tag(2, space.blockName);
        integer(70, 0);
        point(10, 0, 0, 0);
        tag(3, space.blockName);
        tag(1, "");
        ownedEntity("ENDBLK", "0", "AcDbBlockEnd", space.block);
    }
    void dictionary(const QByteArray &type, const QByteArray &id, const QByteArray &owner) {
        tag(0, type);
        tag(5, id);
        tag(330, owner);
        tag(100, "AcDbDictionary");
        integer(281, 1);
    }
    void dictionaryEntry(const QByteArray &name, const QByteArray &id) {
        tag(3, name);
        tag(350, id);
    }
    void material(const QByteArray &name, const QByteArray &id) {
        tag(0, "MATERIAL");
        tag(5, id);
        tag(102, "{ACAD_REACTORS");
        tag(330, materials_);
        tag(102, "}");
        tag(330, materials_);
        tag(100, "AcDbMaterial");
        tag(1, name);
    }
    void layout(const Space &space, int tabOrder) {
        tag(0, "LAYOUT");
        tag(5, space.layout);
        tag(330, layouts_);
        tag(100, "AcDbPlotSettings");
        tag(1, "");
        tag(2, "none_device");
        real(142, 1);
        real(143, 1);
        integer(70, tabOrder == 0 ? 1024 : 0);
        integer(72, 1);
        integer(74, 5);
        real(147, 1);
        tag(100, "AcDbLayout");
        tag(1, space.layoutName);
        integer(70, 1);
        integer(71, tabOrder);
        xy(10, {0, 0, 0});
        xy(11, {420, 297, 0});
        point(12, 0, 0, 0);
        point(14, 1.e20, 1.e20, 1.e20);
        point(15, -1.e20, -1.e20, -1.e20);
        real(146, 0);
        point(13, 0, 0, 0);
        point(16, 1, 0, 0);
        point(17, 0, 1, 0);
        integer(76, 0);
        tag(330, space.block);
    }
    void point(int code, double x, double y, double z) {
        xy(code, {x, y, z});
        real(code + 20, z);
    }

    QByteArray nextHandle() {
        return QByteArray::number(nextHandle_++, 16).toUpper();
    }
    QByteArray handle() {
        const auto value = nextHandle();
        tag(5, value);
        return value;
    }
    QByteArray bytes_;
    QByteArray tableHandle_;
    PointMm minimum_{std::numeric_limits<double>::infinity(),
                     std::numeric_limits<double>::infinity(), 0};
    PointMm maximum_{-std::numeric_limits<double>::infinity(),
                     -std::numeric_limits<double>::infinity(), 0};
    unsigned nextHandle_ = 0x100;
    // These records refer to one another, so allocate their handles before writing either section.
    const Space model_{"*Model_Space", "Model", nextHandle(), nextHandle()};
    const Space paper_{"*Paper_Space", "Layout1", nextHandle(), nextHandle()};
    const QByteArray root_ = nextHandle();
    const QByteArray groups_ = nextHandle();
    const QByteArray layouts_ = nextHandle();
    const QByteArray plotStyles_ = nextHandle();
    const QByteArray normalPlotStyle_ = nextHandle();
    const QByteArray textStyle_ = nextHandle();
    const QByteArray dimensionStyle_ = nextHandle();
    const QByteArray materials_ = nextHandle();
    const QByteArray byBlockMaterial_ = nextHandle();
    const QByteArray byLayerMaterial_ = nextHandle();
    const QByteArray globalMaterial_ = nextHandle();
};

double angleDegrees(const PointMm &point, const PointMm &centre) {
    const auto angle =
        std::atan2(point.y - centre.y, point.x - centre.x) * (180 / std::numbers::pi);
    return angle < 0 ? angle + 360 : angle;
}

bool writeCutEdge(Dxf &dxf, const application::FlatCutVertex &vertex, const PointMm &end) {
    const auto &start = vertex.point;
    const auto bulge = vertex.bulge;
    const auto dx = end.x - start.x, dy = end.y - start.y;
    const auto length = std::hypot(dx, dy);
    if (!finite(start) || !finite(end) || !std::isfinite(length) || length == 0 ||
        !std::isfinite(bulge))
        return false;
    if (bulge == 0) {
        dxf.line(cutLayer, start, end);
        return true;
    }

    const auto [centre, radius, radians] = cutArc(start, end, bulge);
    if (!finite(centre) || !std::isfinite(radius))
        return false;
    // DXF arcs in the XY plane run counterclockwise, so reverse clockwise edges.
    const auto first = angleDegrees(bulge > 0 ? start : end, centre);
    const auto last = angleDegrees(bulge > 0 ? end : start, centre);
    const auto sweep = last >= first ? last - first : 360 + last - first;
    const auto expectedSweep = std::abs(radians) * (180 / std::numbers::pi);
    if (sweep <= 0 || sweep >= 360 || std::abs(sweep - expectedSweep) > 1.e-6)
        return false;
    dxf.include(start);
    dxf.include(end);
    const std::array<PointMm, 4> extrema{{{centre.x + radius, centre.y, 0},
                                          {centre.x, centre.y + radius, 0},
                                          {centre.x - radius, centre.y, 0},
                                          {centre.x, centre.y - radius, 0}}};
    for (std::size_t i = 0; i < extrema.size(); ++i) {
        const double delta = 90 * static_cast<double>(i) - first;
        if ((delta < 0 ? delta + 360 : delta) <= sweep)
            dxf.include(extrema[i]);
    }
    dxf.entity("ARC", cutLayer, "AcDbCircle");
    dxf.circleGeometry(centre, radius);
    dxf.tag(100, "AcDbArc");
    dxf.real(50, first);
    dxf.real(51, last);
    return true;
}

struct AxisEvent {
    double x;
    bool crossing = false;
    int boundaryDelta = 0;
};

double axisY(double y) {
    return std::abs(y) <= axisTolerance ? 0 : y;
}

// Apply the same half-open rule to straight and monotone curved edges.
// At a shared vertex this counts one crossing or a touch, independent of wire direction.
template <class Root>
void intersectMonotone(QPointF a, QPointF b, Root root, std::vector<AxisEvent> &events) {
    if (a.y() == 0)
        events.push_back({a.x(), b.y() > 0});
    if (b.y() == 0)
        events.push_back({b.x(), a.y() > 0});
    if ((a.y() < 0 && b.y() > 0) || (b.y() < 0 && a.y() > 0))
        events.push_back({root(), true});
}

double circleHalfChord(QPointF centre, double radius) {
    const auto y = std::abs(centre.y());
    return std::sqrt(std::max(0., (radius - y) * (radius + y)));
}

void intersectArc(QPointF a, QPointF b, QPointF centre, double radius, double sweep,
                  std::vector<AxisEvent> &events) {
    if (std::abs(centre.y()) > radius + axisTolerance)
        return;
    const auto start = std::atan2(a.y() - centre.y(), a.x() - centre.x());
    const auto direction = sweep < 0 ? -1. : 1.;
    const auto turn = std::abs(sweep);
    std::array<double, 4> cuts{};
    std::size_t count = 1;
    for (double extremum : {std::numbers::pi / 2, -std::numbers::pi / 2}) {
        auto distance = std::fmod((extremum - start) * direction + fullTurn, fullTurn);
        if (distance > 0 && distance < turn)
            cuts[count++] = distance;
    }
    if (count == 3 && cuts[1] > cuts[2])
        std::swap(cuts[1], cuts[2]);
    cuts[count++] = turn;
    const auto halfChord = circleHalfChord(centre, radius);
    for (std::size_t i = 1; i < count; ++i) {
        const auto angle = start + direction * cuts[i];
        const auto next =
            i + 1 == count
                ? b
                : QPointF(centre.x(), axisY(centre.y() + std::copysign(radius, std::sin(angle))));
        const auto middle = start + direction * std::midpoint(cuts[i - 1], cuts[i]);
        intersectMonotone(
            a, next, [&] { return centre.x() + std::copysign(halfChord, std::cos(middle)); },
            events);
        a = next;
    }
}

void collectAxisEvents(const application::FlatCutProfile &profile, QPointF origin, QPointF unit,
                       std::vector<AxisEvent> &events) {
    const auto local = [=](const PointMm &point) {
        const auto x = point.x - origin.x(), y = point.y - origin.y();
        return QPointF(x * unit.x() + y * unit.y(), axisY(y * unit.x() - x * unit.y()));
    };
    for (const auto &loop : profile.loops) {
        if (loop.radiusMm > 0) {
            const auto centre = local(loop.centre);
            const auto offset = loop.radiusMm - std::abs(centre.y());
            if (std::abs(offset) <= axisTolerance)
                events.push_back({centre.x()});
            else if (offset > 0) {
                const auto half = circleHalfChord(centre, loop.radiusMm);
                events.push_back({centre.x() - half, true});
                events.push_back({centre.x() + half, true});
            }
            continue;
        }
        for (std::size_t i = 0; i < loop.vertices.size(); ++i) {
            const auto &start = loop.vertices[i];
            const auto &end = loop.vertices[(i + 1) % loop.vertices.size()].point;
            const auto a = local(start.point), b = local(end);
            if (start.bulge != 0) {
                const auto [centre, radius, sweep] = cutArc(start.point, end, start.bulge);
                intersectArc(a, b, local(centre), radius, sweep, events);
            } else if (a.y() == 0 && b.y() == 0) {
                events.push_back({std::min(a.x(), b.x()), false, 1});
                events.push_back({std::max(a.x(), b.x()), false, -1});
            } else {
                intersectMonotone(
                    a, b, [&] { return std::lerp(a.x(), b.x(), a.y() / (a.y() - b.y())); }, events);
            }
        }
    }
}
} // namespace

FlatDxfFrame flatDxfFrame(const application::FlatPatternInfo &flat) {
    if (!flat.cutProfile || flat.cutProfile->loops.empty() || flat.bends.size() < 3 ||
        flat.bends.size() > application::maximumFlatBendSegments)
        return {};
    constexpr double join = 1.e-5;
    double longest = 0;
    double bestSpan = 0;
    QPointF bestOrigin, bestAxis;
    std::vector<QLineF> axes;
    for (const auto &bend : flat.bends) {
        const auto ends = bend.centreLine();
        const QLineF line(ends[0].x, ends[0].y, ends[1].x, ends[1].y);
        if (!finite(ends[0]) || !finite(ends[1]) || !std::isfinite(line.length()) ||
            line.length() <= join)
            return {};
        longest = std::max(longest, line.length());
        axes.push_back(line);
    }
    // A chain of separate zones is a stronger datum than one isolated bend.
    // Evaluate entire finite intervals; parallel but offset lines are unrelated.
    for (const auto &axis : axes) {
        const auto origin = axis.p1(), unit = (axis.p2() - origin) / axis.length();
        const auto distance = [&](QPointF p) {
            p -= origin;
            return std::abs(p.y() * unit.x() - p.x() * unit.y());
        };
        std::vector<std::pair<double, double>> intervals;
        for (const auto &line : axes)
            if (distance(line.p1()) <= join && distance(line.p2()) <= join) {
                const double a = QPointF::dotProduct(line.p1() - origin, unit);
                const double b = QPointF::dotProduct(line.p2() - origin, unit);
                intervals.emplace_back(std::min(a, b), std::max(a, b));
            }
        if (intervals.size() < 3)
            continue;
        std::sort(intervals.begin(), intervals.end());
        if (std::adjacent_find(intervals.begin(), intervals.end(), [](auto a, auto b) {
                return b.first < a.second - join;
            }) != intervals.end())
            continue;
        const auto span = intervals.back().second - intervals.front().first;
        if (span > bestSpan + join) {
            bestSpan = span;
            bestOrigin = origin;
            bestAxis = unit;
        }
    }
    // Both axis-aligned orientations are already intentional/useful in CAM.
    // Correct an oblique datum, not an existing portrait or landscape layout.
    constexpr double aligned = 1.e-7;
    if (bestSpan <= longest + join || std::abs(bestAxis.y()) <= aligned ||
        std::abs(bestAxis.x()) <= aligned)
        return {};

    FlatDxfFrame frame{bestAxis.x(), bestAxis.y()};
    const auto &outer = flat.cutProfile->loops.front();
    if (outer.vertices.empty())
        return {};
    double left = std::numeric_limits<double>::infinity(), bottom = left;
    double right = -left, top = -left;
    const auto include = [&](PointMm p) {
        left = std::min(left, p.x);
        bottom = std::min(bottom, p.y);
        right = std::max(right, p.x);
        top = std::max(top, p.y);
    };
    for (std::size_t i = 0; i < outer.vertices.size(); ++i) {
        const auto &v = outer.vertices[i];
        const auto a = frame.map(v.point);
        include(a);
        if (v.bulge == 0)
            continue;
        const auto b = frame.map(outer.vertices[(i + 1) % outer.vertices.size()].point);
        const auto arc = cutArc(a, b, v.bulge);
        const double start = std::atan2(a.y - arc.centre.y, a.x - arc.centre.x);
        for (int j = 0; j < 4; ++j) {
            const double angle = j * std::numbers::pi / 2;
            double delta = std::fmod((angle - start) * std::copysign(1., arc.sweep), fullTurn);
            if (delta < 0)
                delta += fullTurn;
            if (delta <= std::abs(arc.sweep))
                include({arc.centre.x + arc.radius * std::cos(angle),
                         arc.centre.y + arc.radius * std::sin(angle), 0});
        }
    }
    if (!std::isfinite(left) || !std::isfinite(bottom) || !std::isfinite(right) ||
        !std::isfinite(top))
        return {};
    const double datum = frame.map({bestOrigin.x(), bestOrigin.y(), 0}).y;
    // Put the larger extent of the stock above the datum, without mirroring.
    if (datum - bottom > top - datum + join)
        return {-frame.cosine, -frame.sine, right, top};
    return {frame.cosine, frame.sine, -left, -bottom};
}

std::vector<QLineF> dxfBendLines(const application::FlatPatternInfo &flat) {
    std::vector<QLineF> result;
    if (!flat.cutProfile)
        return result;
    std::vector<AxisEvent> events;
    for (const auto &bend : flat.bends) {
        const auto ends = bend.centreLine();
        const QPointF origin(ends[0].x, ends[0].y), target(ends[1].x, ends[1].y);
        const auto length = QLineF(origin, target).length();
        if (!std::isfinite(length) || length <= axisTolerance)
            continue;
        const auto unit = (target - origin) / length;
        events.clear();
        collectAxisEvents(*flat.cutProfile, origin, unit, events);
        std::sort(events.begin(), events.end(),
                  [](const auto &a, const auto &b) { return a.x < b.x; });
        bool inside = false;
        int boundary = 0;
        std::size_t i = 0;
        while (i < events.size()) {
            const auto left = events[i].x;
            do {
                inside ^= events[i].crossing;
                boundary += events[i++].boundaryDelta;
            } while (i < events.size() && events[i].x - left <= axisTolerance);
            if (i == events.size() || !inside || boundary > 0)
                continue;
            const auto from = std::max(0., events[i - 1].x + markingGapMm);
            const auto to = std::min(length, events[i].x - markingGapMm);
            if (to - from > axisTolerance)
                result.emplace_back(origin + unit * from, origin + unit * to);
        }
    }
    return result;
}

bool validBendLayer(const QString &name) {
    static const QRegularExpression pattern(
        QStringLiteral("\\A[A-Za-z0-9_][A-Za-z0-9_-]{0,62}\\z"));
    return name != QStringLiteral("0") &&
           name.compare(QStringLiteral("CUT"), Qt::CaseInsensitive) &&
           name.compare(QStringLiteral("DEFPOINTS"), Qt::CaseInsensitive) &&
           pattern.match(name).hasMatch();
}

QString writeFlatDxf(const QString &path, const application::FlatPatternInfo &flat,
                     const FlatDxfOptions &options) {
    if (!flat.cutProfile || flat.cutProfile->loops.empty())
        return QStringLiteral("Контуры для резки не подтверждены.");
    const bool includeBends = options.includeBends && !flat.bends.empty();
    if (includeBends &&
        (!validBendLayer(options.bendLayer) || options.bendColor < 1 || options.bendColor > 255))
        return QStringLiteral("Проверьте имя и цвет слоя линий гиба.");
    const auto bendLayer = options.bendLayer.toLatin1();
    Dxf dxf;
    dxf.begin(includeBends ? bendLayer : QByteArray{}, options.bendColor);
    const auto frame = flatDxfFrame(flat);
    for (const auto &loop : flat.cutProfile->loops) {
        if (loop.radiusMm > 0 && std::isfinite(loop.radiusMm) && finite(loop.centre)) {
            const auto centre = frame.map(loop.centre);
            const auto radius = loop.radiusMm;
            const PointMm minimum{centre.x - radius, centre.y - radius, 0};
            const PointMm maximum{centre.x + radius, centre.y + radius, 0};
            if (!finite(minimum) || !finite(maximum))
                return QStringLiteral("Некорректный контур резки.");
            dxf.include(minimum);
            dxf.include(maximum);
            dxf.entity("CIRCLE", cutLayer, "AcDbCircle");
            dxf.circleGeometry(centre, radius);
            continue;
        }
        if (loop.vertices.size() < 2 || loop.vertices.size() > 200000 || loop.radiusMm != 0)
            return QStringLiteral("Некорректный контур резки.");
        for (std::size_t i = 0; i < loop.vertices.size(); ++i) {
            const auto &next = loop.vertices[(i + 1) % loop.vertices.size()].point;
            const auto &v = loop.vertices[i];
            if (!writeCutEdge(dxf, {frame.map(v.point), v.bulge}, frame.map(next)))
                return QStringLiteral("Некорректный участок контура резки.");
        }
    }
    if (includeBends) {
        for (const auto &bend : flat.bends) {
            const auto ends = bend.centreLine();
            if (!finite(ends[0]) || !finite(ends[1]) ||
                std::hypot(ends[1].x - ends[0].x, ends[1].y - ends[0].y) <= 1.e-8)
                return QStringLiteral("Некорректная линия гиба.");
        }
        for (const auto &line : dxfBendLines(flat))
            dxf.line(bendLayer, frame.map({line.x1(), line.y1(), 0}),
                     frame.map({line.x2(), line.y2(), 0}));
    }
    QSaveFile file(path);
    if (!file.open(QIODevice::WriteOnly) || !dxf.writeTo(file) || !file.commit())
        return QStringLiteral("Не удалось сохранить DXF. %1").arg(file.errorString());
    return {};
}
} // namespace bending::ui
