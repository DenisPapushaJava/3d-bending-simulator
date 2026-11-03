#include "bending/ui/DrawingSheet.h"
#include "DrawingSheetInternal.h"
#include <QColor>
#include <QFontDatabase>
#include <QFontMetricsF>
#include <QPageSize>
#include <QPainter>
#include <QPainterPath>
#include <QPdfWriter>
#include <QSaveFile>
#include <QStringList>
#include <algorithm>
#include <cmath>
#include <numbers>
#include <stdexcept>

static void drawingResources() {
    Q_INIT_RESOURCE(drawing_assets);
}
namespace bending::ui {
namespace {
using namespace application;
QString bendId(const BendingDrawing &d, std::size_t index);
double fittedScale(const DrawingView &v, double width, double height);
std::optional<double> commonBendValue(const BendingDrawing &d, bool length) {
    std::optional<double> value;
    for (const auto &b : d.bends) {
        if (b.diagnostic)
            continue;
        const auto candidate = length ? b.axialLengthMm : b.innerRadiusMm;
        if (value && std::abs(*value - candidate) > 1e-5)
            return {};
        value = candidate;
    }
    return value;
}
QString family() {
    static const QString name = [] {
        drawingResources();
        const int id =
            QFontDatabase::addApplicationFont(QStringLiteral(":/drawing/fonts/GOST-Type-A.ttf"));
        const auto names = QFontDatabase::applicationFontFamilies(id);
        if (id < 0 || names.isEmpty())
            throw std::runtime_error("Cannot load the drawing font");
        return names.front();
    }();
    return name;
}
void text(QPainter &p, QRectF area, const QString &value, double height = 3.5,
          int flags = Qt::AlignCenter) {
    QFont font(family());
    font.setPixelSize(1000);
    const QFontMetricsF metrics(font);
    const double factor = height / metrics.capHeight();
    p.save();
    p.setPen(Qt::black);
    p.setFont(font);
    p.translate(area.topLeft());
    p.scale(factor, factor);
    p.drawText(QRectF(0, 0, area.width() / factor, area.height() / factor), flags, value);
    p.restore();
}
QString number(double value, int decimals = 2) {
    if (std::abs(value) < std::min(.0005, .5 * std::pow(10., -decimals)))
        value = 0;
    QString result = QString::number(value, 'f', decimals);
    while (result.endsWith(QLatin1Char('0')))
        result.chop(1);
    if (result.endsWith(QLatin1Char('.')))
        result.chop(1);
    return result.replace(QLatin1Char('.'), QLatin1Char(','));
}
QString scaleName(double scale) {
    return scale >= 1 ? number(scale) + QStringLiteral(":1")
                      : QStringLiteral("1:") + number(1 / scale);
}
void pen(QPainter &p, double width = .25, Qt::PenStyle style = Qt::SolidLine) {
    QPen line(Qt::black, width, style);
    if (style == Qt::DashLine)
        line.setDashPattern({3 / width, 1.5 / width});
    else if (style == Qt::DashDotLine)
        line.setDashPattern({5 / width, 3 / width, .25 / width, 3 / width});
    p.setPen(line);
    p.setBrush(Qt::NoBrush);
}
const QColor dimensionColor(96, 96, 96);
void dimensionPen(QPainter &p) {
    p.setPen(QPen(dimensionColor, .18));
    p.setBrush(Qt::NoBrush);
}
void arrow(QPainter &p, QPointF tip, QPointF from, const QColor &color = Qt::black) {
    const auto delta = from - tip;
    const double length = std::hypot(delta.x(), delta.y());
    if (length < 1e-9)
        return;
    const auto d = delta / length;
    const QPointF n(-d.y(), d.x());
    p.save();
    p.setBrush(color);
    p.setPen(Qt::NoPen);
    p.drawPolygon(QPolygonF{tip, tip + d * 3 + n * .45, tip + d * 3 - n * .45});
    p.restore();
}
bool overlaps(const QRectF &box, const std::vector<QRectF> &occupied) {
    return std::any_of(occupied.begin(), occupied.end(), [&](const auto &old) {
        return old.adjusted(-.8, -.8, .8, .8).intersects(box);
    });
}
bool crossesText(QPointF a, QPointF b, const std::vector<QRectF> &occupied) {
    for (const auto &text : occupied) {
        const auto box = text.adjusted(-.4, -.4, .4, .4);
        if (box.contains(a) || box.contains(b))
            return true;
        const QLineF line(a, b);
        for (const QLineF edge :
             {QLineF(box.topLeft(), box.topRight()), QLineF(box.topRight(), box.bottomRight()),
              QLineF(box.bottomRight(), box.bottomLeft()), QLineF(box.bottomLeft(), box.topLeft())})
            if (line.intersects(edge, nullptr) == QLineF::BoundedIntersection)
                return true;
    }
    return false;
}
struct DimensionPlacement {
    std::array<int, 4> nextLane{}; // Top, right, bottom, left, in paper coordinates.
    std::vector<QLineF> lines;
    QRectF area;
};
void dimension(QPainter &p, QPointF a, QPointF b, QRectF bounds, const DrawingDimension &d,
               std::vector<QRectF> &occupied, DimensionPlacement *placement = nullptr,
               bool holePosition = false, std::optional<std::array<QPointF, 2>> witnesses = {}) {
    const auto firstWitness = witnesses ? (*witnesses)[0] : a;
    const auto secondWitness = witnesses ? (*witnesses)[1] : b;
    const double gap = witnesses ? .5 : .8;
    const auto vector = b - a;
    const double length = std::hypot(vector.x(), vector.y());
    if (length < 1e-7)
        return;
    const auto direction = vector / length;
    QPointF normal(-direction.y(), direction.x());
    const auto middle = (a + b) / 2;
    if (QPointF::dotProduct(middle - bounds.center(), normal) < 0)
        normal = -normal;
    if (holePosition)
        normal = std::abs(vector.x()) > std::abs(vector.y()) ? QPointF(0, -1) : QPointF(1, 0);
    int pitchDecimals = 2;
    // The displayed pitch must still reproduce the displayed total span after
    // multiplication; rounding a long fractional pattern to 0.01 can change it.
    for (double factor = 100; d.pitchIntervals && pitchDecimals < 6 &&
                              std::abs(std::round(d.pitchMm * factor) / factor * d.pitchIntervals -
                                       std::round(d.valueMm * 100) / 100) > .005;
         ++pitchDecimals, factor *= 10) {
    }
    const QString value = d.pitchIntervals
                              ? QStringLiteral("%1×%2=%3")
                                    .arg(d.pitchIntervals)
                                    .arg(number(d.pitchMm, pitchDecimals), number(d.valueMm))
                              : (d.label.empty() || d.label.starts_with("H")
                                     ? QString{}
                                     : QString::fromStdString(d.label) + QLatin1Char('=')) +
                                    number(d.valueMm);
    const double textWidth = std::max(9., value.size() * 2.2);
    const bool outside = length < textWidth + 7;
    double angle = std::atan2(direction.y(), direction.x()) * 180 / std::numbers::pi;
    if (angle >= 90 - 1e-8 || angle < -90)
        angle += 180;
    QPointF da, db, label;
    bool found = false;
    for (int side = 0; side < 2 && !found; ++side) {
        if (side)
            normal = -normal;
        double maximum = -1e30;
        for (auto corner :
             {bounds.topLeft(), bounds.topRight(), bounds.bottomLeft(), bounds.bottomRight()})
            maximum = std::max(maximum, QPointF::dotProduct(corner - middle, normal));
        const std::size_t sideIndex = std::abs(normal.x()) > std::abs(normal.y())
                                          ? (normal.x() > 0 ? 1 : 3)
                                          : (normal.y() > 0 ? 2 : 0);
        const int firstLane = placement ? placement->nextLane[sideIndex] : 0;
        for (int lane = firstLane; lane < firstLane + 8 && !found; ++lane) {
            const double offset = std::max(10.0, maximum + 10) + lane * 7;
            da = a + normal * offset;
            db = b + normal * offset;
            const std::array<QLineF, 3> segments{
                QLineF(da - direction * (outside ? 6 : 0), db + direction * (outside ? 6 : 0)),
                QLineF(firstWitness + normal * gap, da + normal * 2),
                QLineF(secondWitness + normal * gap, db + normal * 2)};
            for (int position = 0; position < (outside ? 2 : 3); ++position) {
                label = outside
                            ? (position == 0 ? db + direction * (textWidth / 2 + 5)
                                             : da - direction * (textWidth / 2 + 5))
                            : (da + db) / 2 +
                                  direction * (position == 0
                                                   ? 0
                                                   : (position == 1 ? -1 : 1) *
                                                         std::min(length / 4,
                                                                  (length - textWidth - 7) / 2));
                QTransform transform;
                transform.translate(label.x(), label.y());
                transform.rotate(angle);
                const auto rect = transform.mapRect(QRectF(-textWidth / 2, -5, textWidth, 4.5));
                const bool crosses =
                    placement &&
                    (std::any_of(segments.begin(), segments.end(),
                                 [&](auto line) {
                                     return crossesText(line.p1(), line.p2(), occupied) ||
                                            !placement->area.contains(line.p1()) ||
                                            !placement->area.contains(line.p2());
                                 }) ||
                     std::any_of(placement->lines.begin(), placement->lines.end(), [&](auto line) {
                         return crossesText(line.p1(), line.p2(), {rect});
                     }));
                if (!crosses && !overlaps(rect, occupied) &&
                    (placement ? placement->area : QRectF(25, 20, 385, 193)).contains(rect)) {
                    occupied.push_back(rect);
                    if (placement) {
                        placement->nextLane[sideIndex] = lane + 1;
                        placement->lines.insert(placement->lines.end(), segments.begin(),
                                                segments.end());
                    }
                    found = true;
                    break;
                }
            }
        }
    }
    if (!found)
        throw std::runtime_error(
            QStringLiteral("Не удалось разместить размер %1. Измените ориентацию.")
                .arg(value)
                .toStdString());
    dimensionPen(p);
    p.drawLine(firstWitness + normal * gap, da + normal * 2);
    p.drawLine(secondWitness + normal * gap, db + normal * 2);
    p.drawLine(da - direction * (outside ? 6 : 0), db + direction * (outside ? 6 : 0));
    arrow(p, da, outside ? da - direction * 4 : db, dimensionColor);
    arrow(p, db, outside ? db + direction * 4 : da, dimensionColor);
    p.save();
    p.translate(label);
    p.rotate(angle);
    text(p, QRectF(-textWidth / 2, -5, textWidth, 4.5), value);
    p.restore();
}
void angularDimension(QPainter &p, QPointF vertex, QPointF first, QPointF second,
                      const DrawingAngle &angle, double scale, QRectF area,
                      std::vector<QRectF> &occupied, bool fullProfile = false,
                      bool standardText = false) {
    const auto a = first - vertex, b = second - vertex;
    const double start = std::atan2(a.y(), a.x());
    const double sweep = std::atan2(a.x() * b.y() - a.y() * b.x(), QPointF::dotProduct(a, b));
    if (std::abs(std::abs(sweep) - angle.radians) > 1e-5)
        throw std::runtime_error("The angle is not shown in a true profile");
    const QString value = number(angle.radians * 180 / std::numbers::pi) + QChar(0x00b0);
    const double width = std::max(fullProfile ? 8. : 10., value.size() * 2.2);
    const auto at = [&](double radians, double radius) {
        return vertex + QPointF(std::cos(radians), std::sin(radians)) * radius;
    };
    for (double radius = std::max(fullProfile ? 5. : 10.,
                                  angle.minimumRadiusMm * scale + (fullProfile ? 1.5 : 3));
         radius <= 45; radius += fullProfile ? 3 : 5) {
        const auto label = at(start + sweep / 2, radius + (fullProfile ? 3 : 5));
        const QRectF box(label.x() - width / 2, label.y() - 2.5, width, 5);
        if (!area.contains(box) || overlaps(box, occupied))
            continue;
        occupied.push_back(box);
        dimensionPen(p);
        p.drawLine(vertex, at(start, radius + 2));
        p.drawLine(vertex, at(start + sweep, radius + 2));
        QPainterPath arc;
        arc.moveTo(at(start, radius));
        for (int i = 1; i <= 64; ++i)
            arc.lineTo(at(start + sweep * i / 64, radius));
        p.drawPath(arc);
        arrow(p, at(start, radius), at(start + sweep * .08, radius), dimensionColor);
        arrow(p, at(start + sweep, radius), at(start + sweep * .92, radius), dimensionColor);
        text(p, box, value, fullProfile && !standardText ? 2.5 : 3.5);
        return;
    }
    throw std::runtime_error("An angular dimension does not fit; choose another orientation");
}
void leaderNote(QPainter &p, QPointF anchor, QRectF bounds, const QString &value, QRectF area,
                std::vector<QRectF> &occupied, bool sides = false,
                std::vector<QLineF> *lines = nullptr) {
    const auto textLines = value.split(QLatin1Char('\n'));
    double width = 18;
    for (const auto &line : textLines)
        width = std::max(width, line.size() * 1.6 + 2);
    const double height = textLines.size() * 4.2 + 1;
    const auto crossed = [&](QRectF box) {
        return lines && std::any_of(lines->begin(), lines->end(), [&](const auto &line) {
                   return crossesText(line.p1(), line.p2(), {box});
               });
    };
    if (lines) {
        struct Candidate {
            QRectF box;
            QPointF end;
            double score;
        };
        std::optional<Candidate> best;
        const auto consider = [&](QRectF box, QPointF end) {
            if (!area.contains(box) || box.intersects(bounds.adjusted(-1, -1, 1, 1)) ||
                overlaps(box, occupied) || crossed(box) || crossesText(anchor, end, occupied))
                return;
            const QLineF leader(anchor, end);
            double score = leader.length();
            for (const auto &line : *lines) {
                QPointF intersection;
                if (leader.intersects(line, &intersection) == QLineF::BoundedIntersection &&
                    QLineF(anchor, intersection).length() > 1e-4)
                    score += 7;
            }
            if (!best || score < best->score)
                best = Candidate{box, end, score};
        };
        for (bool above : {true, false})
            for (int row = 0; row < 5; ++row)
                for (double dx : {0., -12., 12., -24., 24., -36., 36.}) {
                    const QRectF box(
                        std::clamp(anchor.x() - width / 2 + dx, area.left(), area.right() - width),
                        above ? bounds.top() - height - 3 - row * 7 : bounds.bottom() + 3 + row * 7,
                        width, height);
                    consider(box, {box.center().x(), above ? box.bottom() : box.top()});
                }
        for (bool right : {true, false})
            for (int column = 0; column < 5; ++column)
                for (double dy : {0., -7., 7., -14., 14., -21., 21., -28., 28.}) {
                    const QRectF box(right ? bounds.right() + 6 + column * 7
                                           : bounds.left() - width - 6 - column * 7,
                                     anchor.y() - height / 2 + dy, width, height);
                    consider(box, {right ? box.left() : box.right(), box.bottom()});
                }
        if (!best)
            throw std::runtime_error("Flat bend callouts do not fit the available area");
        occupied.push_back(best->box);
        lines->emplace_back(anchor, best->end);
        dimensionPen(p);
        p.drawLine(anchor, best->end);
        arrow(p, anchor, best->end, dimensionColor);
        p.drawLine(best->box.bottomLeft(), best->box.bottomRight());
        text(p, best->box, value, 2.5);
        return;
    }
    if (sides)
        for (bool right : {anchor.x() > bounds.center().x(), anchor.x() <= bounds.center().x()})
            for (double dy : {-12., 12., 0., -24., 24.}) {
                const QRectF box(right ? bounds.right() + 6 : bounds.left() - width - 6,
                                 anchor.y() - height / 2 + dy, width, height);
                const QPointF end(right ? box.left() : box.right(), box.bottom());
                if (!area.contains(box) || overlaps(box, occupied) || crossed(box) ||
                    crossesText(anchor, end, occupied))
                    continue;
                occupied.push_back(box);
                dimensionPen(p);
                p.drawLine(anchor, end);
                arrow(p, anchor, end, dimensionColor);
                p.drawLine(QPointF(box.left(), box.bottom()), QPointF(box.right(), box.bottom()));
                text(p, box, value, 2.5);
                return;
            }
    for (bool above : {true, false})
        for (double dx : {0., -18., 18., -30., 30.})
            for (double dy : {0., 1., 2., -5., 5., -10., 10.}) {
                const QRectF box(bounds.center().x() - width / 2 + dx,
                                 (above ? bounds.top() - height - 3 : bounds.bottom() + 3) + dy,
                                 width, height);
                if (!area.contains(box) || box.intersects(bounds.adjusted(-1, -1, 1, 1)) ||
                    overlaps(box, occupied) || crossed(box))
                    continue;
                occupied.push_back(box);
                const double y = above ? box.bottom() : box.top();
                const QPointF end(box.center().x(), y);
                dimensionPen(p);
                p.drawLine(anchor, end);
                arrow(p, anchor, end, dimensionColor);
                p.drawLine(QPointF(box.left(), y), QPointF(box.right(), y));
                text(p, box, value, 2.5);
                return;
            }
    throw std::runtime_error("Bend callouts do not fit; choose another orientation");
}
void view(QPainter &p, const BendingDrawing &drawing, const DrawingView &v, QPointF center,
          double scale, const QString &caption, bool isometric = false, bool circular = false,
          bool compact = false, std::vector<QRectF> *sheetOccupied = nullptr,
          bool fullProfile = false, std::optional<QRectF> cellArea = {}, bool classical = false,
          std::vector<QLineF> *dimensionLines = nullptr) {
    const bool schematic = v.profileStart.has_value();
    const QPointF midpoint((v.minimum.x + v.maximum.x) / 2, (v.minimum.y + v.maximum.y) / 2);
    const double removed = v.omittedX ? (*v.omittedX)[1] - (*v.omittedX)[0] : 0;
    const double displayedWidth =
        (v.maximum.x - v.minimum.x - removed) * scale + (v.omittedX ? 8 : 0);
    const auto map = [&](DrawingPoint a) {
        const bool right = v.omittedX && a.x >= (*v.omittedX)[1] - 1e-8;
        const double x = a.x - (right ? removed : 0);
        return center + QPointF((x - v.minimum.x) * scale + (right ? 8 : 0) - displayedWidth / 2,
                                -(a.y - midpoint.y()) * scale);
    };
    const QRectF bounds(map({v.minimum.x, v.maximum.y}), map({v.maximum.x, v.minimum.y}));
    p.save();
    if (circular) {
        QPainterPath clip;
        clip.addEllipse(bounds);
        p.setClipPath(clip, Qt::IntersectClip);
    }
    if (!v.sectionLoops.empty()) {
        QPainterPath material;
        material.setFillRule(Qt::OddEvenFill);
        for (const auto &loop : v.sectionLoops) {
            if (loop.empty())
                continue;
            material.moveTo(map(loop.front()));
            for (const auto point : loop)
                material.lineTo(map(point));
            material.closeSubpath();
        }
        p.save();
        p.setClipPath(material, Qt::IntersectClip);
        pen(p);
        for (double x = bounds.left() - bounds.height(); x < bounds.right(); x += 3)
            p.drawLine(QPointF(x, bounds.bottom()), QPointF(x + bounds.height(), bounds.top()));
        p.restore();
    }
    // Hidden lines first, visible geometry stays legible on top.
    for (bool hidden : {true, false}) {
        pen(p, hidden ? .25 : .5, hidden ? Qt::DashLine : Qt::SolidLine);
        for (const auto &path : v.paths) {
            if (path.hidden != hidden || path.points.size() < 2)
                continue;
            QPainterPath q;
            for (std::size_t i = 1; i < path.points.size(); ++i) {
                const auto a = path.points[i - 1], b = path.points[i];
                const auto piece = [&](double low, double high) {
                    double from = 0, to = 1;
                    const double dx = b.x - a.x;
                    if (std::abs(dx) < 1e-12) {
                        if (a.x < low - 1e-8 || a.x > high + 1e-8)
                            return;
                    } else {
                        const double t0 = (low - a.x) / dx, t1 = (high - a.x) / dx;
                        from = std::max(0., std::min(t0, t1));
                        to = std::min(1., std::max(t0, t1));
                        if (to <= from + 1e-12)
                            return;
                    }
                    const auto start = map({a.x + dx * from, a.y + (b.y - a.y) * from});
                    if (q.elementCount() == 0 || QLineF(q.currentPosition(), start).length() > 1e-7)
                        q.moveTo(start);
                    q.lineTo(map({a.x + dx * to, a.y + (b.y - a.y) * to}));
                };
                if (v.omittedX) {
                    piece(v.minimum.x - 1e-5, (*v.omittedX)[0]);
                    piece((*v.omittedX)[1], v.maximum.x + 1e-5);
                } else
                    piece(-1e100, 1e100);
            }
            p.drawPath(q);
        }
    }
    pen(p, .25, Qt::DashDotLine);
    std::vector<std::pair<DrawingPoint, double>> centers;
    for (const auto &circle : v.circleCenters) {
        const auto found = std::find_if(centers.begin(), centers.end(), [&](const auto &old) {
            return std::hypot(old.first.x - circle.first.x, old.first.y - circle.first.y) < 1e-5;
        });
        if (found == centers.end())
            centers.push_back(circle);
        else
            found->second = std::max(found->second, circle.second);
    }
    for (const auto &[c, radius] : centers) {
        if (v.omittedX && c.x > (*v.omittedX)[0] && c.x < (*v.omittedX)[1])
            continue;
        const auto at = map(c);
        const double r = radius * scale + 2;
        p.drawLine(at - QPointF(r, 0), at + QPointF(r, 0));
        p.drawLine(at - QPointF(0, r), at + QPointF(0, r));
    }
    for (const auto &axes : v.openingAxes) {
        const auto at = map(axes.center);
        for (const auto end : {axes.first, axes.second}) {
            auto vector = map(end) - at;
            const double length = std::hypot(vector.x(), vector.y());
            if (length > 1e-7) {
                vector *= (length + 2) / length;
                p.drawLine(at - vector, at + vector);
            }
        }
    }
    if (v.omittedX) {
        pen(p);
        for (const double x : *v.omittedX) {
            const double px = map({x, v.minimum.y}).x();
            QPainterPath wave;
            wave.moveTo(px, bounds.top() - 1);
            const int count = std::max(2, static_cast<int>(bounds.height() / 3));
            for (int i = 0; i < count; ++i) {
                const double y = bounds.top() - 1 + (bounds.height() + 2) * i / count;
                const double step = (bounds.height() + 2) / count;
                wave.cubicTo(px + .7, y + step / 3, px - .7, y + step * 2 / 3, px, y + step);
            }
            p.drawPath(wave);
        }
    }
    p.restore();
    pen(p);
    if (circular)
        p.drawEllipse(bounds);
    for (const auto &[anchor, radius] : v.detailCircles) {
        const auto at = map(anchor);
        const double r = std::max(2.5, radius * scale);
        p.drawEllipse(at, r, r);
    }
    const bool paired = drawing.details.size() >= 2 && drawing.details[0].fullProfile &&
                        drawing.details[1].fullProfile;
    const double cell = fullProfile ? (paired ? 104 : 140) : compact ? 100 : 190;
    const QRectF captionBox =
        cellArea ? QRectF(cellArea->left(), cellArea->top(), cellArea->width(), 7)
                 : QRectF(center.x() - cell / 2, fullProfile ? 30 : center.y() - 48, cell, 7);
    const QRectF annotationArea = cellArea ? cellArea->adjusted(0, caption.isEmpty() ? 0 : 9, 0, 0)
                                  : fullProfile
                                      ? QRectF(center.x() - cell / 2, 25, cell, 185)
                                      : QRectF(center.x() - cell / 2, center.y() - 40, cell, 85);
    std::vector<QRectF> localOccupied;
    auto &occupied = sheetOccupied ? *sheetOccupied : localOccupied;
    if (!caption.isEmpty())
        occupied.push_back(captionBox);
    DimensionPlacement placement;
    const auto witnesses =
        [&](const DrawingDimension &dimension) -> std::optional<std::array<QPointF, 2>> {
        if (!dimension.witnesses)
            return {};
        return std::array{map((*dimension.witnesses)[0]), map((*dimension.witnesses)[1])};
    };
    placement.area =
        cellArea ? annotationArea
                 : annotationArea.adjusted(0, -10, 0, 7).intersected(QRectF(25, 20, 385, 193));
    std::array<int, 4> chainLanes{};
    for (const auto &d : v.axialDimensions) {
        placement.nextLane.fill(0); // Adjacent spans share one chain dimension line.
        dimension(p, map(d.start), map(d.end), bounds, d, occupied, &placement, false,
                  witnesses(d));
        for (std::size_t i = 0; i < chainLanes.size(); ++i)
            chainLanes[i] = std::max(chainLanes[i], placement.nextLane[i]);
    }
    placement.nextLane = chainLanes;
    for (const auto &d : v.holeDimensions)
        dimension(p, map(d.start), map(d.end), bounds, d, occupied, &placement, true, witnesses(d));
    for (const auto &d : v.sectionDimensions)
        dimension(p, map(d.start), map(d.end), bounds, d, occupied, &placement, true, witnesses(d));
    auto dimensions = v.dimensions;
    if (paired && fullProfile)
        std::stable_sort(dimensions.begin(), dimensions.end(),
                         [](const auto &a, const auto &b) { return a.valueMm < b.valueMm; });
    for (const auto &d : dimensions)
        dimension(p, map(d.start), map(d.end), bounds, d, occupied,
                  !cellArea && v.holeDimensions.empty() && v.sectionDimensions.empty() &&
                          !(paired && fullProfile)
                      ? nullptr
                      : &placement,
                  false, witnesses(d));
    for (const auto &angle : v.angles)
        angularDimension(p, map(angle.vertex), map(angle.first), map(angle.second), angle, scale,
                         annotationArea, occupied, fullProfile && !schematic, classical);
    for (const auto &marker : v.sectionMarkers) {
        const auto a = map(marker.first), b = map(marker.second);
        const auto delta = b - a;
        const double length = std::hypot(delta.x(), delta.y());
        QPointF look(marker.viewDirection.x, -marker.viewDirection.y);
        const double lookLength = std::hypot(look.x(), look.y());
        if (length < 1e-8 || lookLength < 1e-8)
            throw std::runtime_error("Invalid section marker");
        look /= lookLength;
        for (int endIndex = 0; endIndex < 2; ++endIndex) {
            const auto outward = delta / length * (endIndex == 0 ? -1 : 1);
            const auto end = endIndex == 0 ? a : b;
            double offset = 3;
            for (; offset <= 24; offset += 7) {
                const auto candidate = end + outward * (offset + 2.5);
                if (!crossesText(end + outward * offset, end + outward * (offset + 5), occupied) &&
                    !crossesText(candidate - look * 6, candidate, occupied))
                    break;
            }
            if (offset > 24)
                throw std::runtime_error("Section marker does not fit");
            const auto tip = end + outward * (offset + 2.5);
            pen(p, .75);
            p.drawLine(end + outward * offset, end + outward * (offset + 5));
            pen(p);
            p.drawLine(tip - look * 6, tip);
            arrow(p, tip, tip - look * 6);
            bool placed = false;
            for (double along : {3., 0., -3., 6., -6., 9., -9.}) {
                for (int lane = 0; lane < 8 && !placed; ++lane)
                    for (double side : {-1., 1.}) {
                        const auto label = tip + look * (side * (9 + lane * 7)) + outward * along;
                        const QRectF box(label.x() - 6, label.y() - 2.5, 12, 5);
                        if (!(classical ? placement.area : QRectF(25, 20, 385, 193))
                                 .contains(box) ||
                            overlaps(box, occupied) || (classical && box.intersects(bounds)))
                            continue;
                        occupied.push_back(box);
                        text(p, box, QString::fromStdString(marker.designation), 3.5);
                        placed = true;
                        break;
                    }
                if (placed)
                    break;
            }
            if (!placed)
                throw std::runtime_error("Section designation does not fit");
        }
    }
    for (const auto &[anchor, index] : v.bendNotes) {
        if (index >= drawing.bends.size())
            throw std::runtime_error("A bend callout refers to another drawing");
        const auto &bend = drawing.bends[index];
        QString value = (circular ? QString{} : bendId(drawing, index) + QStringLiteral(" · ")) +
                        QStringLiteral("R%1\nL=%2%3%4")
                            .arg(number(bend.innerRadiusMm), number(bend.axialLengthMm),
                                 bend.trimmedEnds ? QStringLiteral("!") : QString{},
                                 bend.diagnostic ? QStringLiteral(" (кандидат)") : QString{});
        if (schematic) {
            value = bendId(drawing, index);
            if (!commonBendValue(drawing, false))
                value += QStringLiteral(" · R%1").arg(number(bend.innerRadiusMm));
            if (!commonBendValue(drawing, true))
                value += QStringLiteral("\nL=%1").arg(number(bend.axialLengthMm));
            if (bend.trimmedEnds)
                value += QStringLiteral("!");
        }
        leaderNote(p, map(anchor), bounds, value,
                   cellArea || fullProfile ? annotationArea
                   : compact               ? QRectF(center.x() - 50, center.y() - 40, 100, 87)
                                           : QRectF(25, 20, 385, 193),
                   occupied, fullProfile);
    }
    for (const auto &note : v.groupedBendNotes) {
        if (note.bends.empty())
            continue;
        QStringList ids;
        for (auto index : note.bends)
            ids << bendId(drawing, index);
        const auto &bend = drawing.bends[note.bends.front()];
        QString value = ids.join(QStringLiteral(", "));
        if (!commonBendValue(drawing, false))
            value += QStringLiteral(" · R%1").arg(number(bend.innerRadiusMm));
        if (!commonBendValue(drawing, true))
            value += QStringLiteral("\nL=%1").arg(number(bend.axialLengthMm));
        if (bend.trimmedEnds)
            value += QStringLiteral("!");
        leaderNote(p, map(note.anchor), bounds, value, annotationArea, occupied, true);
    }
    if (classical) {
        std::vector<double> diameters;
        for (const auto &[holeCenter, diameter] : v.holeDiameters) {
            if (std::any_of(diameters.begin(), diameters.end(),
                            [&](auto d) { return std::abs(d - diameter) < 1e-5; }))
                continue;
            diameters.push_back(diameter);
            const auto count =
                std::count_if(v.holeDiameters.begin(), v.holeDiameters.end(),
                              [&](const auto &d) { return std::abs(d.second - diameter) < 1e-5; });
            const auto value = (count > 1 ? QStringLiteral("%1×").arg(count) : QString{}) +
                               QChar(0x2300) + number(diameter);
            leaderNote(p, map({holeCenter.x, holeCenter.y + diameter / 2}), bounds, value,
                       annotationArea, occupied, true);
        }
    }
    for (const auto &marker : v.viewArrows) {
        const auto anchor = map(marker.anchor);
        QPointF direction(marker.direction.x, -marker.direction.y);
        direction /= std::hypot(direction.x(), direction.y());
        const QPointF normal(-direction.y(), direction.x());
        bool placed = false;
        for (double lane : {0., 10., -10., 20., -20., 30., -30.}) {
            const auto tip = anchor - direction * 4 + normal * lane;
            const auto from = tip - direction * 8;
            const auto at = from - direction * 4;
            const QRectF box(at.x() - 4, at.y() - 2.5, 8, 5);
            if (!placement.area.contains(box) || overlaps(box, occupied) ||
                crossesText(tip, from, occupied))
                continue;
            occupied.push_back(box);
            pen(p);
            p.drawLine(from, tip);
            arrow(p, tip, from);
            text(p, box, QString::fromStdString(marker.designation));
            placed = true;
            break;
        }
        if (!placed)
            throw std::runtime_error("A view direction arrow does not fit");
    }
    for (const auto &note : v.directionNotes)
        leaderNote(p, map(note.anchor), bounds,
                   QStringLiteral("Оси %1/%2: %3°")
                       .arg(bendId(drawing, note.first), bendId(drawing, note.second),
                            number(note.radians * 180 / std::numbers::pi)),
                   annotationArea.adjusted(0, -10, 0, 7), occupied);
    for (const auto &[anchor, value] : v.labels) {
        const auto a = map(anchor);
        auto delta = a - bounds.center();
        const double distance = std::hypot(delta.x(), delta.y());
        delta = distance > 1e-6 ? delta / distance : QPointF(1, -1) / std::sqrt(2.);
        QPointF end;
        bool placed = false;
        for (int lane = 0; lane < 8 && !placed; ++lane)
            for (double rotation : {0., .785398163, -.785398163, 1.570796327, -1.570796327}) {
                const QPointF direction(
                    delta.x() * std::cos(rotation) - delta.y() * std::sin(rotation),
                    delta.x() * std::sin(rotation) + delta.y() * std::cos(rotation));
                end = a + direction * (12 + lane * 7);
                const QRectF box(end.x() - 6, end.y() - 5, 12, 5);
                if (overlaps(box, occupied) || box.intersects(bounds) ||
                    !(cellArea ? annotationArea : QRectF(25, 20, 385, 193)).contains(box) ||
                    crossesText(a, end, occupied))
                    continue;
                occupied.push_back(box);
                placed = true;
                break;
            }
        if (!placed) {
            // A locator inside a long narrow view may be far from its radial edge.
            // Try the same bounded external leader placement used by bend notes.
            leaderNote(p, a, bounds, QString::fromStdString(value), annotationArea, occupied, true);
            continue;
        }
        dimensionPen(p);
        p.drawLine(a, end);
        arrow(p, a, end, dimensionColor);
        if (!v.detailCircles.empty())
            p.drawLine(end - QPointF(6, 0), end + QPointF(6, 0));
        text(p, QRectF(end.x() - 6, end.y() - 5, 12, 5), QString::fromStdString(value), 3.5);
    }
    if (isometric && !classical) {
        const auto a = map(v.sideAnchor);
        auto b = map(v.sideTip);
        const auto delta = b - a;
        const double length = std::hypot(delta.x(), delta.y());
        if (length > 1e-8 && length < 12)
            b = a + delta * (12 / length);
        pen(p);
        p.drawLine(a, b);
        arrow(p, a, b);
        text(p, QRectF(b.x() - 4, b.y() - 5, 8, 5), QStringLiteral("A"), 5);
    }
    if (v.profileStart) {
        const auto a = map(v.profileStart->first), b = map(v.profileStart->second);
        const auto delta = b - a;
        const double length = std::hypot(delta.x(), delta.y());
        if (length > 1e-7) {
            p.save();
            p.setPen(Qt::NoPen);
            p.setBrush(Qt::black);
            p.drawEllipse(a, 1.1, 1.1);
            p.restore();
            const auto tip = a + delta * (std::min(8., length * .6) / length);
            arrow(p, tip, a);
        }
    }
    text(p, captionBox, caption, 3.5);
    if (dimensionLines)
        dimensionLines->insert(dimensionLines->end(), placement.lines.begin(),
                               placement.lines.end());
}
void titleBlock(QPainter &p, const DrawingTitle &t, double scale, int page, int pages) {
    p.save();
    p.translate(230, 237);
    pen(p, .5);
    p.drawRect(QRectF(0, 0, 185, 55));
    p.drawLine(QPointF(65, 0), QPointF(65, 55));
    p.drawLine(QPointF(65, 15), QPointF(185, 15));
    p.drawLine(QPointF(135, 15), QPointF(135, 55));
    p.drawLine(QPointF(65, 40), QPointF(185, 40));
    p.drawLine(QPointF(135, 20), QPointF(185, 20));
    p.drawLine(QPointF(135, 35), QPointF(185, 35));
    p.drawLine(QPointF(150, 15), QPointF(150, 35));
    p.drawLine(QPointF(167, 15), QPointF(167, 35));
    p.drawLine(QPointF(155, 35), QPointF(155, 40));
    p.drawLine(QPointF(175, 0), QPointF(175, 15));
    p.drawLine(QPointF(175, 5), QPointF(185, 5));
    pen(p);
    for (double y : {5., 10., 15., 20., 25., 30., 35., 40., 45., 50.})
        p.drawLine(QPointF(0, y), QPointF(65, y));
    for (double x : {7., 17., 40., 55.})
        p.drawLine(QPointF(x, 0), QPointF(x, 20));
    for (double x : {17., 40., 55.})
        p.drawLine(QPointF(x, 20), QPointF(x, 55));
    for (int i = 1; i < 3; ++i)
        p.drawLine(QPointF(135 + i * 5, 20), QPointF(135 + i * 5, 35));
    const std::array<QString, 5> headings{QStringLiteral("Изм."), QStringLiteral("Лист"),
                                          QStringLiteral("№ докум."), QStringLiteral("Подп."),
                                          QStringLiteral("Дата")};
    const std::array<double, 6> x{0, 7, 17, 40, 55, 65};
    for (int i = 0; i < 5; ++i)
        text(p, QRectF(x[i], 15, x[i + 1] - x[i], 5), headings[i], 2.5);
    const std::array<QString, 7> roles{QStringLiteral("Разраб."),   QStringLiteral("Пров."),
                                       QStringLiteral("Т. контр."), QString{},
                                       QStringLiteral("Н. контр."), QString{},
                                       QStringLiteral("Утв.")};
    for (int i = 0; i < 7; ++i)
        text(p, QRectF(.8, 20 + 5 * i, 16, 5), roles[i], 2.5, Qt::AlignLeft | Qt::AlignVCenter);
    text(p, QRectF(66, 1, 108, 13), t.designation, 5);
    text(p, QRectF(175, 0, 10, 5), QStringLiteral("Вер."), 2.5);
    text(p, QRectF(67, 16, 66, 23), t.name, 5, Qt::AlignCenter | Qt::TextWordWrap);
    text(p, QRectF(67, 41, 66, 13), t.material, 3.5, Qt::AlignCenter | Qt::TextWrapAnywhere);
    text(p, QRectF(135, 15, 15, 5), QStringLiteral("Лит."), 2.5);
    text(p, QRectF(150, 15, 17, 5), QStringLiteral("Масса"), 2.5);
    text(p, QRectF(167, 15, 18, 5), QStringLiteral("Масштаб"), 2.5);
    text(p, QRectF(168, 21, 16, 13), scaleName(scale), 3.5);
    text(p, QRectF(135, 35, 20, 5), QStringLiteral("Лист %1").arg(page + 1), 2.5);
    text(p, QRectF(155, 35, 30, 5), QStringLiteral("Листов %1").arg(pages), 2.5);
    p.restore();
    text(p, QRectF(365, 292.3, 48, 4.5), QStringLiteral("Формат A3"), 2.5,
         Qt::AlignRight | Qt::AlignVCenter);
    // Additional designation field 26, rotated as in form 1. Other archival fields stay blank.
    pen(p, .5);
    p.drawRect(QRectF(20, 5, 70, 14));
    p.save();
    p.translate(90, 19);
    p.rotate(180);
    text(p, QRectF(1, 1, 68, 12), t.designation, 3.5);
    p.restore();
}
QString bendId(const BendingDrawing &d, std::size_t index) {
    int n = 0;
    for (std::size_t i = 0; i <= index; ++i)
        if (d.bends[i].diagnostic == d.bends[index].diagnostic)
            ++n;
    return QStringLiteral("%1%2")
        .arg(d.bends[index].diagnostic ? QLatin1Char('D') : QLatin1Char('B'))
        .arg(n, 2, 10, QLatin1Char('0'));
}
bool inlineProfiles(const BendingDrawing &d) {
    return drawingProfilesCoverBends(d);
}
std::optional<std::size_t> openingView(const BendingDrawing &d) {
    std::optional<std::size_t> best;
    for (std::size_t i : {1u, 2u})
        if (!d.views[i].holeDimensions.empty() &&
            (!best || d.views[i].holeDimensions.size() > d.views[*best].holeDimensions.size()))
            best = i;
    return best;
}
std::size_t profileCapacity(const BendingDrawing &d) {
    if (d.details.size() >= 2 && d.details[0].fullProfile && d.details[1].fullProfile)
        return 2;
    return !d.details.empty() && d.details.front().fullProfile ? 3 : 4;
}
bool inOperatorProfile(const BendingDrawing &d, std::size_t bend) {
    return std::any_of(d.profiles.begin(), d.profiles.end(), [&](const auto &profile) {
        return std::find(profile.bends.begin(), profile.bends.end(), bend) != profile.bends.end();
    });
}
std::vector<std::size_t> remainingDetails(const BendingDrawing &d) {
    std::vector<std::size_t> result;
    for (std::size_t i = 0; i < d.details.size(); ++i)
        if (d.details[i].view.bendNotes.empty() ||
            std::any_of(d.details[i].view.bendNotes.begin(), d.details[i].view.bendNotes.end(),
                        [&](const auto &note) {
                            if (!inOperatorProfile(d, note.second))
                                return true;
                            if (d.details[i].fullProfile)
                                return false;
                            for (std::size_t j = 0; j < d.profiles.size(); ++j) {
                                const auto &profile = d.profiles[j];
                                if (std::find(profile.bends.begin(), profile.bends.end(),
                                              note.second) == profile.bends.end())
                                    continue;
                                const bool single = j / 2 * 2 + 1 == d.profiles.size();
                                const double scale =
                                    fittedScale(profile.view, single ? 165 : 125, 80);
                                const auto &bend = d.bends[note.second];
                                return (bend.innerRadiusMm + bend.thicknessMm) * scale < 1.5;
                            }
                            return false;
                        }))
            result.push_back(i);
    return result;
}
std::size_t remainingCapacity(const BendingDrawing &d) {
    const auto details = remainingDetails(d);
    return std::all_of(details.begin(), details.end(),
                       [&](auto i) { return !d.details[i].fullProfile; })
               ? 4
               : 2;
}
bool referencePage(const BendingDrawing &d) {
    const auto holes = std::count_if(d.views.begin(), d.views.begin() + 3,
                                     [](const auto &v) { return !v.holeDimensions.empty(); });
    const auto coverage = drawingMeasurementCoverage(d);
    return holes > (d.profiles.size() == 1 ? 2 : 0) ||
           std::any_of(d.bends.begin(), d.bends.end(),
                       [](const auto &b) { return b.diagnostic; }) ||
           coverage.dimensioned < coverage.total;
}
int operatorPages(const BendingDrawing &d) {
    return static_cast<int>((d.profiles.size() + 1) / 2);
}
// Reference views retain their holes and overall dimensions. The schematic removes only
// closed internal wires; neither the source body nor these views are replaced by it.
DrawingView referenceView(const DrawingView &source) {
    auto result = source;
    result.bendNotes.clear();
    result.labels.clear();
    result.detailCircles.clear();
    result.sectionMarkers.clear();
    result.angles.clear();
    return result;
}
double fittedScale(const DrawingView &v, double width, double height) {
    double w = v.maximum.x - v.minimum.x;
    if (v.omittedX)
        w -= (*v.omittedX)[1] - (*v.omittedX)[0];
    const auto fit = standardDrawingScale(
        std::min((width - (v.omittedX ? 8 : 0)) / w, height / (v.maximum.y - v.minimum.y)));
    if (!fit)
        throw std::runtime_error("The view does not fit the sheet");
    return *fit;
}
bool largeOpeningView(const BendingDrawing &d) {
    return d.profiles.empty() && d.bends.empty() && d.views[0].holeDimensions.size() > 4;
}
void openingSheet(QPainter &p, const BendingDrawing &d, const DrawingTitle &title) {
    QString parameters =
        d.thicknessMm ? QStringLiteral("t = %1 мм%2")
                            .arg(number(*d.thicknessMm),
                                 d.manualThickness ? QStringLiteral(" (вручную)") : QString{})
                      : QStringLiteral("Толщина не определена");
    if (d.thicknessMm && d.declaredThicknessMm && !d.manualThickness &&
        std::abs(*d.thicknessMm - *d.declaredThicknessMm) > 1e-5)
        parameters +=
            QStringLiteral(" · вручную t = %1 (расхождение)").arg(number(*d.declaredThicknessMm));
    if (!title.execution.trimmed().isEmpty())
        parameters += QStringLiteral(" · ") + title.execution.trimmed();
    text(p, QRectF(25, 24, 385, 8), parameters, 3.5);
    for (std::size_t i = 0; i < 4; ++i) {
        const QRectF cell =
            i == 0 ? QRectF(25, 37, 240, 174) : QRectF(275, 37 + (i - 1) * 59, 135, 56);
        const auto &v = d.views[i];
        const auto scale = fittedScale(v, i == 0 ? 140 : 80, i == 0 ? 95 : 25);
        const QStringList names{
            d.reverseView ? QStringLiteral("Обратная сторона A") : QStringLiteral("Сторона A"),
            v.reverseProjection ? QStringLiteral("Вид снизу") : QStringLiteral("Вид сверху"),
            v.reverseProjection ? QStringLiteral("Вид справа") : QStringLiteral("Вид слева"),
            QStringLiteral("Общий вид")};
        view(p, d, v, cell.center() + QPointF(0, 4), scale,
             names[static_cast<int>(i)] + QStringLiteral(" · ") + scaleName(scale), i == 3, false,
             false, nullptr, false, cell);
    }
}
void operatorSheet(QPainter &p, const BendingDrawing &d, const DrawingTitle &title, int page) {
    QStringList parameters;
    if (d.thicknessMm)
        parameters << QStringLiteral("t = %1 мм%2")
                          .arg(number(*d.thicknessMm),
                               d.manualThickness ? QStringLiteral(" (вручную)") : QString{});
    if (d.thicknessMm && d.declaredThicknessMm && !d.manualThickness &&
        std::abs(*d.thicknessMm - *d.declaredThicknessMm) > 1e-5)
        parameters
            << QStringLiteral("Вручную t = %1 (расхождение)").arg(number(*d.declaredThicknessMm));
    if (const auto r = commonBendValue(d, false))
        parameters << QStringLiteral("Ri = %1 мм").arg(number(*r));
    if (const auto length = commonBendValue(d, true))
        parameters << QStringLiteral("Длина зон гиба L = %1 мм").arg(number(*length));
    else
        parameters << QStringLiteral("Длина зоны L — у каждого гиба");
    if (!title.execution.trimmed().isEmpty())
        parameters << title.execution.trimmed();
    text(p, QRectF(25, 24, 385, 8), parameters.join(QStringLiteral("     ")), 3.5);
    const auto iso = [&](QRectF cell, std::size_t first, std::size_t count) {
        auto v = referenceView(d.views[3]);
        for (std::size_t i = first; i < first + count; ++i)
            v.labels.push_back({d.profiles[i].locator, d.profiles[i].designation});
        const auto scale = fittedScale(v, cell.width() - 50, cell.height() - 20);
        view(p, d, v, cell.center() + QPointF(0, 4), scale,
             QStringLiteral("Общий вид · %1").arg(scaleName(scale)), true, false, false, nullptr,
             false, cell);
    };
    if (page < operatorPages(d)) {
        const auto first = static_cast<std::size_t>(page) * 2;
        const auto count = std::min(std::size_t{2}, d.profiles.size() - first);
        for (std::size_t i = 0; i < count; ++i) {
            const auto &profile = d.profiles[first + i];
            auto v = profile.view;
            for (const auto &segment : profile.segments) {
                auto dimension = segment.dimension;
                dimension.label.clear();
                v.dimensions.push_back(std::move(dimension));
            }
            const QRectF cell =
                count == 1 ? QRectF(25, 37, 230, 125) : QRectF(25 + 195 * i, 37, 190, 124);
            const auto scale = fittedScale(v, cell.width() - 65, 80);
            view(p, d, v, cell.center() + QPointF(0, 5), scale,
                 QStringLiteral("%1 · наружные размеры · %2")
                     .arg(QString::fromStdString(profile.designation), scaleName(scale)),
                 false, false, false, nullptr, true, cell);
        }
        if (count == 1) {
            iso(QRectF(25, 164, 230, 47), first, count);
            std::array<std::size_t, 3> order{0, 1, 2};
            std::stable_sort(order.begin(), order.end(), [&](auto a, auto b) {
                return d.views[a].holeDimensions.size() > d.views[b].holeDimensions.size();
            });
            for (std::size_t i = 0; i < 2; ++i) {
                const auto index = order[i];
                const auto v = referenceView(d.views[index]);
                const QRectF cell(260, 37 + 88 * i, 150, 85);
                const auto scale = fittedScale(v, cell.width() - 58, cell.height() - 46);
                const QStringList names{d.reverseView ? QStringLiteral("Обратная сторона A")
                                                      : QStringLiteral("Сторона A"),
                                        v.reverseProjection ? QStringLiteral("Вид снизу")
                                                            : QStringLiteral("Вид сверху"),
                                        v.reverseProjection ? QStringLiteral("Вид справа")
                                                            : QStringLiteral("Вид слева")};
                view(p, d, v, cell.center() + QPointF(0, 5), scale,
                     names[static_cast<int>(index)] + QStringLiteral(" · ") + scaleName(scale),
                     false, false, false, nullptr, false, cell);
            }
        } else
            iso(QRectF(25, 164, 385, 47), first, count);
    } else if (referencePage(d) && page == operatorPages(d)) {
        for (std::size_t i = 0; i < 4; ++i) {
            const QRectF cell(25 + (i / 2) * 195, 37 + (i % 2) * 87, 190, 85);
            auto v = d.views[i];
            v.labels.clear();
            v.detailCircles.clear();
            v.sectionMarkers.clear();
            const auto scale = fittedScale(v, 125, 40);
            view(p, d, v, cell.center() + QPointF(0, 5), scale,
                 QStringLiteral("%1 · %2").arg(
                     QStringList{QStringLiteral("Сторона A"), QStringLiteral("Вид сверху"),
                                 QStringLiteral("Вид слева"),
                                 QStringLiteral("Общий вид")}[static_cast<int>(i)],
                     scaleName(scale)),
                 i == 3, false, false, nullptr, false, cell);
        }
    } else {
        const auto details = remainingDetails(d);
        const auto capacity = remainingCapacity(d);
        const auto first =
            static_cast<std::size_t>(page - operatorPages(d) - (referencePage(d) ? 1 : 0)) *
            capacity;
        for (std::size_t i = first; i < std::min(first + capacity, details.size()); ++i) {
            const auto &detail = d.details[details[i]];
            const auto position = i - first;
            const QRectF cell(25 + (capacity == 4 ? position / 2 : position) * 195,
                              37 + (capacity == 4 ? position % 2 * 87 : 0), 190,
                              capacity == 4 ? 85 : 174);
            const auto scale =
                fittedScale(detail.view, capacity == 4 ? 60 : 120, capacity == 4 ? 40 : 100);
            view(p, d, detail.view, cell.center() + QPointF(0, 4), scale,
                 QStringLiteral("%1 · %2 · %3")
                     .arg(QString::fromStdString(detail.designation), bendId(d, detail.bendIndex),
                          scaleName(scale)),
                 false, !detail.fullProfile, false, nullptr, detail.fullProfile, cell);
        }
    }
}
QString flatMaterial(core::SheetMaterial material) {
    switch (material) {
    case core::SheetMaterial::MildSteel:
        return QStringLiteral("Низкоуглеродистая сталь");
    case core::SheetMaterial::StainlessSteel:
        return QStringLiteral("Нержавеющая сталь");
    case core::SheetMaterial::Aluminium:
        return QStringLiteral("Алюминий");
    case core::SheetMaterial::Unknown:
        return QStringLiteral("Материал не задан");
    }
    return {};
}
double flatView(QPainter &p, const BendingDrawing &d, QRectF cell, int attempt = 0,
                DrawingFlatSide side = DrawingFlatSide::Automatic,
                std::optional<double> requestedScale = {}) {
    const auto &flat = *d.flatPattern;
    const auto &info = flat.info;
    auto v = flat.view;
    const auto orientation = DrawingSheet::flatOrientation(info, side);
    const auto reflect = [&](DrawingPoint point) {
        return orientation.reversed ? DrawingPoint{v.maximum.x - point.x, point.y} : point;
    };
    if (orientation.reversed) {
        for (auto &path : v.paths)
            for (auto &point : path.points)
                point = reflect(point);
        for (auto *dimensions :
             {&v.dimensions, &v.holeDimensions, &v.sectionDimensions, &v.axialDimensions})
            for (auto &dimension : *dimensions) {
                dimension.start = reflect(dimension.start);
                dimension.end = reflect(dimension.end);
                if (dimension.witnesses)
                    for (auto &point : *dimension.witnesses)
                        point = reflect(point);
            }
        for (auto &axes : v.openingAxes) {
            axes.center = reflect(axes.center);
            axes.first = reflect(axes.first);
            axes.second = reflect(axes.second);
        }
        for (auto &label : v.labels)
            label.first = reflect(label.first);
        for (auto &circle : v.circleCenters)
            circle.first = reflect(circle.first);
    }
    if (info.bends.empty() || v.paths.empty() || !(v.maximum.x > 0) || !(v.maximum.y > 0))
        throw std::runtime_error("Invalid flat drawing snapshot");
    const auto area = cell.adjusted(0, 14, 0, 0);
    const double fit =
        std::min({1., (area.width() - 70) / v.maximum.x, (area.height() - 42) / v.maximum.y}) *
        std::pow(.8, attempt);
    const auto scale = requestedScale ? requestedScale : standardDrawingScale(fit);
    if (!scale)
        throw std::runtime_error("Flat pattern does not fit the sheet");
    if (!std::isfinite(*scale) || *scale <= 0 || v.maximum.x * *scale > area.width() - 20 ||
        v.maximum.y * *scale > area.height() - 20)
        throw std::runtime_error("Развёртка не помещается в выбранном масштабе");
    const auto center = area.center();
    text(p, QRectF(cell.left(), cell.top(), cell.width(), 7),
         QStringLiteral("Развёртка · %1 · %2")
             .arg(scaleName(*scale),
                  orientation.sideA ? (*orientation.sideA ? QStringLiteral("сторона A")
                                                          : QStringLiteral("сторона B"))
                                    : (orientation.reversed ? QStringLiteral("обратная сторона")
                                                            : QStringLiteral("исходная сторона"))),
         3.5);
    const bool sameK = std::all_of(info.bends.begin(), info.bends.end(), [&](const auto &b) {
        return std::abs(b.k - info.bends.front().k) < 1e-7;
    });
    const auto k = sameK ? QStringLiteral("K=%1").arg(number(info.bends.front().k))
                         : QStringLiteral("K по Ri/t");
    text(p, QRectF(cell.left(), cell.top() + 7, cell.width(), 6),
         QStringLiteral("Теоретическая · %1 · t%2 · %3 · %4")
             .arg(flatMaterial(info.settings.material), number(info.thicknessMm), k,
                  info.settings.manualK ? QStringLiteral("вручную")
                                        : QStringLiteral("Wiz Parts v1")),
         2.5);
    std::vector<QRectF> occupied;
    std::vector<QLineF> dimensionLines;
    view(p, d, v, center, *scale, {}, false, false, false, &occupied, false, area, true,
         &dimensionLines);
    const auto mapView = [&](DrawingPoint point) {
        return center +
               QPointF((point.x - v.maximum.x / 2) * *scale, -(point.y - v.maximum.y / 2) * *scale);
    };
    const auto map = [&](DrawingPoint point) { return mapView(reflect(point)); };
    const QRectF bounds(mapView({0, v.maximum.y}), mapView({v.maximum.x, 0}));
    auto bends = info.bends;
    std::sort(bends.begin(), bends.end(), [](const auto &a, const auto &b) {
        const auto aa = a.centreLine(), bb = b.centreLine();
        const double ay = aa[0].y + aa[1].y, by = bb[0].y + bb[1].y;
        return ay != by ? ay > by : aa[0].x < bb[0].x;
    });
    for (const auto &b : bends) {
        const auto line = b.centreLine();
        const auto a = map({line[0].x, line[0].y}), end = map({line[1].x, line[1].y});
        pen(p, .25, Qt::DashDotLine);
        p.drawLine(a, end);
    }
    auto groups = flat.repeatedBendGroups;
    if (groups.empty())
        for (const auto &b : bends)
            groups.push_back({b.bendIndex});
    for (auto group : groups) {
        if (group.empty())
            throw std::runtime_error("Empty repeated bend group");
        const auto found = std::find_if(bends.begin(), bends.end(), [&](const auto &b) {
            return b.bendIndex == group.front();
        });
        if (found == bends.end())
            throw std::runtime_error("Invalid repeated bend group");
        const auto &b = *found;
        const auto line = b.centreLine();
        const auto a = map({line[0].x, line[0].y}), end = map({line[1].x, line[1].y});
        std::sort(group.begin(), group.end());
        QStringList ids;
        for (auto id : group)
            ids << QStringLiteral("%1").arg(id + 1, 2, 10, QLatin1Char('0'));
        const auto value =
            QStringLiteral("B%1 %2 %3°\nL=%4%5")
                .arg(ids.join(QStringLiteral(",")))
                .arg((b.direction > 0) != orientation.reversed ? QStringLiteral("ВВЕРХ")
                                                               : QStringLiteral("ВНИЗ"),
                     number(180 - b.turnRadians * 180 / std::numbers::pi), number(b.lengthMm()),
                     sameK ? QString{} : QStringLiteral(" · K=%1").arg(number(b.k)));
        leaderNote(p, (a + end) / 2, bounds, value, area, occupied, true, &dimensionLines);
    }
    return *scale;
}
void compactOrientation(QPainter &p, const BendingDrawing &drawing) {
    // The source projection is already prepared in the CAD worker. Leave only
    // visible contours: this small view is an orientation aid, not a measured view.
    DrawingView orientation;
    const auto &source = drawing.views[3];
    orientation.minimum = source.minimum;
    orientation.maximum = source.maximum;
    for (const auto &path : source.paths)
        if (!path.hidden)
            orientation.paths.push_back(path);
    if (orientation.paths.empty())
        throw std::runtime_error("Missing isometric orientation view");
    const double scale = fittedScale(orientation, 175, 27);
    view(p, drawing, orientation, {122.5, 273}, scale,
         QStringLiteral("Наглядный вид (%1)").arg(scaleName(scale)), false, false, false, nullptr,
         false, QRectF(25, 251, 195, 38), true);
}
bool classicalLayout(const BendingDrawing &d, const DrawingTitle &title) {
    return title.layout == DrawingLayout::Classical && d.classicalViews &&
           d.variableProfile != DrawingVariableProfile::Measured;
}
double classicalLeftWidth(const BendingDrawing &d, bool flat = false) {
    if (flat || std::any_of(d.sections.begin(), d.sections.end(),
                            [](const auto &s) { return s.repeatCount != 0; }))
        return 183;
    return d.sections.size() == 1 ? 183 : 238;
}
double sectionScale(const BendingDrawing &d, std::size_t index) {
    if (d.sections[index].repeatCount)
        return fittedScale(d.sections[index].view, 162, 70);
    if (d.sections.size() == 2 && d.sections[1].repeatCount)
        return fittedScale(d.sections[index].view, 162, 35);
    const bool single = d.sections.size() == 1;
    return fittedScale(d.sections[index].view, index < 2 ? (single ? 162 : 87) : 130,
                       index < 2 ? (single ? 139 : 54) : 130);
}
std::vector<std::size_t> classicalDetails(const BendingDrawing &d) {
    std::vector<std::size_t> result;
    for (std::size_t i = 0; i < d.details.size(); ++i) {
        bool needed = d.details[i].view.bendNotes.empty();
        for (const auto &note : d.details[i].view.bendNotes) {
            bool covered = false;
            for (std::size_t s = 0; s < d.sections.size(); ++s)
                if (std::find(d.sections[s].view.dimensionedBends.begin(),
                              d.sections[s].view.dimensionedBends.end(),
                              note.second) != d.sections[s].view.dimensionedBends.end()) {
                    covered = true;
                    const auto &b = d.bends[note.second];
                    if (!d.sections[s].repeatCount && !d.details[i].fullProfile &&
                        (b.innerRadiusMm + b.thicknessMm) * sectionScale(d, s) < 1.5)
                        needed = true;
                }
            needed = needed || !covered;
        }
        if (needed)
            result.push_back(i);
    }
    return result;
}
std::size_t classicalDetailCapacity(const BendingDrawing &d) {
    const auto details = classicalDetails(d);
    return std::all_of(details.begin(), details.end(),
                       [&](auto i) { return !d.details[i].fullProfile; })
               ? 4
               : 2;
}
DrawingView classicalSource(const BendingDrawing &d, const DrawingView &source,
                            std::size_t projection) {
    auto view = source;
    const auto &original = d.views[projection];
    if (source.reverseProjection != original.reverseProjection)
        return view;
    for (auto index : classicalDetails(d)) {
        const auto &name = d.details[index].designation;
        for (const auto &label : original.labels) {
            if (label.second != name ||
                std::any_of(view.labels.begin(), view.labels.end(),
                            [&](const auto &old) { return old.second == name; }))
                continue;
            view.labels.push_back(label);
            for (const auto &circle : original.detailCircles)
                if (std::hypot(circle.first.x - label.first.x, circle.first.y - label.first.y) <
                    1e-5)
                    view.detailCircles.push_back(circle);
        }
    }
    return view;
}
int sectionPages(const BendingDrawing &d) {
    return static_cast<int>((d.sections.size() > 2 ? d.sections.size() - 2 : 0) + 1) / 2;
}
int classicalPages(const BendingDrawing &d) {
    const auto capacity = classicalDetailCapacity(d);
    return 1 + sectionPages(d) + static_cast<int>(d.auxiliaryViews.size()) +
           static_cast<int>((classicalDetails(d).size() + capacity - 1) / capacity);
}
double classicalTopMargin(const BendingDrawing &d) {
    const auto count = (*d.classicalViews)[1].holeDimensions.size();
    return count >= 4 ? 36 : count ? 26 : 20;
}
double classicalFrontHeight(const BendingDrawing &d) {
    const auto &f = (*d.classicalViews)[0], &t = (*d.classicalViews)[1];
    const double fh = f.maximum.y - f.minimum.y, th = t.maximum.y - t.minimum.y;
    const double desired = 44 + (170 - 44 - classicalTopMargin(d)) * fh / (fh + th);
    return std::clamp(desired, 80., t.holeDimensions.size() >= 6 ? 100. : 134.);
}
double classicalScale(const BendingDrawing &d, bool flat = false) {
    const auto &f = (*d.classicalViews)[0], &t = (*d.classicalViews)[1];
    return std::min({1.,
                     fittedScale(f, classicalLeftWidth(d, flat) - 65, classicalFrontHeight(d) - 44),
                     fittedScale(t, classicalLeftWidth(d, flat) - 65,
                                 170 - classicalFrontHeight(d) - classicalTopMargin(d))});
}
void classicalParameters(QPainter &p, const BendingDrawing &d, const DrawingTitle &title,
                         QRectF area = {25, 24, 385, 8}) {
    QStringList values;
    if (d.thicknessMm)
        values << QStringLiteral("t = %1 мм%2")
                      .arg(number(*d.thicknessMm),
                           d.manualThickness ? QStringLiteral(" (вручную)") : QString{});
    if (const auto r = commonBendValue(d, false))
        values << QStringLiteral("Ri = %1 мм").arg(number(*r));
    if (const auto l = commonBendValue(d, true))
        values << QStringLiteral("Длина зон гиба L = %1 мм").arg(number(*l));
    else if (!d.bends.empty())
        values << QStringLiteral("L — у соответствующих гибов");
    if (d.thicknessMm && d.declaredThicknessMm && !d.manualThickness &&
        std::abs(*d.thicknessMm - *d.declaredThicknessMm) > 1e-5)
        values
            << QStringLiteral("Вручную t = %1 (расхождение)").arg(number(*d.declaredThicknessMm));
    if (!title.execution.trimmed().isEmpty())
        values << title.execution.trimmed();
    text(p, area, values.join(QStringLiteral("     ")), 3.5);
}
double classicalSheet(QPainter &p, const BendingDrawing &d, const DrawingTitle &title, int page,
                      bool flat = false) {
    classicalParameters(p, d, title);
    std::string lastError;
    double scaleLimit = classicalScale(d, flat);
    for (int attempt = 0; attempt < 5; ++attempt) {
        const double scale = standardDrawingScale(scaleLimit).value_or(scaleLimit);
        const auto draw = [&](QPainter &trial) {
            std::vector<QRectF> occupied;
            const auto section = [&](std::size_t index, QRectF cell) {
                const auto &s = d.sections[index];
                const double localScale =
                    standardDrawingScale(
                        std::min(sectionScale(d, index),
                                 flat && index < 2
                                     ? fittedScale(s.view, cell.width() - 40, cell.height() - 34)
                                     : sectionScale(d, index)) *
                        std::pow(.8, attempt))
                        .value_or(sectionScale(d, index));
                const auto name = QString::fromStdString(s.designation);
                const auto caption =
                    QStringLiteral("%1 (%2)%3%4")
                        .arg(s.repeatCount ? QStringLiteral("Волна сечения %1-%1 · повторений: %2")
                                                 .arg(name)
                                                 .arg(s.repeatCount)
                                           : QStringLiteral("%1-%1").arg(name),
                             scaleName(localScale),
                             s.locations.size() > 1
                                 ? QStringLiteral(" · %1 места").arg(s.locations.size())
                                 : QString{},
                             s.rotated ? QStringLiteral(" · повёрнуто") : QString{});
                view(trial, d, s.view, cell.center() + QPointF(0, 6), localScale, caption, false,
                     false, false, &occupied, true, cell, true);
            };
            if (page == 0) {
                const double leftWidth = classicalLeftWidth(d, flat);
                const double frontHeight = classicalFrontHeight(d);
                const QRectF front(25, 37, leftWidth, frontHeight),
                    top(25, 42 + frontHeight, leftWidth, 170 - frontHeight);
                // The front and top share U, scale and horizontal origin. The left view
                // shares V, scale and vertical origin with the front (first-angle layout).
                view(trial, d, classicalSource(d, (*d.classicalViews)[0], 0), front.center(), scale,
                     {}, false, false, false, &occupied, false, front, true);
                view(trial, d, classicalSource(d, (*d.classicalViews)[1], 1), top.center(), scale,
                     {}, false, false, false, &occupied, false, top, true);
                const double x = 25 + leftWidth + 10, width = 410 - x;
                if (flat) {
                    flatView(trial, d, QRectF(x, 37, width, 102), attempt);
                    const double sectionWidth = width / static_cast<double>(d.sections.size());
                    for (std::size_t i = 0; i < d.sections.size(); ++i)
                        section(i, QRectF(x + i * sectionWidth, 147, sectionWidth - 3, 70));
                } else if (d.sections.empty()) {
                    const QRectF left(x, 37, width, frontHeight);
                    view(trial, d, classicalSource(d, (*d.classicalViews)[2], 2),
                         {left.center().x(), front.center().y()}, scale, {}, false, false, false,
                         &occupied, false, left, true);
                } else if (d.sections.size() == 1)
                    section(0, QRectF(x, 37, width, 175));
                else if (d.sections[1].repeatCount) {
                    section(0, QRectF(x, 37, width, 65));
                    section(1, QRectF(x, 108, width, 109));
                } else {
                    section(0, QRectF(x, 37, width, 87.5));
                    section(1, QRectF(x, 129.5, width, 87.5));
                }
            } else if (page <= sectionPages(d)) {
                const auto first = 2 + static_cast<std::size_t>(page - 1) * 2;
                for (std::size_t i = first; i < std::min(first + 2, d.sections.size()); ++i)
                    section(i, QRectF(25 + (i - first) * 195, 37, 190, 175));
            } else if (page <= sectionPages(d) + static_cast<int>(d.auxiliaryViews.size())) {
                const auto &v =
                    d.auxiliaryViews[static_cast<std::size_t>(page - sectionPages(d) - 1)];
                const double localScale = fittedScale(v.view, 280, 110) * std::pow(.8, attempt);
                const double fit = standardDrawingScale(localScale).value_or(localScale);
                view(trial, d, classicalSource(d, v.view, v.projection), {217.5, 128}, fit,
                     QStringLiteral("Вид %1 (%2)")
                         .arg(QString::fromStdString(v.designation), scaleName(fit)),
                     false, false, false, &occupied, false, QRectF(25, 37, 385, 175), true);
            } else {
                const auto details = classicalDetails(d);
                const auto capacity = classicalDetailCapacity(d);
                const auto first =
                    static_cast<std::size_t>(page - sectionPages(d) -
                                             static_cast<int>(d.auxiliaryViews.size()) - 1) *
                    capacity;
                for (std::size_t i = first; i < std::min(first + capacity, details.size()); ++i) {
                    const auto &detail = d.details[details[i]];
                    const auto position = i - first;
                    const QRectF cell(25 + (capacity == 4 ? position / 2 : position) * 195,
                                      37 + (capacity == 4 ? position % 2 * 87 : 0), 190,
                                      capacity == 4 ? 85 : 175);
                    const double localScale = fittedScale(detail.view, capacity == 4 ? 60 : 120,
                                                          capacity == 4 ? 40 : 130) *
                                              std::pow(.8, attempt);
                    const double fit = standardDrawingScale(localScale).value_or(localScale);
                    view(trial, d, detail.view, cell.center() + QPointF(0, 4), fit,
                         QStringLiteral("%1 · %2 (%3)")
                             .arg(QString::fromStdString(detail.designation),
                                  bendId(d, detail.bendIndex), scaleName(fit)),
                         false, !detail.fullProfile, false, &occupied, detail.fullProfile, cell,
                         true);
                }
            }
        };
        // Test placement without recording/replaying device-DPI-dependent painter commands.
        // Both passes use the same millimetre geometry and fixed paper text heights.
        QImage scratch(420, 297, QImage::Format_RGB32);
        QPainter trial(&scratch);
        try {
            draw(trial);
            trial.end();
            draw(p);
            return scale;
        } catch (const std::runtime_error &e) {
            trial.end();
            lastError = e.what();
            scaleLimit = scale * .8;
        }
    }
    throw std::runtime_error(lastError);
}
bool inlineFlat(const BendingDrawing &d, const DrawingTitle &title) {
    if (std::any_of(d.sections.begin(), d.sections.end(),
                    [](const auto &s) { return s.repeatCount; }))
        return false;
    if (!d.flatPattern || !classicalLayout(d, title) || d.sections.empty() || d.sections.size() > 2)
        return false;
    QImage scratch(420, 297, QImage::Format_RGB32);
    QPainter painter(&scratch);
    try {
        classicalSheet(painter, d, title, 0, true);
        return true;
    } catch (const std::runtime_error &) {
        return false;
    }
}
// Compact only complete, validated arrangements. No measured view is dropped to save a page.
enum class SupplementKind { Section, Auxiliary, Detail, Flat };
struct Supplement {
    SupplementKind kind;
    std::size_t index = 0;
};
struct PackedSupplement {
    Supplement item;
    QRectF cell;
    double scale;
};
struct ClassicalPacking {
    bool onePage = false;
    bool legacyFlat = false;
    double leftWidth = 0, mainScale = 0;
    std::vector<PackedSupplement> items;
};
void packedView(QPainter &p, const BendingDrawing &d, const PackedSupplement &packed,
                std::vector<QRectF> &occupied) {
    const auto [item, cell, scale] = packed;
    if (item.kind == SupplementKind::Flat) {
        flatView(p, d, cell);
        occupied.push_back(cell);
        return;
    }
    DrawingView v;
    QString caption;
    bool full = false, circular = false;
    if (item.kind == SupplementKind::Section) {
        const auto &s = d.sections[item.index];
        v = s.view;
        full = true;
        const auto name = QString::fromStdString(s.designation);
        caption =
            QStringLiteral("%1 (%2)%3%4")
                .arg(s.repeatCount ? QStringLiteral("Волна сечения %1-%1 · повторений: %2")
                                         .arg(name)
                                         .arg(s.repeatCount)
                                   : QStringLiteral("%1-%1").arg(name),
                     scaleName(scale),
                     s.locations.size() > 1 ? QStringLiteral(" · %1 места").arg(s.locations.size())
                                            : QString{},
                     s.rotated ? QStringLiteral(" · повёрнуто") : QString{});
    } else if (item.kind == SupplementKind::Auxiliary) {
        const auto &a = d.auxiliaryViews[item.index];
        v = classicalSource(d, a.view, a.projection);
        caption = QStringLiteral("Вид %1 (%2)")
                      .arg(QString::fromStdString(a.designation), scaleName(scale));
    } else {
        const auto &detail = d.details[item.index];
        v = detail.view;
        full = detail.fullProfile;
        circular = !full;
        caption = QStringLiteral("%1 · %2 (%3)")
                      .arg(QString::fromStdString(detail.designation), bendId(d, detail.bendIndex),
                           scaleName(scale));
    }
    // The unscaled text and its leaders must fit too, not just the geometry bounds.
    double width = v.maximum.x - v.minimum.x;
    if (v.omittedX)
        width -= (*v.omittedX)[1] - (*v.omittedX)[0];
    if (width * scale + (v.omittedX ? 8 : 0) > cell.width() - 12 ||
        (v.maximum.y - v.minimum.y) * scale > cell.height() - 18)
        throw std::runtime_error("The measured view is too large for the compact cell");
    view(p, d, v, cell.center() + QPointF(0, 4), scale, caption, false, circular, false, &occupied,
         full, cell, true);
    occupied.push_back(cell);
}
std::vector<std::vector<QRectF>> packingCells(QRectF area, std::size_t count) {
    std::vector<std::vector<QRectF>> result;
    const auto grid = [](QRectF a, int columns, int rows) {
        std::vector<QRectF> cells;
        const double width = (a.width() - 6 * (columns - 1)) / columns;
        const double height = (a.height() - 6 * (rows - 1)) / rows;
        for (int row = 0; row < rows; ++row)
            for (int column = 0; column < columns; ++column)
                cells.emplace_back(a.left() + column * (width + 6), a.top() + row * (height + 6),
                                   width, height);
        return cells;
    };
    result.push_back(grid(area, 1, static_cast<int>(count)));
    if (count > 1)
        result.push_back(grid(area, 2, static_cast<int>((count + 1) / 2)));
    if (count > 2)
        result.push_back(grid(area, 3, static_cast<int>((count + 2) / 3)));
    // One large view plus a grid of small, genuinely enlarged details.
    if (count >= 2)
        for (bool horizontal : {true, false})
            for (double fraction : {.5, .4, .6, .65, .35})
                for (bool largeFirst : {true, false}) {
                    const double span = (horizontal ? area.height() : area.width()) - 6;
                    const double first = span * fraction, second = span - first;
                    const QRectF a(area.left(), area.top(), horizontal ? area.width() : first,
                                   horizontal ? first : area.height());
                    const QRectF b(horizontal ? area.left() : area.left() + first + 6,
                                   horizontal ? area.top() + first + 6 : area.top(),
                                   horizontal ? area.width() : second,
                                   horizontal ? second : area.height());
                    const int columns = (horizontal && count > 2) || count > 4 ? 2 : 1;
                    auto cells = grid(largeFirst ? b : a, columns,
                                      static_cast<int>((count - 2 + columns) / columns));
                    cells.insert(cells.begin(), largeFirst ? a : b);
                    result.push_back(std::move(cells));
                }
    return result;
}
std::optional<std::vector<PackedSupplement>>
packSupplements(const BendingDrawing &d, const std::vector<Supplement> &items, QRectF area) {
    if (items.empty() || items.size() > 8)
        return {};
    std::vector<double> auxiliaryScale(items.size());
    for (std::size_t i = 0; i < items.size(); ++i)
        if (items[i].kind == SupplementKind::Auxiliary) {
            const double preferred = fittedScale(d.auxiliaryViews[items[i].index].view, 280, 110);
            auxiliaryScale[i] = preferred;
            // Compare with the successful normal sheet, including its annotation lanes.
            for (int attempt = 0; attempt < 5; ++attempt) {
                QImage scratch(420, 297, QImage::Format_RGB32);
                QPainter painter(&scratch);
                std::vector<QRectF> occupied;
                const double scale =
                    standardDrawingScale(preferred * std::pow(.8, attempt)).value_or(preferred);
                try {
                    packedView(painter, d, {items[i], QRectF(25, 37, 385, 175), scale}, occupied);
                    auxiliaryScale[i] = scale;
                    break;
                } catch (const std::runtime_error &) {
                }
            }
        }
    for (const auto &cells : packingCells(area, items.size())) {
        std::vector<std::vector<std::optional<double>>> fits(
            items.size(), std::vector<std::optional<double>>(cells.size()));
        for (std::size_t i = 0; i < items.size(); ++i)
            for (std::size_t c = 0; c < cells.size(); ++c) {
                QImage scratch(420, 297, QImage::Format_RGB32);
                QPainter painter(&scratch);
                std::vector<QRectF> occupied;
                try {
                    const auto item = items[i];
                    double scale = 0, preferred = 0, minimum = 0;
                    if (item.kind == SupplementKind::Flat) {
                        scale = flatView(painter, d, cells[c]);
                        const auto &v = d.flatPattern->view;
                        minimum = standardDrawingScale(
                                      std::min({1., 122 / v.maximum.x, 46 / v.maximum.y}))
                                      .value_or(1.);
                    } else {
                        const auto &v = item.kind == SupplementKind::Section
                                            ? d.sections[item.index].view
                                        : item.kind == SupplementKind::Auxiliary
                                            ? d.auxiliaryViews[item.index].view
                                            : d.details[item.index].view;
                        if (item.kind == SupplementKind::Section) {
                            preferred = minimum = sectionScale(d, item.index);
                            double readable = 0;
                            for (auto b : v.dimensionedBends) {
                                const auto &bend = d.bends.at(b);
                                readable = std::max({readable,
                                                     1.5 / (bend.innerRadiusMm + bend.thicknessMm),
                                                     .75 / bend.thicknessMm});
                            }
                            if (readable > 0 && !d.sections[item.index].repeatCount)
                                minimum = std::min(preferred, readable);
                        } else if (item.kind == SupplementKind::Auxiliary) {
                            preferred = auxiliaryScale[i];
                            minimum = preferred * .5;
                        } else {
                            const bool small = classicalDetailCapacity(d) == 4;
                            preferred = fittedScale(v, small ? 60 : 120, small ? 40 : 130);
                            const auto &bend = d.bends.at(d.details[item.index].bendIndex);
                            minimum = std::min(
                                preferred,
                                std::max({1., 1.5 / (bend.innerRadiusMm + bend.thicknessMm),
                                          .75 / bend.thicknessMm}));
                        }
                        scale = std::min(preferred, fittedScale(v, cells[c].width() - 40,
                                                                cells[c].height() - 34));
                        if (scale + 1e-8 < minimum)
                            continue;
                        bool placed = false;
                        for (int attempt = 0; attempt < 5 && scale + 1e-8 >= minimum; ++attempt) {
                            occupied.clear();
                            try {
                                packedView(painter, d, {item, cells[c], scale}, occupied);
                                placed = true;
                                break;
                            } catch (const std::runtime_error &) {
                                scale = standardDrawingScale(scale * .8).value_or(0.);
                            }
                        }
                        if (!placed)
                            continue;
                    }
                    if (scale + 1e-8 >= minimum)
                        fits[i][c] = scale;
                } catch (const std::runtime_error &) {
                    // Try another paper rectangle, never suppress a dimension or lower its font.
                }
            }
        std::vector<int> owner(cells.size(), -1);
        const auto match = [&](auto &&self, std::size_t item, std::vector<bool> &seen) -> bool {
            // Preserve the familiar numbering order when equally sized cells all fit.
            for (bool reassign : {false, true})
                for (std::size_t c = 0; c < cells.size(); ++c)
                    if (fits[item][c] && !seen[c] && (owner[c] >= 0) == reassign) {
                        seen[c] = true;
                        if (owner[c] < 0 || self(self, static_cast<std::size_t>(owner[c]), seen)) {
                            owner[c] = static_cast<int>(item);
                            return true;
                        }
                    }
            return false;
        };
        bool complete = true;
        for (std::size_t i = 0; i < items.size() && complete; ++i) {
            std::vector<bool> seen(cells.size());
            complete = match(match, i, seen);
        }
        if (complete) {
            std::vector<PackedSupplement> result;
            for (std::size_t c = 0; c < cells.size(); ++c)
                if (owner[c] >= 0) {
                    const auto i = static_cast<std::size_t>(owner[c]);
                    result.push_back({items[i], cells[c], *fits[i][c]});
                }
            QImage scratch(420, 297, QImage::Format_RGB32);
            QPainter painter(&scratch);
            std::vector<QRectF> occupied;
            try {
                for (const auto &packed : result)
                    packedView(painter, d, packed, occupied);
                return result;
            } catch (const std::runtime_error &) {
            }
        }
    }
    return {};
}
void packedPrimary(QPainter &p, const BendingDrawing &d, double width, double scale,
                   std::vector<QRectF> &occupied) {
    const double height = classicalFrontHeight(d);
    const auto &views = *d.classicalViews;
    if (scale > std::min(fittedScale(views[0], width - 65, height - 44),
                         fittedScale(views[1], width - 65, 170 - height - classicalTopMargin(d))) +
                    1e-8)
        throw std::runtime_error("Compaction would reduce the main projection scale");
    const QRectF front(25, 37, width, height), top(25, 42 + height, width, 170 - height);
    view(p, d, classicalSource(d, (*d.classicalViews)[0], 0), front.center(), scale, {}, false,
         false, false, &occupied, false, front, true);
    occupied.push_back(front);
    view(p, d, classicalSource(d, (*d.classicalViews)[1], 1), top.center(), scale, {}, false, false,
         false, &occupied, false, top, true);
    occupied.push_back(top);
}
std::optional<ClassicalPacking> compactClassical(const BendingDrawing &d, const DrawingTitle &t) {
    if (!classicalLayout(d, t))
        return {};
    const bool flat = inlineFlat(d, t);
    const int legacyPages = classicalPages(d) + (d.flatPattern && !flat ? 1 : 0);
    if (legacyPages <= 1)
        return {};
    std::vector<Supplement> all, remainder;
    for (std::size_t i = 0; i < d.sections.size(); ++i) {
        all.push_back({SupplementKind::Section, i});
        if (i >= 2)
            remainder.push_back(all.back());
    }
    for (std::size_t i = 0; i < d.auxiliaryViews.size(); ++i) {
        all.push_back({SupplementKind::Auxiliary, i});
        remainder.push_back(all.back());
    }
    for (auto i : classicalDetails(d)) {
        all.push_back({SupplementKind::Detail, i});
        remainder.push_back(all.back());
    }
    if (d.flatPattern) {
        all.push_back({SupplementKind::Flat, 0});
        if (!flat)
            remainder.push_back(all.back());
    }
    const double scale = standardDrawingScale(classicalScale(d, flat)).value_or(1.);
    std::vector<double> widths;
    if (!d.sections.empty()) {
        widths.push_back(classicalLeftWidth(d, flat));
        if (widths.front() != 183.)
            widths.push_back(183.);
    }
    for (double width : widths) {
        QImage scratch(420, 297, QImage::Format_RGB32);
        QPainter painter(&scratch);
        std::vector<QRectF> occupied;
        try {
            packedPrimary(painter, d, width, scale, occupied);
            if (auto items = packSupplements(d, all, QRectF(35 + width, 37, 375 - width, 175))) {
                for (const auto &item : *items)
                    packedView(painter, d, item, occupied);
                return ClassicalPacking{true, flat, width, scale, std::move(*items)};
            }
        } catch (const std::runtime_error &) {
        }
    }
    if (legacyPages > 2)
        if (auto items = packSupplements(d, remainder, QRectF(25, 37, 385, 175))) {
            QImage scratch(420, 297, QImage::Format_RGB32);
            QPainter painter(&scratch);
            std::vector<QRectF> occupied;
            try {
                for (const auto &item : *items)
                    packedView(painter, d, item, occupied);
                return ClassicalPacking{false, flat, 0, scale, std::move(*items)};
            } catch (const std::runtime_error &) {
            }
        }
    return {};
}

int sourcePages(const BendingDrawing &d, const DrawingTitle &title) {
    if (classicalLayout(d, title))
        return classicalPages(d);
    if (!d.profiles.empty())
        return operatorPages(d) + (referencePage(d) ? 1 : 0) +
               static_cast<int>((remainingDetails(d).size() + remainingCapacity(d) - 1) /
                                remainingCapacity(d));
    const auto capacity = profileCapacity(d);
    const auto inlineCount = inlineProfiles(d) ? std::min(capacity, d.details.size()) : 0;
    return 1 + static_cast<int>((d.details.size() - inlineCount + capacity - 1) / capacity);
}
} // namespace
namespace drawing {
std::vector<std::size_t> requiredDetails(const BendingDrawing &d) {
    return classicalDetails(d);
}
DrawingView preparedView(const BendingDrawing &d, DrawingViewKey key) {
    switch (key.kind) {
    case DrawingViewKind::Projection:
        return d.classicalViews ? classicalSource(d, d.classicalViews->at(key.index), key.index)
                                : d.views.at(key.index);
    case DrawingViewKind::Section:
        return d.sections.at(key.index).view;
    case DrawingViewKind::Profile: {
        const auto &profile = d.profiles.at(key.index);
        auto v = profile.view;
        for (const auto &segment : profile.segments) {
            auto dimension = segment.dimension;
            dimension.label.clear();
            v.dimensions.push_back(std::move(dimension));
        }
        return v;
    }
    case DrawingViewKind::Detail:
        return d.details.at(key.index).view;
    case DrawingViewKind::Auxiliary: {
        const auto &v = d.auxiliaryViews.at(key.index);
        return classicalSource(d, v.view, v.projection);
    }
    case DrawingViewKind::Flat:
        if (!d.flatPattern)
            throw std::runtime_error("Развёртка не рассчитана");
        return d.flatPattern->view;
    case DrawingViewKind::Isometric: {
        DrawingView v;
        v.minimum = d.views[3].minimum;
        v.maximum = d.views[3].maximum;
        for (const auto &path : d.views[3].paths)
            if (!path.hidden)
                v.paths.push_back(path);
        return v;
    }
    }
    throw std::runtime_error("Unknown view kind");
}
void paintPlaced(QPainter &p, const BendingDrawing &d, const DrawingPlacedView &at,
                 DrawingFlatSide side, std::vector<QRectF> &occupied) {
    if (at.key.kind == DrawingViewKind::Flat) {
        flatView(p, d, at.area, 0, side, at.scale);
        return;
    }
    const auto v = preparedView(d, at.key);
    double width = v.maximum.x - v.minimum.x;
    if (v.omittedX)
        width -= (*v.omittedX)[1] - (*v.omittedX)[0];
    if (width * at.scale + (v.omittedX ? 8 : 0) > at.area.width() - 12 ||
        (v.maximum.y - v.minimum.y) * at.scale > at.area.height() - 14)
        throw std::runtime_error("Вид выходит за границы области");
    const bool full =
        at.key.kind == DrawingViewKind::Section || at.key.kind == DrawingViewKind::Profile ||
        (at.key.kind == DrawingViewKind::Detail && d.details.at(at.key.index).fullProfile);
    view(p, d, v, at.centre, at.scale,
         DrawingSheet::viewName(d, at.key) + QStringLiteral(" (%1)").arg(scaleName(at.scale)),
         at.key.kind == DrawingViewKind::Isometric, at.key.kind == DrawingViewKind::Detail && !full,
         false, &occupied, full, at.area, at.key.kind != DrawingViewKind::Profile);
}
void paintCompositionFrame(QPainter &p, const BendingDrawing &d, const DrawingTitle &title,
                           const DrawingSheetPlan &plan, int page) {
    p.setRenderHint(QPainter::Antialiasing);
    p.fillRect(QRectF(0, 0, DrawingSheet::widthMm, DrawingSheet::heightMm), Qt::white);
    pen(p, .5);
    p.drawRect(QRectF(20, 5, 395, 287));
    double scale = 1;
    for (const auto &v : plan.views)
        if (v.page == page && v.key.kind != DrawingViewKind::Isometric) {
            scale = v.scale;
            break;
        }
    for (const auto &v : plan.views)
        if (v.page == page && v.key.kind == DrawingViewKind::Projection && v.key.index == 0)
            scale = v.scale;
    titleBlock(p, title, scale, page, plan.pages);
    classicalParameters(p, d, title, {96, 7, 312, 8});
    if (title.quantity)
        text(p, QRectF(25, 237, 195, 12), QStringLiteral("Количество: %1 шт.").arg(*title.quantity),
             3.5, Qt::AlignLeft | Qt::AlignTop);
}
} // namespace drawing
int DrawingSheet::pageCount(const BendingDrawing &d, const DrawingTitle &title) {
    if (title.plan) {
        if (title.plan->source.get() != &d)
            throw std::runtime_error("Устаревшая компоновка");
        return title.plan->pages;
    }
    if (const auto compact = compactClassical(d, title))
        return compact->onePage ? 1 : 2;
    return sourcePages(d, title) + (d.flatPattern && !inlineFlat(d, title) ? 1 : 0);
}
DrawingMeasurementCoverage DrawingSheet::coverage(const BendingDrawing &d,
                                                  const DrawingTitle &title) {
    if (title.plan) {
        if (title.plan->source.get() != &d)
            throw std::runtime_error("Устаревшая компоновка");
        return title.plan->coverage;
    }
    return drawingMeasurementCoverage(d, classicalLayout(d, title));
}
void DrawingSheet::paint(QPainter &p, const BendingDrawing &d, const DrawingTitle &t, int page) {
    if (t.plan) {
        drawing::paintComposition(p, d, t, page);
        return;
    }
    const auto packing = compactClassical(d, t);
    const bool flatInline = packing ? packing->legacyFlat : inlineFlat(d, t);
    const int totalPages = packing ? (packing->onePage ? 1 : 2)
                                   : sourcePages(d, t) + (d.flatPattern && !flatInline ? 1 : 0);
    const auto scale = drawingScale(d);
    if (!scale || page < 0 || page >= totalPages)
        throw std::runtime_error("Invalid drawing layout");
    p.setRenderHint(QPainter::Antialiasing);
    p.fillRect(QRectF(0, 0, widthMm, heightMm), Qt::white);
    pen(p, .5);
    p.drawRect(QRectF(20, 5, 395, 287));
    const double mainScale =
        largeOpeningView(d) ? fittedScale(d.views[0], 140, 95)
        : d.profiles.empty()
            ? *scale
            : fittedScale(d.profiles[0].view, d.profiles.size() == 1 ? 165 : 125, 80);
    const bool flatPage = !packing && d.flatPattern && !flatInline && page == sourcePages(d, t);
    const bool flatShown =
        packing ? ((packing->onePage || page > 0)
                       ? std::any_of(packing->items.begin(), packing->items.end(),
                                     [](const auto &item) {
                                         return item.item.kind == SupplementKind::Flat;
                                     })
                       : packing->legacyFlat)
                : flatPage || (flatInline && page == 0);
    const bool classical = classicalLayout(d, t);
    if (!classical && !flatPage)
        titleBlock(p, t, mainScale, page, totalPages);
    text(p, QRectF(96, 5, 312, 8),
         flatPage    ? QStringLiteral("ТЕОРЕТИЧЕСКАЯ РАЗВЁРТКА")
         : page == 0 ? QStringLiteral("ЛИСТ ГИБКИ")
         : packing   ? QStringLiteral("ПРОДОЛЖЕНИЕ ЛИСТА ГИБКИ")
                     : QStringLiteral("ПРОФИЛИ СГИБОВ"),
         5);
    const auto coverage = DrawingSheet::coverage(d, t);
    text(p, QRectF(96, 13, 312, 6),
         QStringLiteral("Размеры справочные, мм · измерено гибов %1/%2")
             .arg(coverage.dimensioned)
             .arg(coverage.total),
         3.5);
    std::vector<QRectF> occupied;
    const auto reserveCaption = [&](QPointF center, double width, bool full = false) {
        occupied.emplace_back(center.x() - width / 2, full ? 30 : center.y() - 48, width, 7);
    };
    const auto isoView = [&](QPointF center, double width, double height) {
        const auto &iso = d.views[3];
        const double fit = std::min(width / (iso.maximum.x - iso.minimum.x),
                                    height / (iso.maximum.y - iso.minimum.y));
        const double isoScale = standardDrawingScale(std::min(*scale, fit)).value_or(*scale);
        view(p, d, iso, center, isoScale, QStringLiteral("Общий вид · %1").arg(scaleName(isoScale)),
             true, false, false, &occupied);
    };
    const auto drawDetail = [&](std::size_t index, QPointF center, bool compact) {
        const auto &detail = d.details[index];
        const auto &v = detail.view;
        const auto detailScale = standardDrawingScale(
            std::min((detail.fullProfile ? 70. : 60.) / (v.maximum.x - v.minimum.x),
                     (detail.fullProfile ? 115. : 60.) / (v.maximum.y - v.minimum.y)));
        if (!detailScale)
            throw std::runtime_error("An enlarged profile does not fit the sheet");
        const auto caption =
            detail.fullProfile
                ? QStringLiteral("%1-%1 (%2)")
                      .arg(QString::fromStdString(detail.designation), scaleName(*detailScale))
                : QStringLiteral("%1 · %2 (%3)")
                      .arg(QString::fromStdString(detail.designation), bendId(d, detail.bendIndex),
                           scaleName(*detailScale));
        view(p, d, v, center, *detailScale, caption, false, !detail.fullProfile, compact, &occupied,
             detail.fullProfile);
    };
    if (packing) {
        if (packing->onePage || page > 0)
            classicalParameters(p, d, t);
        std::vector<QRectF> placed;
        double actualScale = packing->mainScale;
        if (packing->onePage)
            packedPrimary(p, d, packing->leftWidth, packing->mainScale, placed);
        else if (page == 0)
            actualScale = classicalSheet(p, d, t, 0, packing->legacyFlat);
        if (packing->onePage || page > 0)
            for (const auto &item : packing->items)
                packedView(p, d, item, placed);
        titleBlock(p, t, actualScale, page, totalPages);
    } else if (flatPage) {
        double flatScale = 0;
        std::string error;
        for (int attempt = 0; attempt < 5; ++attempt) {
            QImage scratch(420, 297, QImage::Format_RGB32);
            QPainter trial(&scratch);
            try {
                flatScale = flatView(trial, d, QRectF(25, 37, 385, 175), attempt);
                trial.end();
                flatView(p, d, QRectF(25, 37, 385, 175), attempt);
                break;
            } catch (const std::runtime_error &e) {
                flatScale = 0;
                error = e.what();
            }
        }
        if (!flatScale)
            throw std::runtime_error(error);
        titleBlock(p, t, flatScale, page, totalPages);
    } else if (classical) {
        const auto actualScale = classicalSheet(p, d, t, page, flatInline && page == 0);
        titleBlock(p, t, actualScale, page, totalPages);
    } else if (largeOpeningView(d) && page == 0) {
        openingSheet(p, d, t);
    } else if (!d.profiles.empty()) {
        operatorSheet(p, d, t, page);
        if (page < operatorPages(d))
            text(p, QRectF(25, 215, 385, 6),
                 QStringLiteral("Профиль условный; отверстия — на видах. Точка и стрелка — начало "
                                "обхода, не порядок гибки."),
                 2.5, Qt::AlignLeft | Qt::AlignVCenter);
    } else if (page == 0) {
        const bool compact = inlineProfiles(d);
        const bool tall = profileCapacity(d) == 3;
        const bool paired = profileCapacity(d) == 2;
        const std::array<QPointF, 4> centers =
            paired ? std::array<QPointF, 4>{{{244, 115}, {358, 115}, {360, 165}, {248, 165}}}
            : tall ? std::array<QPointF, 4>{{{245, 115}, {360, 70}, {360, 165}, {248, 165}}}
                   : std::array<QPointF, 4>{{{248, 70}, {358, 70}, {248, 165}, {358, 165}}};
        if (compact) {
            const auto auxiliary = openingView(d);
            reserveCaption({102, 78}, 190);
            reserveCaption(auxiliary ? QPointF(102, 170) : QPointF(102, 166), 190);
            for (std::size_t i = 0; i < std::min(profileCapacity(d), d.details.size()); ++i)
                reserveCaption(centers[i], d.details[i].fullProfile ? (paired ? 104 : 140) : 100,
                               d.details[i].fullProfile);
            view(p, d, d.views[0], {102, 78}, *scale,
                 d.reverseView ? QStringLiteral("Обратная сторона A")
                               : QStringLiteral("Вид со стороны A"),
                 false, false, false, &occupied);
            if (auxiliary) {
                view(p, d, d.views[*auxiliary], {102, 170}, *scale,
                     *auxiliary == 1
                         ? (d.views[1].reverseProjection ? QStringLiteral("Отверстия · вид снизу")
                                                         : QStringLiteral("Отверстия · вид сверху"))
                         : (d.views[2].reverseProjection ? QStringLiteral("Отверстия · вид справа")
                                                         : QStringLiteral("Отверстия · вид слева")),
                     false, false, false, &occupied);
                if (d.details.size() < profileCapacity(d)) {
                    reserveCaption(centers[d.details.size()], 100);
                    isoView(centers[d.details.size()], 75, 55);
                }
            } else
                isoView({102, 166}, 135, 65);
            for (std::size_t i = 0; i < std::min(profileCapacity(d), d.details.size()); ++i)
                drawDetail(i, centers[i], true);
        } else {
            for (const auto center :
                 std::array<QPointF, 4>{{{128, 72}, {128, 174}, {328, 72}, {328, 172}}})
                reserveCaption(center, 190);
            view(p, d, d.views[0], {128, 72}, *scale,
                 d.reverseView ? QStringLiteral("Обратная сторона A")
                               : QStringLiteral("Вид со стороны A"),
                 false, false, false, &occupied);
            view(p, d, d.views[1], {128, 174}, *scale,
                 d.views[1].reverseProjection ? QStringLiteral("Вид снизу")
                                              : QStringLiteral("Вид сверху"),
                 false, false, false, &occupied);
            view(p, d, d.views[2], {328, 72}, *scale,
                 d.views[2].reverseProjection ? QStringLiteral("Вид справа")
                                              : QStringLiteral("Вид слева"),
                 false, false, false, &occupied);
            isoView({328, 172}, 150, 65);
        }
        QString thickness =
            d.thicknessMm
                ? QStringLiteral("t = %1 мм%2")
                      .arg(number(*d.thicknessMm),
                           d.manualThickness ? QStringLiteral(" (задано вручную)") : QString{})
                : QStringLiteral("Толщина не определена");
        if (d.thicknessMm && d.declaredThicknessMm && !d.manualThickness &&
            std::abs(*d.thicknessMm - *d.declaredThicknessMm) > 1e-5)
            thickness +=
                QStringLiteral(" · вручную %1 (расхождение)").arg(number(*d.declaredThicknessMm));
        if (!t.execution.trimmed().isEmpty())
            thickness += QStringLiteral(" · ") + t.execution.trimmed();
        text(p, QRectF(25, 216, 380, 6), thickness, 3.5, Qt::AlignLeft | Qt::AlignVCenter);
        if (totalPages > 1)
            text(p, QRectF(25, 285, 195, 6),
                 QStringLiteral("Продолжение профилей: лист 2-%1").arg(totalPages), 2.5,
                 Qt::AlignLeft | Qt::AlignVCenter);
    } else {
        const std::size_t first = (inlineProfiles(d) ? profileCapacity(d) : 0) +
                                  static_cast<std::size_t>(page - 1) * profileCapacity(d);
        const std::array<QPointF, 4> centers =
            profileCapacity(d) == 2
                ? std::array<QPointF, 4>{{{128, 115}, {328, 115}, {128, 115}, {328, 115}}}
            : profileCapacity(d) == 3
                ? std::array<QPointF, 4>{{{128, 115}, {328, 70}, {328, 165}, {128, 165}}}
                : std::array<QPointF, 4>{{{128, 70}, {328, 70}, {128, 165}, {328, 165}}};
        for (std::size_t i = first; i < std::min(first + profileCapacity(d), d.details.size()); ++i)
            reserveCaption(centers[i - first], d.details[i].fullProfile ? 140 : 190,
                           d.details[i].fullProfile);
        for (std::size_t i = first; i < std::min(first + profileCapacity(d), d.details.size()); ++i)
            drawDetail(i, centers[i - first], false);
    }
    if (!d.bends.empty())
        text(p, QRectF(25, 222, 380, 6),
             QStringLiteral("Углы — внутренние, между стенками. L — осевой размер зоны гиба."), 2.5,
             Qt::AlignLeft | Qt::AlignVCenter);
    if (d.flatPattern && flatShown)
        text(p, QRectF(25, 218, 385, 4),
             QStringLiteral(
                 "Развёртка: размеры от края заготовки, не X упора. ВВЕРХ/ВНИЗ — от показанной "
                 "стороны; без калибровки станка."),
             2.5, Qt::AlignLeft | Qt::AlignVCenter);
    pen(p, .5);
    p.drawRect(QRectF(25, 237, 195, 13));
    p.drawLine(QPointF(160, 237), QPointF(160, 250));
    text(p, QRectF(27, 237, 130, 13), QStringLiteral("Количество, шт."), 3.5,
         Qt::AlignLeft | Qt::AlignVCenter);
    text(p, QRectF(161, 237, 58, 13),
         t.quantity ? QString::number(*t.quantity) : QStringLiteral("-"), 5);
    // Keep the folded shape visible beside continuation profiles and a separate blank too.
    if (classical || d.variableProfile == DrawingVariableProfile::Measured || flatPage)
        compactOrientation(p, d);
}
QImage DrawingSheet::preview(const BendingDrawing &d, const DrawingTitle &t, int page, int width) {
    QImage image(width, static_cast<int>(width * heightMm / widthMm), QImage::Format_RGB32);
    QPainter p(&image);
    p.scale(width / widthMm, width / widthMm);
    paint(p, d, t, page);
    return image;
}
QString DrawingSheet::writePdf(const QString &path, const BendingDrawing &d,
                               const DrawingTitle &t) {
    if ((t.quantity && (*t.quantity < 1 || *t.quantity > 1000000)) || !drawingScale(d))
        return QStringLiteral("Некорректное количество или размеры листа");
    QSaveFile file(path);
    if (!file.open(QIODevice::WriteOnly))
        return file.errorString();
    try {
        {
            QPdfWriter writer(&file);
            writer.setPageSize(QPageSize(QSizeF(widthMm, heightMm), QPageSize::Millimeter));
            writer.setPageMargins(QMarginsF(0, 0, 0, 0), QPageLayout::Millimeter);
            writer.setResolution(254); // Exactly 10 device units / mm.
            writer.setTitle(t.name);
            writer.setCreator(QStringLiteral("3D Bending Simulator"));
            QPainter painter(&writer);
            if (!painter.isActive())
                return QStringLiteral("Не удалось начать запись PDF");
            painter.scale(writer.resolution() / 25.4, writer.resolution() / 25.4);
            for (int page = 0; page < pageCount(d, t); ++page) {
                if (page && !writer.newPage())
                    return QStringLiteral("Не удалось создать страницу PDF");
                paint(painter, d, t, page);
            }
            if (!painter.end())
                return QStringLiteral("Не удалось завершить PDF");
        }
        if (!file.commit())
            return file.errorString();
    } catch (const std::exception &e) {
        return QString::fromUtf8(e.what());
    }
    return {};
}
} // namespace bending::ui
