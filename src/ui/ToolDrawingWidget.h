#pragma once

#include "bending/core/ToolCatalog.h"
#include <QPainter>
#include <QPainterPath>
#include <QWidget>
#include <cmath>
#include <numbers>

namespace bending::ui {
class ToolDrawingWidget final : public QWidget {
  public:
    ToolDrawingWidget(core::ToolDrawing drawing, QWidget *parent)
        : QWidget(parent), drawing_(std::move(drawing)) {
        setMinimumSize(160, 260);
        setObjectName(QStringLiteral("toolDrawingCanvas"));
        setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding);
        setAccessibleName(QStringLiteral("Контур из исходного чертежа инструмента"));
        const auto &first = drawing_.edges.front();
        path_.moveTo(first.start.x, -first.start.y);
        for (const auto &e : drawing_.edges) {
            if (!e.center) {
                path_.lineTo(e.end.x, -e.end.y);
                continue;
            }
            const double r = std::hypot(e.start.x - e.center->x, e.start.y - e.center->y);
            const double start = std::atan2(e.start.y - e.center->y, e.start.x - e.center->x);
            path_.arcTo(e.center->x - r, -e.center->y - r, 2 * r, 2 * r,
                        start * 180 / std::numbers::pi, e.sweepRadians * 180 / std::numbers::pi);
        }
        path_.closeSubpath();
        setToolTip(QStringLiteral("%1\nSHA-256: %2")
                       .arg(QString::fromStdString(drawing_.sourceFile),
                            QString::fromStdString(drawing_.sha256)));
    }

  protected:
    void paintEvent(QPaintEvent *) override {
        QPainter p(this);
        p.setRenderHint(QPainter::Antialiasing);
        p.fillRect(rect(), QColor("#F0F3F6"));
        const auto bounds = path_.boundingRect();
        if (bounds.width() <= 0 || bounds.height() <= 0)
            return;
        const auto target = QRectF(rect()).adjusted(20, 20, -20, -55);
        const double scale =
            std::min(target.width() / bounds.width(), target.height() / bounds.height());
        p.save();
        p.translate(target.center());
        p.scale(scale, scale);
        p.translate(-bounds.center());
        QPen pen(QColor("#087F83"), 2);
        pen.setCosmetic(true);
        p.setPen(pen);
        p.setBrush(QColor("#D8E8EA"));
        p.drawPath(path_);
        p.restore();
        p.setPen(QColor("#1C2938"));
        p.drawText(rect().adjusted(8, height() - 48, -8, -8), Qt::AlignCenter | Qt::TextWordWrap,
                   QStringLiteral("%1 × %2 %3")
                       .arg(bounds.width(), 0, 'f', 2)
                       .arg(bounds.height(), 0, 'f', 2)
                       .arg(drawing_.millimeters
                                ? QStringLiteral("мм")
                                : QStringLiteral("ед. файла · масштаб не подтверждён")));
    }

  private:
    core::ToolDrawing drawing_;
    QPainterPath path_;
};
} // namespace bending::ui
