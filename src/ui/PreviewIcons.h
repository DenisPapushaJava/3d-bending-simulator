#pragma once

#include <QFont>
#include <QIcon>
#include <QPainter>
#include <QPainterPath>
#include <QPixmap>

namespace bending::ui::previewIcons {
namespace detail {
template <typename Draw> inline QIcon make(Draw draw) {
    QIcon icon;
    for (int ratio : {1, 2})
        for (auto state : {QIcon::Off, QIcon::On}) {
            QPixmap pixmap(24 * ratio, 24 * ratio);
            pixmap.setDevicePixelRatio(ratio);
            pixmap.fill(Qt::transparent);
            QPainter painter(&pixmap);
            painter.setRenderHint(QPainter::Antialiasing);
            painter.setPen(QPen(QColor(state == QIcon::On ? "#087F83" : "#334155"), 1.6,
                                Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));
            draw(painter);
            painter.end();
            icon.addPixmap(pixmap, QIcon::Normal, state);
        }
    return icon;
}
} // namespace detail

inline QIcon fit() {
    return detail::make([](QPainter &painter) {
        QPainterPath corners;
        for (const auto corner : {QPointF(3, 3), QPointF(21, 3), QPointF(3, 21), QPointF(21, 21)}) {
            corners.moveTo(corner + QPointF(corner.x() < 12 ? 4 : -4, 0));
            corners.lineTo(corner);
            corners.lineTo(corner + QPointF(0, corner.y() < 12 ? 4 : -4));
        }
        painter.drawPath(corners);
        painter.drawRoundedRect(QRectF(8, 6, 8, 12), .7, .7);
    });
}

inline QIcon paper(const QString &label) {
    return detail::make([label](QPainter &painter) {
        const QRectF sheet(2, 4, 20, 16);
        constexpr double fold = 3;
        QPainterPath outline;
        outline.moveTo(sheet.topLeft());
        outline.lineTo(sheet.topRight() - QPointF(fold, 0));
        outline.lineTo(sheet.topRight() + QPointF(0, fold));
        outline.lineTo(sheet.bottomRight());
        outline.lineTo(sheet.bottomLeft());
        outline.closeSubpath();
        outline.moveTo(sheet.topRight() - QPointF(fold, 0));
        outline.lineTo(sheet.topRight() + QPointF(-fold, fold));
        outline.lineTo(sheet.topRight() + QPointF(0, fold));
        painter.drawPath(outline);

        auto font = painter.font();
        font.setPixelSize(10);
        font.setBold(true);
        painter.setFont(font);
        painter.drawText(sheet.adjusted(0, 1, -1, 0), Qt::AlignCenter, label);
    });
}

inline QIcon ruler() {
    return detail::make([](QPainter &painter) {
        painter.translate(12, 12);
        painter.rotate(-35);
        painter.drawRoundedRect(QRectF(-10, -4, 20, 8), 1, 1);
        for (int x : {-6, -3, 0, 3, 6})
            painter.drawLine(QPointF(x, -4), QPointF(x, x % 6 == 0 ? 0 : -1.5));
    });
}
} // namespace bending::ui::previewIcons
