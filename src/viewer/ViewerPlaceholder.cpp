#include "bending/viewer/ViewerPlaceholder.h"

#include <QPainter>

namespace bending::viewer {

ViewerPlaceholder::ViewerPlaceholder(QWidget *parent) : QWidget(parent) {
    setObjectName(QStringLiteral("viewer"));
    setMinimumSize(400, 300);
    setAccessibleName(QStringLiteral("Область 3D-сцены"));
}

void ViewerPlaceholder::paintEvent(QPaintEvent *) {
    QPainter painter(this);
    painter.fillRect(rect(), QColor("#202A36"));
    painter.setPen(QColor("#2F3E4D"));
    for (int offset = -height(); offset < width() + height(); offset += 48) {
        painter.drawLine(offset, height(), offset + height(), 0);
        painter.drawLine(offset, 0, offset + height(), height());
    }
    painter.setPen(QColor("#DCE3E9"));
    auto font = painter.font();
    font.setPointSize(16);
    painter.setFont(font);
    painter.drawText(rect().adjusted(24, -24, -24, -24), Qt::AlignCenter,
                     QStringLiteral("3D Bending Simulator"));
    font.setPointSize(10);
    painter.setFont(font);
    painter.setPen(QColor("#A3B2C3"));
    painter.drawText(rect().adjusted(24, 40, -24, 40), Qt::AlignCenter,
                     QStringLiteral("Начальный каркас · CAD-просмотрщик ещё не подключён"));
}

} // namespace bending::viewer
