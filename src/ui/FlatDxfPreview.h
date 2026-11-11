#pragma once

#include "bending/application/FlatPattern.h"
#include <QGraphicsView>
#include <array>
#include <optional>

class QGraphicsPathItem;

namespace bending::ui {
class FlatDxfPreview final : public QGraphicsView {
    Q_OBJECT
  public:
    explicit FlatDxfPreview(QWidget *parent = nullptr);
    void setPattern(application::FlatPatternPtr pattern);
    void setBends(bool visible, const QColor &color);
    void setMeasuring(bool enabled);
    void fitDrawing();
    QSizeF drawingSize() const {
        return drawingBounds_.size();
    }

  signals:
    void measurementChanged(const QString &text);

  protected:
    void drawForeground(QPainter *, const QRectF &) override;
    void keyPressEvent(QKeyEvent *) override;
    void leaveEvent(QEvent *) override;
    void mouseMoveEvent(QMouseEvent *) override;
    void mousePressEvent(QMouseEvent *) override;
    void mouseReleaseEvent(QMouseEvent *) override;
    void resizeEvent(QResizeEvent *) override;
    void wheelEvent(QWheelEvent *) override;

  private:
    enum class PickKind { FreePoint, EndPoint, Centre, Quadrant, ContourPoint, Edge, Circle, Arc };
    struct Curve {
        QLineF chord;
        QRectF circle;
        double start = 0;
        double sweep = 0;
    };
    struct Pick {
        PickKind kind = PickKind::FreePoint;
        QPointF point;
        QLineF edge;
        QPointF centre;
        double radius = 0;
        std::optional<Curve> arc{};
    };
    struct Dimension {
        QLineF line;
        QString text;
        QString deltaX{};
        QString deltaY{};
        std::optional<Curve> arc{};
        std::array<PickKind, 2> pointKinds{PickKind::ContourPoint, PickKind::ContourPoint};
    };

    static Curve curve(const application::FlatCutVertex &start, const application::PointMm &end);
    std::optional<Pick> pickAt(const QPoint &position, Qt::KeyboardModifiers modifiers) const;
    Dimension between(const Pick &first, const Pick &second) const;
    void clearMeasurement();
    void selectAt(const QPoint &position, Qt::KeyboardModifiers modifiers);
    void drawDimensionLabel(QPainter &painter, const Dimension &dimension, QPointF anchor,
                            const QRectF &arcBounds) const;

    application::FlatPatternPtr pattern_;
    QGraphicsPathItem *bends_ = nullptr;
    QWidget *axesIndicator_ = nullptr;
    std::vector<Curve> outline_;
    std::vector<QRectF> circles_;
    std::vector<QLineF> bendLines_;
    QRectF drawingBounds_;
    std::optional<QPoint> panPosition_;
    std::optional<Pick> hover_;
    std::optional<Pick> first_;
    std::optional<Dimension> dimension_;
    double fitScale_ = 1;
    bool fitted_ = true;
    bool measuring_ = false;
};
} // namespace bending::ui
