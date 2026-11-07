#include "ToolAssemblyDiagram.h"
#include "ToolSegmentColorPreferences.h"
#include <QMouseEvent>
#include <QPainter>
#include <QPainterPath>
#include <QPixmap>
#include <algorithm>
#include <cmath>

namespace bending::ui {
namespace {
QPainterPath hornOutline(double width, double height, double scale, const core::HornRelief &h) {
    QPainterPath path;
    const double root = core::hornRootHeight(h);
    path.moveTo(h.projectionMm * scale, 0);
    path.lineTo(width, 0);
    path.lineTo(width, height);
    path.lineTo(0, height);
    path.lineTo(0, height - h.tipHeightMm * scale);
    const double angle = std::acos(std::max(0., 1 - h.projectionMm / h.radiusMm));
    for (int i = 0; i <= 16; ++i) {
        const double t = angle * (1 - i / 16.);
        path.lineTo((h.projectionMm - h.radiusMm + h.radiusMm * std::cos(t)) * scale,
                    height - (root - h.radiusMm * std::sin(t)) * scale);
    }
    path.closeSubpath();
    return path;
}
} // namespace
QIcon toolHornEndIcon(bool right, qreal ratio) {
    QPixmap image(QSize(32, 32) * ratio);
    image.setDevicePixelRatio(ratio);
    image.fill(Qt::transparent);
    QPainter painter(&image);
    painter.setRenderHint(QPainter::Antialiasing);
    painter.translate(right ? 28 : 4, 3);
    if (right)
        painter.scale(-1, 1);
    painter.setPen(QPen(QColor("#507781"), 1.2));
    painter.setBrush(QColor("#adbec4"));
    painter.drawPath(hornOutline(24, 25, .7, {16, 16, 7}));
    return QIcon(image);
}

ToolAssemblyDiagram::ToolAssemblyDiagram(std::optional<core::ToolPreset> t, QWidget *parent)
    : QWidget(parent), tool(std::move(t)) {
    setObjectName("assemblyDiagram");
    setMinimumHeight(128);
    setAccessibleName(QStringLiteral("Расположение секций вдоль инструмента"));
    setToolTip(QStringLiteral("Нажмите на секцию, чтобы выбрать её в составе ниже."));
}
void ToolAssemblyDiagram::setTool(std::optional<core::ToolPreset> value) {
    if (tool != value)
        tool = std::move(value);
    update();
}
void ToolAssemblyDiagram::paintEvent(QPaintEvent *) {
    QPainter p(this);
    p.setRenderHint(QPainter::Antialiasing);
    const auto sections = intervals ? intervals() : std::nullopt;
    p.setPen(palette().color(QPalette::Text));
    if (!tool || !tool->segments || !sections || sections->empty()) {
        p.drawText(rect().adjusted(12, 4, -12, -4), Qt::AlignCenter | Qt::TextWordWrap,
                   QStringLiteral("Добавьте секции или подберите состав по длине гиба"));
        return;
    }
    const double span = sections->back().toMm, target = required ? required() : 0;
    const double scale = (width() - 32.) / std::max(span, target);
    const double start = 16 + std::max(0., target - span) / 2 * scale;
    const double y = 12, height = 44;
    const auto colors = toolSegmentColors();
    for (std::size_t i = 0; i < sections->size(); ++i) {
        const auto &s = (*sections)[i];
        const auto &piece = tool->segments->at(s.stockRow);
        const double x = start + s.fromMm * scale;
        const double w = (s.toMm - s.fromMm) * scale;
        p.save();
        p.translate(x, y);
        p.setPen(QPen(selected && selected() == static_cast<int>(i) ? QColor("#15858c")
                                                                    : QColor("#507781"),
                      selected && selected() == static_cast<int>(i) ? 2 : 1));
        p.setBrush(viewer::toolSegmentColor(piece.lengthMm, colors));
        if (piece.horn) {
            // A longitudinal schematic: a/R/h are real, transverse profile is not shown.
            const double vertical = height / std::max(tool->heightMm.value_or(60.),
                                                      core::hornRootHeight(*piece.horn) + 1);
            // Axial and vertical scales differ; retain the true axial span of the relief.
            QTransform axial;
            axial.scale(scale / vertical, 1);
            auto path =
                axial.map(hornOutline(piece.lengthMm * vertical, height, vertical, *piece.horn));
            if (piece.shape == core::SegmentShape::RightHorn) {
                QTransform mirror;
                mirror.translate(w, 0);
                mirror.scale(-1, 1);
                path = mirror.map(path);
            }
            p.drawPath(path);
        } else
            p.drawRect(QRectF(0, 0, w, height));
        p.restore();
        p.setPen(palette().color(QPalette::Text));
        if (w >= 38)
            p.drawText(QRectF(x, y + height + 4, w, 18), Qt::AlignCenter,
                       QString::number(piece.lengthMm));
    }
    if (target > 0) {
        const double x = start + (span - target) / 2 * scale, lineY = 99;
        p.setPen(
            QPen(span + 1e-5 < target ? QColor("#a86529") : QColor("#507781"), 1, Qt::DashLine));
        p.drawLine(QPointF(x, lineY), QPointF(x + target * scale, lineY));
        for (double end : {x, x + target * scale})
            p.drawLine(QPointF(end, lineY - 5), QPointF(end, lineY + 5));
        p.setPen(palette().color(QPalette::Text));
        p.drawText(QRectF(0, lineY + 7, width(), 20), Qt::AlignCenter,
                   QStringLiteral("Гиб %1 мм").arg(target));
    }
}
void ToolAssemblyDiagram::mousePressEvent(QMouseEvent *event) {
    const auto sections = intervals ? intervals() : std::nullopt;
    if (event->button() == Qt::LeftButton && sections && !sections->empty() && onSelected &&
        event->position().y() >= 10 && event->position().y() <= 58) {
        const double span = sections->back().toMm, target = required ? required() : 0;
        const double scale = (width() - 32.) / std::max(span, target);
        const double local = (event->position().x() - 16) / scale - std::max(0., target - span) / 2;
        for (std::size_t i = 0; i < sections->size(); ++i)
            if (local >= (*sections)[i].fromMm && local <= (*sections)[i].toMm) {
                onSelected(static_cast<int>(i));
                event->accept();
                return;
            }
    }
    QWidget::mousePressEvent(event);
}
} // namespace bending::ui
