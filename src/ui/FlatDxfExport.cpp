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
bool finite(const PointMm &p) {
    return std::isfinite(p.x) && std::isfinite(p.y);
}

class Dxf {
  public:
    QByteArray bytes;

    void tag(int code, const QByteArray &value) {
        bytes += QByteArray::number(code) + '\n' + value + '\n';
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
    void continuousLinetype() {
        tag(0, "LTYPE");
        handle();
        tag(330, tableHandle_);
        tag(100, "AcDbSymbolTableRecord");
        tag(100, "AcDbLinetypeTableRecord");
        tag(2, "CONTINUOUS");
        integer(70, 0);
        tag(3, "Solid line");
        integer(72, 65);
        integer(73, 0);
        real(40, 0);
    }
    void layer(const QByteArray &name, int color) {
        tag(0, "LAYER");
        handle();
        tag(330, tableHandle_);
        tag(100, "AcDbSymbolTableRecord");
        tag(100, "AcDbLayerTableRecord");
        tag(2, name);
        integer(70, 0);
        integer(62, color);
        tag(6, "CONTINUOUS");
    }
    void entity(const QByteArray &type, const QByteArray &layer, const QByteArray &subclass) {
        tag(0, type);
        handle();
        tag(100, "AcDbEntity");
        tag(8, layer);
        tag(100, subclass);
    }
    void line(const QByteArray &layer, const PointMm &start, const PointMm &end) {
        entity("LINE", layer, "AcDbLine");
        xy(10, start);
        real(30, 0);
        xy(11, end);
        real(31, 0);
    }

  private:
    QByteArray handle() {
        const auto value = QByteArray::number(nextHandle_++, 16).toUpper();
        tag(5, value);
        return value;
    }
    QByteArray tableHandle_;
    unsigned nextHandle_ = 0x100;
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
        dxf.line("CUT", start, end);
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
    dxf.entity("ARC", "CUT", "AcDbCircle");
    dxf.xy(10, centre);
    dxf.real(30, 0);
    dxf.real(40, radius);
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
    std::array<double, 4> cuts{0, turn};
    std::size_t count = 2;
    for (double extremum : {std::numbers::pi / 2, -std::numbers::pi / 2}) {
        auto distance = std::fmod((extremum - start) * direction + fullTurn, fullTurn);
        if (distance > 0 && distance < turn)
            cuts[count++] = distance;
    }
    std::sort(cuts.begin(), cuts.begin() + count);
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
    dxf.section("HEADER");
    dxf.tag(9, "$ACADVER");
    dxf.tag(1, "AC1015");
    dxf.tag(9, "$INSUNITS");
    dxf.integer(70, 4);
    dxf.tag(9, "$MEASUREMENT");
    dxf.integer(70, 1);
    dxf.tag(0, "ENDSEC");
    dxf.section("TABLES");
    dxf.table("LTYPE", 1);
    dxf.continuousLinetype();
    dxf.tag(0, "ENDTAB");
    dxf.table("LAYER", includeBends ? 3 : 2);
    dxf.layer("0", 7);
    dxf.layer("CUT", 7);
    if (includeBends)
        dxf.layer(bendLayer, options.bendColor);
    dxf.tag(0, "ENDTAB");
    dxf.tag(0, "ENDSEC");
    dxf.section("ENTITIES");
    const auto frame = flatDxfFrame(flat);
    for (const auto &loop : flat.cutProfile->loops) {
        if (loop.radiusMm > 0 && std::isfinite(loop.radiusMm) && finite(loop.centre)) {
            dxf.entity("CIRCLE", "CUT", "AcDbCircle");
            dxf.xy(10, frame.map(loop.centre));
            dxf.real(30, 0);
            dxf.real(40, loop.radiusMm);
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
    dxf.tag(0, "ENDSEC");
    dxf.tag(0, "EOF");

    QSaveFile file(path);
    if (!file.open(QIODevice::WriteOnly) || file.write(dxf.bytes) != dxf.bytes.size() ||
        !file.commit())
        return QStringLiteral("Не удалось сохранить DXF. %1").arg(file.errorString());
    return {};
}
} // namespace bending::ui
