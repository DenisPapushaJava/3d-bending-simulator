#include "ToolProfilesWidget.h"
#include "bending/simulation/ToolProfiles.h"

#include <QLocale>
#include <QPainter>
#include <QPainterPath>
#include <QTransform>
#include <algorithm>
#include <cmath>
#include <numbers>

namespace bending::ui {
namespace {
QString number(double v) {
    return QLocale::system().toString(v, 'g', 6);
}
constexpr double degrees = 180 / std::numbers::pi;
} // namespace
ToolProfilesWidget::ToolProfilesWidget(QWidget *parent, bool enlarged)
    : QWidget(parent), enlarged_(enlarged) {
    setMinimumHeight(enlarged ? 240 : 170);
    setSizePolicy(QSizePolicy::Expanding, enlarged ? QSizePolicy::Expanding : QSizePolicy::Fixed);
    setAccessibleName(QStringLiteral("Рабочие сечения пуансона и матрицы"));
}
void ToolProfilesWidget::setTools(std::optional<core::PressTools> tools) {
    tools_ = tools && core::validPressTools(*tools) ? tools : std::nullopt;
    setAccessibleDescription(
        tools_
            ? QStringLiteral(
                  "Условная рабочая модель. Пуансон: угол %1°, радиус %2 мм, длина %3 мм. Матрица: "
                  "V %4 мм, угол %5°, длина %6 мм. Полные заводские профили не показаны.")
                  .arg(number(tools_->punchAngleRadians * degrees), number(tools_->noseRadiusMm),
                       number(core::punchLengthMm(*tools_)), number(tools_->openingMm),
                       number(tools_->dieAngleRadians * degrees), number(tools_->lengthMm))
            : QStringLiteral("Выберите пару и ручей или проверьте ручные параметры."));
    update();
}
void ToolProfilesWidget::paintEvent(QPaintEvent *) {
    QPainter painter(this);
    painter.setRenderHint(QPainter::Antialiasing);
    painter.fillRect(rect(), QColor("#FFFFFF"));
    painter.setPen(QColor("#DCE3E9"));
    painter.drawRoundedRect(QRectF(rect()).adjusted(.5, .5, -.5, -.5), 4, 4);
    if (!tools_) {
        painter.setPen(QColor("#526171"));
        painter.drawText(
            rect().adjusted(16, 12, -16, -12), Qt::AlignCenter | Qt::TextWordWrap,
            QStringLiteral("Выберите полную пару и ручей\nили проверьте ручные параметры"));
        return;
    }
    const auto profiles = simulation::toolProfiles(*tools_).value();
    QPainterPath punch;
    punch.moveTo(profiles.punch[0].x, profiles.punch[0].y);
    punch.lineTo(profiles.punch[1].x, profiles.punch[1].y);
    const auto &arc = profiles.nose;
    punch.arcTo(QRectF(arc.center.x - arc.radiusMm, arc.center.y - arc.radiusMm, 2 * arc.radiusMm,
                       2 * arc.radiusMm),
                -arc.startRadians * degrees, -arc.sweepRadians * degrees);
    punch.lineTo(profiles.punch[4].x, profiles.punch[4].y);
    punch.closeSubpath();
    QPainterPath die;
    die.moveTo(profiles.die[0].x, profiles.die[0].y);
    for (std::size_t i = 1; i < profiles.die.size(); ++i)
        die.lineTo(profiles.die[i].x, profiles.die[i].y);
    die.closeSubpath();
    const auto punchBounds = punch.boundingRect(), dieBounds = die.boundingRect();
    const double half = width() / 2.;
    const double drawingTop = 42, drawingBottom = height() - 49.;
    const double drawingHeight = std::max(1., drawingBottom - drawingTop);
    const double drawingWidth = std::max(1., half - 32);
    const double scale =
        std::min(drawingWidth / std::max(punchBounds.width(), dieBounds.width()),
                 drawingHeight / std::max(punchBounds.height(), dieBounds.height()));
    auto font = painter.font();
    font.setPixelSize(enlarged_ ? 14 : 11);
    painter.setFont(font);
    painter.setPen(QColor("#DCE3E9"));
    painter.drawLine(QPointF(half, 12), QPointF(half, height() - 12));
    for (int i = 0; i < 2; ++i) {
        const auto &path = i == 0 ? punch : die;
        const auto &bounds = i == 0 ? punchBounds : dieBounds;
        const double cx = half * (i + .5), cy = (drawingTop + drawingBottom) / 2.;
        QTransform transform;
        transform.translate(cx, cy);
        transform.scale(scale, -scale);
        transform.translate(-bounds.center().x(), -bounds.center().y());
        painter.setPen(QPen(QColor("#DCE3E9"), 1, Qt::DashLine));
        painter.drawLine(QPointF(cx, drawingTop), QPointF(cx, drawingBottom));
        painter.setPen(QPen(QColor("#087F83"), enlarged_ ? 2 : 1.3));
        painter.setBrush(QColor(i == 0 ? "#E5F3F3" : "#D9E5ED"));
        painter.drawPath(transform.map(path));
        painter.setBrush(Qt::NoBrush);
        painter.setPen(QColor("#1C2938"));
        painter.drawText(QRectF(i * half, 7, half, 19), Qt::AlignCenter,
                         i == 0 ? QStringLiteral("Пуансон") : QStringLiteral("Матрица"));
        const auto dimensions =
            i == 0 ? QStringLiteral("α %1° · R %2 мм\nL %3 мм")
                         .arg(number(tools_->punchAngleRadians * degrees),
                              number(tools_->noseRadiusMm), number(core::punchLengthMm(*tools_)))
                   : QStringLiteral("V %1 мм · β %2°\nL %3 мм")
                         .arg(number(tools_->openingMm), number(tools_->dieAngleRadians * degrees),
                              number(tools_->lengthMm));
        painter.drawText(QRectF(i * half + 4, height() - 43, half - 8, 37), Qt::AlignCenter,
                         dimensions);
        if (i == 1) {
            const auto left = transform.map(QPointF(profiles.die[1].x, profiles.die[1].y));
            const auto right = transform.map(QPointF(profiles.die[3].x, profiles.die[3].y));
            const double y = left.y() - 10;
            painter.setPen(QPen(QColor("#526171"), 1));
            painter.drawLine(QPointF(left.x(), y), QPointF(right.x(), y));
            for (const auto p : {left, right})
                painter.drawLine(QPointF(p.x(), y - 3), QPointF(p.x(), p.y() - 2));
            const double tick = std::min(4., (right.x() - left.x()) / 4);
            painter.drawLine(QPointF(left.x(), y), QPointF(left.x() + tick, y - 3));
            painter.drawLine(QPointF(right.x(), y), QPointF(right.x() - tick, y - 3));
        }
    }
}
} // namespace bending::ui
