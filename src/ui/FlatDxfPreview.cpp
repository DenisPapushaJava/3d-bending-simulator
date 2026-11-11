#include "FlatDxfPreview.h"
#include "FlatDxfExport.h"
#include "FlatDxfGeometry.h"

#include <QFontMetricsF>
#include <QGraphicsPathItem>
#include <QGraphicsScene>
#include <QKeyEvent>
#include <QLocale>
#include <QPainter>
#include <QScrollBar>
#include <QWheelEvent>
#include <algorithm>
#include <array>
#include <cmath>
#include <numbers>

namespace bending::ui {
namespace {
QPointF scenePoint(const application::PointMm &point) {
    return {point.x, -point.y};
}

double dot(QPointF a, QPointF b) {
    return QPointF::dotProduct(a, b);
}

double angle(QPointF p) {
    return std::atan2(-p.y(), p.x()) * 180 / std::numbers::pi;
}

bool onSweep(double at, double start, double sweep) {
    const double direction = sweep > 0 ? 1 : -1;
    const double relative = std::fmod((at - start) * direction + 360, 360);
    return relative <= std::abs(sweep);
}

QPointF project(QPointF point, QLineF line) {
    const auto direction = line.p2() - line.p1();
    const double lengthSquared = dot(direction, direction);
    return lengthSquared > 0
               ? line.pointAt(std::clamp(dot(point - line.p1(), direction) / lengthSquared, 0., 1.))
               : line.p1();
}

QString mm(double value) {
    return QLocale().toString(value, 'f', 2) + QStringLiteral(" мм");
}

QPen cosmeticPen(const QColor &color, double width) {
    QPen pen(color, width);
    pen.setCosmetic(true);
    return pen;
}

QRectF circleAt(QPointF centre, double radius) {
    return {centre.x() - radius, centre.y() - radius, 2 * radius, 2 * radius};
}

struct AxisColors {
    QColor x;
    QColor y;
};

AxisColors axisColors(const QColor &background) {
    const bool dark = background.lightness() < 128;
    return {QColor(dark ? "#f87171" : "#b91c1c"), QColor(dark ? "#4ade80" : "#047857")};
}

class AxesIndicator final : public QWidget {
  public:
    explicit AxesIndicator(QWidget *parent) : QWidget(parent) {
        setAttribute(Qt::WA_TransparentForMouseEvents);
        setAccessibleName(QStringLiteral("Ось X вправо, ось Y вверх"));
        setFixedSize(60, 60);
    }

  protected:
    void paintEvent(QPaintEvent *) override {
        QPainter painter(this);
        painter.setRenderHint(QPainter::Antialiasing);
        auto background = palette().base().color();
        const auto colors = axisColors(background);
        background.setAlpha(232);
        painter.setPen(palette().midlight().color());
        painter.setBrush(background);
        painter.drawRoundedRect(QRectF(rect()).adjusted(.5, .5, -.5, -.5), 6, 6);

        auto labelFont = font();
        labelFont.setBold(true);
        labelFont.setPixelSize(12);
        painter.setFont(labelFont);
        const QPointF origin(16, 44);
        const auto drawAxis = [&](QPointF direction, const QColor &color, const QRectF &label,
                                  const QString &text) {
            const auto tip = origin + direction * 25;
            const QPointF wing(-direction.y() * 3.5, direction.x() * 3.5);
            painter.setPen(QPen(color, 1.8, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));
            painter.drawLine(origin, tip);
            painter.drawPolyline(
                QPolygonF{tip - direction * 5 + wing, tip, tip - direction * 5 - wing});
            painter.drawText(label, Qt::AlignCenter, text);
        };
        drawAxis({1, 0}, colors.x, {43, 35, 14, 18}, QStringLiteral("X"));
        drawAxis({0, -1}, colors.y, {9, 2, 14, 16}, QStringLiteral("Y"));
    }
};
} // namespace

FlatDxfPreview::FlatDxfPreview(QWidget *parent) : QGraphicsView(parent) {
    setObjectName(QStringLiteral("flatDxfPreview"));
    setAccessibleName(QStringLiteral("Предпросмотр DXF"));
    setScene(new QGraphicsScene(this));
    setRenderHint(QPainter::Antialiasing);
    const auto base = palette().base().color();
    setBackgroundBrush(base.lightness() < 128 ? base : QColor("#edf0f2"));
    setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    setVerticalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    setTransformationAnchor(NoAnchor);
    setResizeAnchor(AnchorViewCenter);
    setMouseTracking(true);
    setFocusPolicy(Qt::StrongFocus);
    axesIndicator_ = new AxesIndicator(this);
    axesIndicator_->hide();
}

FlatDxfPreview::Curve FlatDxfPreview::curve(const application::FlatCutVertex &start,
                                            const application::PointMm &end) {
    Curve result{QLineF(scenePoint(start.point), scenePoint(end)), {}, 0, 0};
    if (start.bulge != 0) {
        const auto arc = cutArc(start.point, end, start.bulge);
        const auto centre = scenePoint(arc.centre);
        result.circle = circleAt(centre, arc.radius);
        result.start = angle(result.chord.p1() - centre);
        result.sweep = arc.sweep * 180 / std::numbers::pi;
    }
    return result;
}

void FlatDxfPreview::setPattern(application::FlatPatternPtr pattern) {
    if (pattern_ == pattern)
        return;
    pattern_ = std::move(pattern);
    scene()->clear();
    bends_ = nullptr;
    outline_.clear();
    circles_.clear();
    bendLines_.clear();
    drawingBounds_ = {};
    axesIndicator_->hide();
    clearMeasurement();
    if (!pattern_ || !pattern_->info().cutProfile)
        return;

    const auto frame = flatDxfFrame(pattern_->info());
    QPainterPath cutPath;
    std::optional<QRectF> bounds;
    const auto include = [&](QPointF p) {
        if (!bounds)
            bounds = QRectF(p, p);
        else
            bounds->setCoords(std::min(bounds->left(), p.x()), std::min(bounds->top(), p.y()),
                              std::max(bounds->right(), p.x()), std::max(bounds->bottom(), p.y()));
    };
    for (const auto &loop : pattern_->info().cutProfile->loops) {
        if (loop.radiusMm > 0) {
            const auto circle = circleAt(scenePoint(frame.map(loop.centre)), loop.radiusMm);
            circles_.push_back(circle);
            cutPath.addEllipse(circle);
            include(circle.topLeft());
            include(circle.bottomRight());
            continue;
        }
        if (loop.vertices.empty())
            continue;
        cutPath.moveTo(scenePoint(frame.map(loop.vertices.front().point)));
        for (std::size_t i = 0; i < loop.vertices.size(); ++i) {
            const auto &vertex = loop.vertices[i];
            const auto &next = loop.vertices[(i + 1) % loop.vertices.size()].point;
            const auto edge = curve({frame.map(vertex.point), vertex.bulge}, frame.map(next));
            include(edge.chord.p1());
            if (edge.sweep != 0) {
                cutPath.arcTo(edge.circle, edge.start, edge.sweep);
                const auto centre = edge.circle.center();
                for (const QPointF extremum : {QPointF(edge.circle.right(), centre.y()),
                                               QPointF(centre.x(), edge.circle.top()),
                                               QPointF(edge.circle.left(), centre.y()),
                                               QPointF(centre.x(), edge.circle.bottom())})
                    if (onSweep(angle(extremum - centre), edge.start, edge.sweep))
                        include(extremum);
            } else
                cutPath.lineTo(edge.chord.p2());
            outline_.push_back(edge);
        }
        cutPath.closeSubpath();
    }
    scene()->addPath(cutPath, cosmeticPen(palette().text().color(), 1.4));
    drawingBounds_ = bounds.value_or(QRectF{});
    axesIndicator_->setVisible(!drawingBounds_.isEmpty());

    QPainterPath bendPath;
    for (const auto &segment : dxfBendLines(pattern_->info())) {
        const QLineF line(scenePoint(frame.map({segment.x1(), segment.y1(), 0})),
                          scenePoint(frame.map({segment.x2(), segment.y2(), 0})));
        bendLines_.push_back(line);
        bendPath.moveTo(line.p1());
        bendPath.lineTo(line.p2());
    }
    bends_ = scene()->addPath(bendPath);
    fitDrawing();
}

void FlatDxfPreview::setBends(bool visible, const QColor &color) {
    if (!bends_)
        return;
    if (bends_->isVisible() != visible) {
        bends_->setVisible(visible);
        clearMeasurement();
        if (fitted_)
            fitDrawing();
    }
    bends_->setPen(cosmeticPen(color, 1.4));
}

void FlatDxfPreview::setMeasuring(bool enabled) {
    measuring_ = enabled;
    viewport()->setCursor(enabled ? Qt::CrossCursor : Qt::ArrowCursor);
    clearMeasurement();
    if (enabled)
        setFocus();
}

void FlatDxfPreview::fitDrawing() {
    if (drawingBounds_.isEmpty())
        return;
    auto bounds = drawingBounds_;
    if (bends_ && bends_->isVisible())
        for (const auto &line : bendLines_)
            for (const auto &point : {line.p1(), line.p2()})
                bounds.setCoords(
                    std::min(bounds.left(), point.x()), std::min(bounds.top(), point.y()),
                    std::max(bounds.right(), point.x()), std::max(bounds.bottom(), point.y()));
    const double margin = std::max(bounds.width(), bounds.height()) * 2;
    setSceneRect(bounds.adjusted(-margin, -margin, margin, margin));
    resetTransform();
    const auto canvas = viewport()->rect().adjusted(32, 32, -32, -32);
    if (canvas.isEmpty())
        return;
    fitScale_ = std::min(canvas.width() / bounds.width(), canvas.height() / bounds.height());
    scale(fitScale_, fitScale_);
    centerOn(bounds.center());
    fitted_ = true;
}

std::optional<FlatDxfPreview::Pick> FlatDxfPreview::pickAt(const QPoint &position,
                                                           Qt::KeyboardModifiers modifiers) const {
    const bool pointOnly = modifiers.testFlag(Qt::ShiftModifier);
    // Some window managers reserve Alt+click for moving windows.
    const bool allowFreePoint =
        modifiers.testFlag(Qt::AltModifier) || modifiers.testFlag(Qt::ControlModifier);
    const auto point = mapToScene(position);
    const double threshold = 9 / transform().m11();
    const bool measuringBetweenPoints = first_ || pointOnly;
    double snapDistance = threshold, entityDistance = threshold;
    Pick snap{PickKind::FreePoint, point, {}, {}, 0};
    Pick entity = snap;
    const auto considerPoint = [&](QPointF candidate, PickKind kind) {
        const double distance = QLineF(candidate, point).length();
        if (distance < snapDistance) {
            snapDistance = distance;
            snap = {kind, candidate, {}, {}, 0};
        }
    };
    const auto considerEntity = [&](Pick candidate) {
        const double distance = QLineF(candidate.point, point).length();
        if (distance < entityDistance) {
            entityDistance = distance;
            entity = candidate;
        }
    };
    const auto considerLine = [&](QLineF line) {
        considerPoint(line.p1(), PickKind::EndPoint);
        considerPoint(line.p2(), PickKind::EndPoint);
        considerEntity({pointOnly ? PickKind::ContourPoint : PickKind::Edge,
                        project(point, line),
                        line,
                        {},
                        0});
    };
    const auto considerQuadrants = [&](QPointF centre, double radius, double start, double sweep) {
        constexpr std::array<QPointF, 4> directions{{{1, 0}, {0, -1}, {-1, 0}, {0, 1}}};
        constexpr std::array<double, 4> angles{0, 90, 180, -90};
        for (std::size_t i = 0; i < directions.size(); ++i) {
            const auto quadrant = centre + directions[i] * radius;
            if (QLineF(quadrant, point).length() <= threshold * (2. / 3) &&
                onSweep(angles[i], start, sweep))
                considerPoint(quadrant, PickKind::Quadrant);
        }
    };
    for (const auto &edge : outline_) {
        if (edge.sweep == 0) {
            const auto box = QRectF(edge.chord.p1(), edge.chord.p2()).normalized();
            if (box.adjusted(-threshold, -threshold, threshold, threshold).contains(point))
                considerLine(edge.chord);
            continue;
        }
        if (!edge.circle.adjusted(-threshold, -threshold, threshold, threshold).contains(point))
            continue;
        considerPoint(edge.chord.p1(), PickKind::EndPoint);
        considerPoint(edge.chord.p2(), PickKind::EndPoint);
        const auto centre = edge.circle.center();
        const double radius = edge.circle.width() / 2;
        const double distance = QLineF(centre, point).length();
        considerPoint(centre, PickKind::Centre);
        if (measuringBetweenPoints)
            considerQuadrants(centre, radius, edge.start, edge.sweep);
        if (distance > 0 && onSweep(angle(point - centre), edge.start, edge.sweep)) {
            const auto projected = centre + (point - centre) * (radius / distance);
            if (measuringBetweenPoints)
                considerEntity({PickKind::ContourPoint, projected, {}, {}, 0});
            else
                considerEntity({PickKind::Arc, projected, {}, centre, radius, edge});
        }
    }
    for (const auto &circle : circles_) {
        const auto centre = circle.center();
        const double radius = circle.width() / 2;
        const double distance = QLineF(centre, point).length();
        considerPoint(centre, PickKind::Centre);
        if (measuringBetweenPoints && std::abs(distance - radius) < threshold)
            considerQuadrants(centre, radius, 0, 360);
        if (distance > 0)
            considerEntity({measuringBetweenPoints ? PickKind::ContourPoint : PickKind::Circle,
                            centre + (point - centre) * (radius / distance),
                            {},
                            centre,
                            radius});
    }
    if (bends_ && bends_->isVisible())
        for (const auto &line : bendLines_)
            considerLine(line);

    // Prefer the actual arc or circle when its rim is closer than a point snap.
    if (snap.kind != PickKind::Quadrant && entity.radius > 0 && entityDistance < snapDistance)
        return entity;
    if (snap.kind != PickKind::FreePoint)
        return snap;
    if (entity.kind != PickKind::FreePoint || allowFreePoint)
        return entity;
    return std::nullopt;
}

FlatDxfPreview::Dimension FlatDxfPreview::between(const Pick &a, const Pick &b) const {
    const bool edgeA = a.kind == PickKind::Edge, edgeB = b.kind == PickKind::Edge;
    if (edgeA != edgeB) {
        const auto &point = edgeA ? b : a;
        const auto &edge = edgeA ? a : b;
        const QLineF line(point.point, project(point.point, edge.edge));
        const auto distance = mm(line.length());
        const auto text = point.kind == PickKind::FreePoint
                              ? QStringLiteral("До линии ≈ %1").arg(distance)
                              : QStringLiteral("До линии %1").arg(distance);
        return {line, text, {}, {}, {}, {point.kind, PickKind::ContourPoint}};
    }
    if (edgeA && edgeB) {
        const auto directionA = a.edge.unitVector(), directionB = b.edge.unitVector();
        const auto u = directionA.p2() - directionA.p1();
        const auto v = directionB.p2() - directionB.p1();
        if (std::abs(u.x() * v.y() - u.y() * v.x()) < 1.e-9) {
            const double start = dot(b.edge.p1() - a.edge.p1(), u);
            const double end = dot(b.edge.p2() - a.edge.p1(), u);
            const double low = std::max(0., std::min(start, end));
            const double high = std::min(a.edge.length(), std::max(start, end));
            if (low <= high) {
                const auto point =
                    a.edge.p1() + u * std::clamp(dot(a.point - a.edge.p1(), u), low, high);
                const QLineF line(point, project(point, b.edge));
                return {line, QStringLiteral("Между линиями %1").arg(mm(line.length()))};
            }
        }
    }
    const QLineF line(a.point, b.point);
    const auto prefix = a.kind == PickKind::FreePoint || b.kind == PickKind::FreePoint
                            ? QStringLiteral("≈ ")
                            : QString{};
    return {line,
            prefix + mm(line.length()),
            QStringLiteral("ΔX %1").arg(mm(std::abs(line.dx()))),
            QStringLiteral("ΔY %1").arg(mm(std::abs(line.dy()))),
            {},
            {a.kind, b.kind}};
}

void FlatDxfPreview::clearMeasurement() {
    first_.reset();
    hover_.reset();
    dimension_.reset();
    emit measurementChanged(
        measuring_ ? QStringLiteral("Выберите точку, линию, дугу или окружность") : QString{});
    viewport()->update();
}

void FlatDxfPreview::selectAt(const QPoint &position, Qt::KeyboardModifiers modifiers) {
    hover_ = pickAt(position, modifiers);
    if (!hover_) {
        viewport()->update();
        return;
    }
    const auto &pick = *hover_;
    if (first_) {
        dimension_ = between(*first_, pick);
        first_.reset();
    } else if (pick.kind == PickKind::Circle) {
        const auto radial = pick.point - pick.centre;
        dimension_ = Dimension{QLineF(pick.centre - radial, pick.centre + radial),
                               QStringLiteral("Ø%1").arg(mm(2 * pick.radius))};
    } else if (pick.kind == PickKind::Arc) {
        dimension_ = Dimension{QLineF(pick.centre, pick.point),
                               QStringLiteral("R%1").arg(mm(pick.radius)),
                               {},
                               {},
                               pick.arc};
    } else {
        dimension_.reset();
        first_ = pick;
    }
    if (!dimension_) {
        emit measurementChanged(QStringLiteral("Выберите вторую точку или линию"));
    } else if (dimension_->deltaX.isEmpty()) {
        emit measurementChanged(dimension_->text);
    } else {
        const auto colors = axisColors(palette().base().color());
        emit measurementChanged(QStringLiteral("<nobr>%1 · <span style=\"color:%2\">%3</span> · "
                                               "<span style=\"color:%4\">%5</span></nobr>")
                                    .arg(dimension_->text, colors.x.name(), dimension_->deltaX,
                                         colors.y.name(), dimension_->deltaY));
    }
    viewport()->update();
}

void FlatDxfPreview::drawDimensionLabel(QPainter &painter, const Dimension &dimension,
                                        QPointF anchor, const QRectF &arcBounds) const {
    painter.setFont(font());
    const QFontMetricsF metrics(painter.font(), painter.device());
    const auto ink = painter.pen().color();
    const auto colors = axisColors(palette().base().color());
    const auto separator = QStringLiteral(" · ");
    const std::array parts{dimension.text, separator, dimension.deltaX, separator,
                           dimension.deltaY};
    const std::array<QColor, 5> inks{ink, ink, colors.x, ink, colors.y};
    const std::size_t count = dimension.deltaX.isEmpty() ? 1 : parts.size();
    std::array<qreal, parts.size()> widths{};
    qreal width = 14;
    for (std::size_t i = 0; i < count; ++i) {
        widths[i] = metrics.horizontalAdvance(parts[i]);
        width += widths[i];
    }
    QRectF label(0, 0, width, metrics.height() + 8);
    label.moveCenter(anchor);
    if (dimension.arc) {
        constexpr qreal gap = 8;
        if (arcBounds.right() + gap + width <= viewport()->width() - 4)
            label.moveLeft(arcBounds.right() + gap);
        else if (arcBounds.left() - gap - width >= 4)
            label.moveRight(arcBounds.left() - gap);
    }
    label.moveLeft(std::clamp(label.left(), 4., std::max(4., viewport()->width() - width - 4)));
    label.moveTop(
        std::clamp(label.top(), 4., std::max(4., viewport()->height() - label.height() - 4)));
    painter.setBrush(palette().base());
    painter.drawRoundedRect(label, 4, 4);

    QPointF baseline = label.topLeft() + QPointF(7, 4 + metrics.ascent());
    for (std::size_t i = 0; i < count; ++i) {
        painter.setPen(inks[i]);
        painter.drawText(baseline, parts[i]);
        baseline.rx() += widths[i];
    }
}

void FlatDxfPreview::drawForeground(QPainter *painter, const QRectF &) {
    if (!measuring_)
        return;
    painter->save();
    painter->resetTransform();
    const QColor color(0, 116, 169);
    painter->setPen(QPen(color, 1.6));
    painter->setBrush(Qt::NoBrush);
    const auto mark = [&](QPointF point, PickKind kind) {
        const auto p = mapFromScene(point);
        if (kind == PickKind::Centre) {
            painter->drawLine(p + QPoint(-6, 0), p + QPoint(6, 0));
            painter->drawLine(p + QPoint(0, -6), p + QPoint(0, 6));
        } else if (kind == PickKind::Quadrant) {
            painter->save();
            const bool dark = palette().base().color().lightness() < 128;
            painter->setPen(QPen(QColor(dark ? "#fb923c" : "#b45309"), 1.6));
            painter->setBrush(palette().base());
            painter->drawPolygon(QPolygonF{p + QPoint(0, -5), p + QPoint(5, 0), p + QPoint(0, 5),
                                           p + QPoint(-5, 0)});
            painter->restore();
        } else {
            painter->save();
            if (kind == PickKind::EndPoint)
                painter->setBrush(color);
            painter->drawEllipse(p, 4, 4);
            painter->restore();
        }
    };
    if (hover_) {
        if (hover_->kind == PickKind::Edge)
            painter->drawLine(mapFromScene(hover_->edge.p1()), mapFromScene(hover_->edge.p2()));
        else if (hover_->kind == PickKind::Circle) {
            const double radius = hover_->radius * transform().m11();
            painter->drawEllipse(QPointF(mapFromScene(hover_->centre)), radius, radius);
        }
        mark(hover_->point, hover_->kind);
        if (hover_->kind == PickKind::Arc && (!dimension_ || !dimension_->arc))
            mark(hover_->centre, PickKind::Centre);
    }
    if (first_) {
        mark(first_->point, first_->kind);
        if (first_->kind == PickKind::Edge)
            painter->drawLine(mapFromScene(first_->edge.p1()), mapFromScene(first_->edge.p2()));
    }
    if (dimension_) {
        const auto a = mapFromScene(dimension_->line.p1()), b = mapFromScene(dimension_->line.p2());
        painter->drawLine(a, b);
        QRectF arcBounds;
        if (const auto &arc = dimension_->arc) {
            const auto circle = viewportTransform().mapRect(arc->circle);
            QPainterPath highlight;
            highlight.arcMoveTo(circle, arc->start);
            highlight.arcTo(circle, arc->start, arc->sweep);
            painter->drawPath(highlight);
            arcBounds = highlight.boundingRect() | QRectF(a, b).normalized();
            mark(dimension_->line.p1(), PickKind::Centre);
        } else {
            mark(dimension_->line.p1(), dimension_->pointKinds[0]);
        }
        mark(dimension_->line.p2(), dimension_->pointKinds[1]);
        const auto midpoint = (QPointF(a) + QPointF(b)) / 2;
        const auto anchor = dimension_->arc ? midpoint : midpoint - QPointF(0, 18);
        drawDimensionLabel(*painter, *dimension_, anchor, arcBounds);
    }
    painter->restore();
}

void FlatDxfPreview::keyPressEvent(QKeyEvent *event) {
    if (event->key() == Qt::Key_Escape && measuring_ && (first_ || dimension_)) {
        clearMeasurement();
        event->accept();
        return;
    }
    QGraphicsView::keyPressEvent(event);
}

void FlatDxfPreview::leaveEvent(QEvent *event) {
    hover_.reset();
    viewport()->update();
    QGraphicsView::leaveEvent(event);
}

void FlatDxfPreview::mousePressEvent(QMouseEvent *event) {
    if (event->button() == Qt::MiddleButton) {
        panPosition_ = event->position().toPoint();
        viewport()->setCursor(Qt::ClosedHandCursor);
        event->accept();
    } else if (event->button() == Qt::LeftButton && measuring_ && !drawingBounds_.isEmpty()) {
        selectAt(event->position().toPoint(), event->modifiers());
        event->accept();
    } else
        QGraphicsView::mousePressEvent(event);
}

void FlatDxfPreview::mouseMoveEvent(QMouseEvent *event) {
    if (panPosition_) {
        const auto delta = event->position().toPoint() - *panPosition_;
        horizontalScrollBar()->setValue(horizontalScrollBar()->value() - delta.x());
        verticalScrollBar()->setValue(verticalScrollBar()->value() - delta.y());
        panPosition_ = event->position().toPoint();
        fitted_ = false;
        hover_.reset();
    } else if (measuring_ && !drawingBounds_.isEmpty()) {
        hover_ = pickAt(event->position().toPoint(), event->modifiers());
    } else {
        QGraphicsView::mouseMoveEvent(event);
        return;
    }
    viewport()->update();
    event->accept();
}

void FlatDxfPreview::mouseReleaseEvent(QMouseEvent *event) {
    if (event->button() == Qt::MiddleButton && panPosition_) {
        panPosition_.reset();
        viewport()->setCursor(measuring_ ? Qt::CrossCursor : Qt::ArrowCursor);
        event->accept();
    } else
        QGraphicsView::mouseReleaseEvent(event);
}

void FlatDxfPreview::resizeEvent(QResizeEvent *event) {
    QGraphicsView::resizeEvent(event);
    if (axesIndicator_) {
        const auto area = viewport()->geometry();
        axesIndicator_->move(area.left() + 10,
                             area.top() +
                                 std::max(0, area.height() - axesIndicator_->height() - 10));
    }
    if (fitted_)
        fitDrawing();
}

void FlatDxfPreview::wheelEvent(QWheelEvent *event) {
    if (drawingBounds_.isEmpty() || event->angleDelta().y() == 0)
        return;
    const auto cursor = event->position().toPoint();
    const auto anchor = mapToScene(cursor);
    const double old = transform().m11();
    const double next = std::clamp(old * std::pow(1.2, event->angleDelta().y() / 120.),
                                   fitScale_ * .5, fitScale_ * 100);
    scale(next / old, next / old);
    centerOn(mapToScene(viewport()->rect().center()) + anchor - mapToScene(cursor));
    fitted_ = false;
    if (measuring_)
        hover_ = pickAt(cursor, event->modifiers());
    viewport()->update();
    event->accept();
}
} // namespace bending::ui
