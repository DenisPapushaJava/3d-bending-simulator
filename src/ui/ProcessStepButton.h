#pragma once

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
        setCheckable(true);
        setMinimumHeight(84);
        setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
        auto *row = new QHBoxLayout(this);
        row->setContentsMargins(18, 10, 18, 10);
        row->addSpacing(42);
        auto *copy = new QVBoxLayout;
        copy->setSpacing(3);
        auto *title = new QLabel(QStringLiteral("0%1  %2").arg(stage + 1).arg(name), this);
        title->setStyleSheet(
            QStringLiteral("font-size:14px;color:#1C2938;background:transparent;"));
        title->setAttribute(Qt::WA_TransparentForMouseEvents);
        copy->addWidget(title);
        status_ = new QLabel(this);
        status_->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
        status_->setStyleSheet(
            QStringLiteral("font-size:12px;color:#526171;background:transparent;"));
        status_->setAttribute(Qt::WA_TransparentForMouseEvents);
        copy->addWidget(status_);
        row->addLayout(copy, 1);
    }

    void setStatus(const QString &status) {
        status_->setText(status);
        setAccessibleName(name_ + QStringLiteral(" · ") + status);
        setToolTip(name_ + QStringLiteral(" · ") + status +
                   QStringLiteral(" · Alt+%1").arg(stage_ + 1));
    }

  protected:
    void paintEvent(QPaintEvent *event) override {
        QToolButton::paintEvent(event);
        QPainter painter(this);
        painter.setRenderHint(QPainter::Antialiasing);
        painter.translate(18, (height() - 32) / 2.0);
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
        } else {
            painter.drawEllipse(QRectF(2, 2, 28, 28));
            painter.drawPolygon(QPolygonF{{13, 9}, {23, 16}, {13, 23}});
        }
    }

  private:
    int stage_;
    QString name_;
    QLabel *status_;
};
} // namespace bending::ui
