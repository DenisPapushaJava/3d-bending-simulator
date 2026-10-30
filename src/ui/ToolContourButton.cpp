#include "ToolContourButton.h"
#include "ToolDrawingPath.h"
#include <QKeyEvent>
#include <QLocale>
#include <QPainter>
#include <QStyleOptionToolButton>
#include <QStylePainter>
#include <QTransform>
#include <numbers>
namespace bending::ui {
namespace {
QString number(double value) {
    return QLocale().toString(value, 'g', 6);
}
} // namespace
ToolContourButton::ToolContourButton(core::ToolKind kind, QWidget *parent, bool compact)
    : QToolButton(parent), kind_(kind), compact_(compact) {
    setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding);
    setMinimumSize(compact ? QSize(112, 142) : QSize(170, 260));
    if (compact)
        setFixedHeight(208);
    setCheckable(compact);
    setFocusPolicy(Qt::StrongFocus);
    setProperty("toolContour", true);
    setStyleSheet(QStringLiteral(
        "QToolButton[toolContour=\"true\"]{background:white;border:1px solid "
        "#DCE3E9;border-radius:5px;}"
        "QToolButton[toolContour=\"true\"]:hover{border:2px solid #087F83;}"
        "QToolButton[toolContour=\"true\"]:checked{background:#E5F3F3;border:2px solid #087F83;}"
        "QToolButton[toolContour=\"true\"]:disabled{background:#F0F3F6;border:1px solid #DCE3E9;}"
        "QToolButton[toolContour=\"true\"]:focus{border:2px solid #087F83;}"));
    setTool({}, 0);
}
void ToolContourButton::setTool(std::optional<core::ToolPreset> tool, std::size_t groove,
                                bool reversed) {
    if (tool_ == tool && groove_ == groove && reversed_ == reversed && !heading_.isEmpty())
        return;
    tool_ = std::move(tool);
    groove_ = groove;
    reversed_ = reversed;
    path_ = {};
    const auto name =
        kind_ == core::ToolKind::Punch ? QStringLiteral("Пуансон") : QStringLiteral("Матрица");
    heading_ = name + QStringLiteral(" ▾");
    dimensions_ = {};
    if (tool_) {
        heading_ = compact_ ? QString::fromStdString(tool_->code)
                            : name + QStringLiteral(" · ") + QString::fromStdString(tool_->code) +
                                  QStringLiteral(" ▾");
        auto drawing =
            kind_ == core::ToolKind::Die ? core::activeDieDrawing(*tool_, groove_) : tool_->drawing;
        if (!tool_->teaching && drawing && drawing->millimeters && core::validToolDrawing(*drawing))
            path_ = toolDrawingPath(*drawing);
        if (kind_ == core::ToolKind::Punch && reversed_)
            path_ = QTransform::fromScale(-1, 1).map(path_);
        if (kind_ == core::ToolKind::Die) {
            if (const auto g = core::dieGroove(*tool_, groove_))
                dimensions_ = QStringLiteral("V %1 мм · %2°")
                                  .arg(number(tool_->openingsMm[groove_]),
                                       number(g->angleRadians * 180 / std::numbers::pi));
        } else if (tool_->noseRadiusMm)
            dimensions_ = QStringLiteral("R %1 мм · %2°")
                              .arg(number(*tool_->noseRadiusMm),
                                   number(tool_->angleRadians * 180 / std::numbers::pi));
    }
    if (compact_ && tool_ && kind_ == core::ToolKind::Die && groove_ < tool_->openingsMm.size()) {
        heading_ = QStringLiteral("V %1").arg(number(tool_->openingsMm[groove_]));
    }
    setAccessibleName((compact_ && tool_
                           ? QString::fromStdString(tool_->code) + QStringLiteral(" · ")
                           : QString{}) +
                      heading_);
    setAccessibleDescription(
        dimensions_ +
        (tool_ ? QStringLiteral(" · L %1 мм").arg(number(tool_->lengthMm)) : QString{}) +
        (path_.isEmpty() ? QStringLiteral(" · нужен контур DXF")
                         : QStringLiteral(" · контур DXF")));
    setToolTip(heading_ + QLatin1Char('\n') + dimensions_ +
               (issue_.isEmpty() ? QString{} : QLatin1Char('\n') + issue_));
    update();
}
void ToolContourButton::setIssue(QString issue) {
    if (issue_ == issue)
        return;
    issue_ = std::move(issue);
    setToolTip(heading_ + QLatin1Char('\n') + dimensions_ +
               (issue_.isEmpty() ? QString{} : QLatin1Char('\n') + issue_));
    update();
}
void ToolContourButton::paintEvent(QPaintEvent *) {
    QStylePainter painter(this);
    QStyleOptionToolButton option;
    initStyleOption(&option);
    option.text.clear();
    option.icon = {};
    painter.drawComplexControl(QStyle::CC_ToolButton, option);
    painter.setRenderHint(QPainter::Antialiasing);
    const auto ink = isEnabled() ? QColor("#1C2938") : QColor("#637181");
    painter.setPen(ink);
    painter.drawText(
        rect().adjusted(12, 10, compact_ ? -28 : -12, -height() + 38), Qt::AlignCenter,
        painter.fontMetrics().elidedText(heading_, Qt::ElideRight, width() - (compact_ ? 40 : 24)));
    if (isChecked()) {
        painter.save();
        painter.setPen(QPen(ink, 1.8, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));
        painter.drawLine(QPointF(width() - 23, 22), QPointF(width() - 19, 26));
        painter.drawLine(QPointF(width() - 19, 26), QPointF(width() - 12, 17));
        painter.restore();
    }
    const QRectF target = QRectF(rect()).adjusted(18, 44, -18, compact_ ? -44 : -68);
    if (path_.isEmpty()) {
        painter.drawText(target, Qt::AlignCenter | Qt::TextWordWrap,
                         tool_ ? QStringLiteral("Нужен контур DXF")
                               : QStringLiteral("+ Выбрать инструмент"));
    } else {
        const auto bounds = path_.boundingRect();
        const double scale = std::min(target.width() / std::max(1.e-9, bounds.width()),
                                      target.height() / std::max(1.e-9, bounds.height()));
        painter.save();
        painter.translate(target.center());
        painter.scale(scale, scale);
        painter.translate(-bounds.center());
        QPen pen(isEnabled() ? QColor("#087F83") : ink, 1.5);
        pen.setCosmetic(true);
        painter.setPen(pen);
        painter.setBrush(isEnabled() ? QColor("#D8E8EA")
                                     : palette().color(QPalette::Disabled, QPalette::Window));
        painter.drawPath(path_);
        painter.restore();
    }
    painter.setPen(ink);
    const auto text = !issue_.isEmpty() && compact_ ? issue_ : dimensions_;
    painter.drawText(rect().adjusted(8, height() - (compact_ ? 39 : 55), -8, -7),
                     Qt::AlignCenter | Qt::TextWordWrap, text);
}
void ToolContourButton::keyPressEvent(QKeyEvent *event) {
    if (event->key() == Qt::Key_Return || event->key() == Qt::Key_Enter) {
        click();
        event->accept();
    } else
        QToolButton::keyPressEvent(event);
}
} // namespace bending::ui
