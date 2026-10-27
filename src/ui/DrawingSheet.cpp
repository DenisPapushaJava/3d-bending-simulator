#include "bending/ui/DrawingSheet.h"
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
QString number(double value) {
    if (std::abs(value) < .0005)
        value = 0;
    QString result = QString::number(value, 'f', 2);
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
void arrow(QPainter &p, QPointF tip, QPointF from) {
    const auto delta = from - tip;
    const double length = std::hypot(delta.x(), delta.y());
    if (length < 1e-9)
        return;
    const auto d = delta / length;
    const QPointF n(-d.y(), d.x());
    p.save();
    p.setBrush(Qt::black);
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
               bool holePosition = false) {
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
    const QString value = (d.label.empty() || d.label.starts_with("H")
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
    for (int side = 0; side < (holePosition ? 2 : 1) && !found; ++side) {
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
                QLineF(a + normal * .8, da + normal * 2), QLineF(b + normal * .8, db + normal * 2)};
            for (int position = 0; position < 2; ++position) {
                label = outside ? (position == 0 ? db + direction * (textWidth / 2 + 5)
                                                 : da - direction * (textWidth / 2 + 5))
                                : (da + db) / 2;
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
                if (!outside)
                    break;
            }
        }
    }
    if (!found)
        throw std::runtime_error(
            QStringLiteral("Не удалось разместить размер %1. Измените ориентацию.")
                .arg(value)
                .toStdString());
    pen(p);
    p.drawLine(a + normal * .8, da + normal * 2);
    p.drawLine(b + normal * .8, db + normal * 2);
    p.drawLine(da - direction * (outside ? 6 : 0), db + direction * (outside ? 6 : 0));
    arrow(p, da, outside ? da - direction * 4 : db);
    arrow(p, db, outside ? db + direction * 4 : da);
    p.save();
    p.translate(label);
    p.rotate(angle);
    text(p, QRectF(-textWidth / 2, -5, textWidth, 4.5), value);
    p.restore();
}
void angularDimension(QPainter &p, QPointF vertex, QPointF first, QPointF second,
                      const DrawingAngle &angle, double scale, QRectF area,
                      std::vector<QRectF> &occupied, bool fullProfile = false) {
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
        pen(p);
        p.drawLine(vertex, at(start, radius + 2));
        p.drawLine(vertex, at(start + sweep, radius + 2));
        QPainterPath arc;
        arc.moveTo(at(start, radius));
        for (int i = 1; i <= 64; ++i)
            arc.lineTo(at(start + sweep * i / 64, radius));
        p.drawPath(arc);
        arrow(p, at(start, radius), at(start + sweep * .08, radius));
        arrow(p, at(start + sweep, radius), at(start + sweep * .92, radius));
        text(p, box, value, fullProfile ? 2.5 : 3.5);
        return;
    }
    throw std::runtime_error("An angular dimension does not fit; choose another orientation");
}
void leaderNote(QPainter &p, QPointF anchor, QRectF bounds, const QString &value, QRectF area,
                std::vector<QRectF> &occupied, bool sides = false) {
    const auto lines = value.split(QLatin1Char('\n'));
    double width = 18;
    for (const auto &line : lines)
        width = std::max(width, line.size() * 1.6 + 2);
    const double height = lines.size() * 4.2 + 1;
    if (sides)
        for (bool right : {anchor.x() > bounds.center().x(), anchor.x() <= bounds.center().x()})
            for (double dy : {-12., 12., 0., -24., 24.}) {
                const QRectF box(right ? bounds.right() + 6 : bounds.left() - width - 6,
                                 anchor.y() - height / 2 + dy, width, height);
                const QPointF end(right ? box.left() : box.right(), box.bottom());
                if (!area.contains(box) || overlaps(box, occupied) ||
                    crossesText(anchor, end, occupied))
                    continue;
                occupied.push_back(box);
                pen(p);
                p.drawLine(anchor, end);
                arrow(p, anchor, end);
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
                    overlaps(box, occupied))
                    continue;
                occupied.push_back(box);
                const double y = above ? box.bottom() : box.top();
                const QPointF end(box.center().x(), y);
                pen(p);
                p.drawLine(anchor, end);
                arrow(p, anchor, end);
                p.drawLine(QPointF(box.left(), y), QPointF(box.right(), y));
                text(p, box, value, 2.5);
                return;
            }
    throw std::runtime_error("Bend callouts do not fit; choose another orientation");
}
void view(QPainter &p, const BendingDrawing &drawing, const DrawingView &v, QPointF center,
          double scale, const QString &caption, bool isometric = false, bool circular = false,
          bool compact = false, std::vector<QRectF> *sheetOccupied = nullptr,
          bool fullProfile = false) {
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
    const double cell = fullProfile ? 140 : compact ? 100 : 190;
    const QRectF captionBox(center.x() - cell / 2, fullProfile ? 30 : center.y() - 48, cell, 7);
    const QRectF annotationArea = fullProfile
                                      ? QRectF(center.x() - 70, 25, 140, 185)
                                      : QRectF(center.x() - cell / 2, center.y() - 40, cell, 85);
    std::vector<QRectF> localOccupied;
    auto &occupied = sheetOccupied ? *sheetOccupied : localOccupied;
    occupied.push_back(captionBox);
    DimensionPlacement placement;
    placement.area = annotationArea.adjusted(0, -10, 0, 7).intersected(QRectF(25, 20, 385, 193));
    for (const auto &d : v.holeDimensions)
        dimension(p, map(d.start), map(d.end), bounds, d, occupied, &placement, true);
    for (const auto &d : v.dimensions)
        dimension(p, map(d.start), map(d.end), bounds, d, occupied,
                  v.holeDimensions.empty() ? nullptr : &placement);
    for (const auto &angle : v.angles)
        angularDimension(p, map(angle.vertex), map(angle.first), map(angle.second), angle, scale,
                         annotationArea, occupied, fullProfile);
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
                        if (!QRectF(25, 20, 385, 193).contains(box) || overlaps(box, occupied))
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
        const QString value =
            (circular ? QString{} : bendId(drawing, index) + QStringLiteral(" · ")) +
            QStringLiteral("R%1\nL=%2%3%4")
                .arg(number(bend.innerRadiusMm), number(bend.axialLengthMm),
                     bend.trimmedEnds ? QStringLiteral("!") : QString{},
                     bend.diagnostic ? QStringLiteral(" (кандидат)") : QString{});
        leaderNote(p, map(anchor), bounds, value,
                   fullProfile ? annotationArea
                   : compact   ? QRectF(center.x() - 50, center.y() - 40, 100, 87)
                               : QRectF(25, 20, 385, 193),
                   occupied, fullProfile);
    }
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
                    !QRectF(25, 20, 385, 193).contains(box) || crossesText(a, end, occupied))
                    continue;
                occupied.push_back(box);
                placed = true;
                break;
            }
        if (!placed)
            throw std::runtime_error("Bend labels do not fit; choose another orientation");
        pen(p);
        p.drawLine(a, end);
        arrow(p, a, end);
        if (!v.detailCircles.empty())
            p.drawLine(end - QPointF(6, 0), end + QPointF(6, 0));
        text(p, QRectF(end.x() - 6, end.y() - 5, 12, 5), QString::fromStdString(value), 3.5);
    }
    if (isometric) {
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
    text(p, captionBox, caption, 3.5);
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
std::size_t profileCapacity(const BendingDrawing &d) {
    return !d.details.empty() && d.details.front().fullProfile ? 3 : 4;
}
} // namespace
int DrawingSheet::pageCount(const BendingDrawing &d) {
    const auto capacity = profileCapacity(d);
    const auto inlineCount = inlineProfiles(d) ? std::min(capacity, d.details.size()) : 0;
    return 1 + static_cast<int>((d.details.size() - inlineCount + capacity - 1) / capacity);
}
void DrawingSheet::paint(QPainter &p, const BendingDrawing &d, const DrawingTitle &t, int page) {
    const auto scale = drawingScale(d);
    if (!scale || page < 0 || page >= pageCount(d))
        throw std::runtime_error("Invalid drawing layout");
    p.setRenderHint(QPainter::Antialiasing);
    p.fillRect(QRectF(0, 0, widthMm, heightMm), Qt::white);
    pen(p, .5);
    p.drawRect(QRectF(20, 5, 395, 287));
    titleBlock(p, t, *scale, page, pageCount(d));
    text(p, QRectF(96, 5, 312, 8),
         page == 0 ? QStringLiteral("ЛИСТ ГИБКИ") : QStringLiteral("ПРОФИЛИ СГИБОВ"), 5);
    text(p, QRectF(96, 13, 312, 6), QStringLiteral("Размеры для справок, мм. Не утверждён."), 3.5);
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
    if (page == 0) {
        const bool compact = inlineProfiles(d);
        const bool tall = profileCapacity(d) == 3;
        const std::array<QPointF, 4> centers =
            tall ? std::array<QPointF, 4>{{{245, 115}, {360, 70}, {360, 165}, {248, 165}}}
                 : std::array<QPointF, 4>{{{248, 70}, {358, 70}, {248, 165}, {358, 165}}};
        if (compact) {
            reserveCaption({102, 78}, 190);
            reserveCaption({102, 166}, 190);
            for (std::size_t i = 0; i < std::min(profileCapacity(d), d.details.size()); ++i)
                reserveCaption(centers[i], d.details[i].fullProfile ? 140 : 100,
                               d.details[i].fullProfile);
            view(p, d, d.views[0], {102, 78}, *scale,
                 d.reverseView ? QStringLiteral("Обратная сторона A")
                               : QStringLiteral("Вид со стороны A"),
                 false, false, false, &occupied);
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
            view(p, d, d.views[1], {128, 174}, *scale, QStringLiteral("Вид сверху"), false, false,
                 false, &occupied);
            view(p, d, d.views[2], {328, 72}, *scale, QStringLiteral("Вид слева"), false, false,
                 false, &occupied);
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
        if (pageCount(d) > 1)
            text(p, QRectF(25, 285, 195, 6),
                 QStringLiteral("Продолжение профилей: лист 2-%1").arg(pageCount(d)), 2.5,
                 Qt::AlignLeft | Qt::AlignVCenter);
    } else {
        const std::size_t first = (inlineProfiles(d) ? profileCapacity(d) : 0) +
                                  static_cast<std::size_t>(page - 1) * profileCapacity(d);
        const std::array<QPointF, 4> centers =
            profileCapacity(d) == 3
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
    QStringList notes;
    if (page == 0 && inlineProfiles(d) &&
        (!d.views[1].holeDimensions.empty() || !d.views[2].holeDimensions.empty()))
        notes << QStringLiteral(
            "Привязки отверстий других стенок: выберите их грань главным видом");
    if (d.bendRecognitionStatus != BendRecognitionStatus::Completed ||
        (d.drawingOnlyRecognition && d.bends.empty()))
        notes << QStringLiteral("Размеры гибов не построены: геометрия не прошла распознавание");
    else if (d.drawingOnlyRecognition)
        notes << QStringLiteral("Измерения только для чертежа; симуляция этих зон не подтверждена");
    if (std::any_of(d.bends.begin(), d.bends.end(), [](const auto &b) { return b.diagnostic; }))
        notes << QStringLiteral("D — кандидат гиба");
    if (std::any_of(d.bends.begin(), d.bends.end(), [](const auto &b) { return b.trimmedEnds; }))
        notes << QStringLiteral("! — подрезанные торцы");
    if (d.unrecognizedCylindricalFaces)
        notes << QStringLiteral("Не распознано цилиндрических граней: %1")
                     .arg(d.unrecognizedCylindricalFaces);
    text(p, QRectF(25, 228, 380, 6), notes.join(QStringLiteral(" · ")), 2.5,
         Qt::AlignLeft | Qt::AlignVCenter);
    pen(p, .5);
    p.drawRect(QRectF(25, 237, 195, 13));
    p.drawLine(QPointF(160, 237), QPointF(160, 250));
    text(p, QRectF(27, 237, 130, 13), QStringLiteral("Количество, шт."), 3.5,
         Qt::AlignLeft | Qt::AlignVCenter);
    text(p, QRectF(161, 237, 58, 13),
         t.quantity ? QString::number(*t.quantity) : QStringLiteral("-"), 5);
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
            for (int page = 0; page < pageCount(d); ++page) {
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
