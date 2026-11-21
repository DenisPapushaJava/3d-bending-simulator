#pragma once

#include <QEvent>
#include <QHBoxLayout>
#include <QLabel>
#include <QPainter>
#include <QPolygonF>
#include <QToolButton>
#include <QVBoxLayout>

namespace bending::ui {

// Local navigation control. Icons are our own vector drawings, with no asset dependency.
class ProcessStepButton final : public QToolButton {
  public:
    ProcessStepButton(int stage, const QString &name, QWidget *parent)
        : QToolButton(parent), stage_(stage), name_(name) {
        setCheckable(stage < 3);
        setMinimumHeight(68);
        setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
        auto *row = new QHBoxLayout(this);
        row->setContentsMargins(12, 8, 12, 8);
        row->addSpacing(36);
        auto *copy = new QVBoxLayout;
        copy->setSpacing(3);
        auto *title =
            new QLabel(stage < 3 ? QStringLiteral("%1. %2").arg(stage + 1).arg(name) : name, this);
        title->setStyleSheet(
            QStringLiteral("font-size:14px;color:#1C2938;background:transparent;"));
        title->setAttribute(Qt::WA_TransparentForMouseEvents);
        copy->addWidget(title);
        status_ = new QLabel(this);
        status_->setObjectName(QStringLiteral("processStepStatus"));
        status_->setTextFormat(Qt::PlainText);
        status_->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
        status_->setStyleSheet(
            QStringLiteral("font-size:12px;color:#526171;background:transparent;"));
        status_->setAttribute(Qt::WA_TransparentForMouseEvents);
        status_->installEventFilter(this);
        copy->addWidget(status_);
        row->addLayout(copy, 1);
    }

    void setStatus(const QString &status) {
        if (statusText_ == status)
            return;
        statusText_ = status;
        updateStatusLabel();
        const auto name = name_ + QStringLiteral(" · ") + status;
        setAccessibleName(name);
        const auto shortcut = stage_ < 3    ? QStringLiteral(" · Alt+%1").arg(stage_ + 1)
                              : stage_ == 3 ? QStringLiteral(" · Ctrl+Shift+E")
                                            : QString{};
        setToolTip(QStringLiteral("<qt>%1</qt>").arg((name + shortcut).toHtmlEscaped()));
    }

  protected:
    bool eventFilter(QObject *watched, QEvent *event) override {
        if (watched == status_ &&
            (event->type() == QEvent::Resize || event->type() == QEvent::FontChange))
            updateStatusLabel();
        return QToolButton::eventFilter(watched, event);
    }
    void paintEvent(QPaintEvent *event) override {
        QToolButton::paintEvent(event);
        QPainter painter(this);
        painter.setRenderHint(QPainter::Antialiasing);
        painter.translate(12, (height() - 32) / 2.0);
        painter.setPen(QPen(isChecked() ? QColor(QStringLiteral("#087F83"))
                                        : QColor(QStringLiteral("#526171")),
                            1.8, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));
        if (stage_ == 0) {
            painter.drawPolygon(QPolygonF{
                {3, 18}, {14, 23}, {24, 17}, {24, 3}, {29, 6}, {29, 21}, {14, 30}, {3, 25}});
            painter.drawPolyline(QPolygonF{{3, 18}, {3, 25}, {14, 30}, {14, 23}});
            painter.drawLine(QPointF(24, 17), QPointF(29, 21));
        } else if (stage_ == 1) {
            painter.drawPolygon(QPolygonF{{7, 2}, {25, 2}, {16, 16}});
            painter.drawPolygon(
                QPolygonF{{2, 21}, {9, 21}, {16, 28}, {23, 21}, {30, 21}, {30, 32}, {2, 32}});
        } else if (stage_ == 2) {
            painter.drawEllipse(QRectF(2, 2, 28, 28));
            painter.drawPolygon(QPolygonF{{13, 9}, {23, 16}, {13, 23}});
        } else if (stage_ == 4) {
            painter.drawPolygon(QPolygonF{{2, 8}, {9, 3}, {30, 3}, {30, 29}, {2, 29}});
            painter.setPen(QPen(painter.pen().color(), 1.5, Qt::DashLine));
            painter.drawLine(QPointF(16, 4), QPointF(16, 28));
        } else {
            painter.drawRect(QRectF(2, 5, 28, 22));
            auto framePen = painter.pen();
            framePen.setWidthF(1.4);
            painter.setPen(framePen);
            painter.drawRect(QRectF(6, 9, 20, 14));
            painter.drawPolyline(QPolygonF{{15, 23}, {15, 18}, {26, 18}});
            painter.drawLine(QPointF(21, 18), QPointF(21, 23));
        }
    }

  private:
    void updateStatusLabel() {
        status_->setText(status_->fontMetrics().elidedText(statusText_, Qt::ElideRight,
                                                           status_->contentsRect().width()));
    }

    int stage_;
    QString name_;
    QString statusText_;
    QLabel *status_;
};
} // namespace bending::ui
