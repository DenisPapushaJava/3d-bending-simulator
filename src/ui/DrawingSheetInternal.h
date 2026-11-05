#pragma once
#include "bending/ui/DrawingSheet.h"
#include <QByteArray>
#include <QPainter>
namespace bending::ui {
// Immutable vector commands, in paper millimetres. No widget, pixmap or CAD handle.
struct DrawingViewRecording {
    QByteArray commands;
    DrawingViewKey key;
    int page;
    QRectF area;
    QPointF centre;
    double scale;
    DrawingFlatSide side;
};
} // namespace bending::ui
namespace bending::ui::drawing {
class LayoutContext {
  public:
    LayoutContext(std::stop_token cancellationToken, DrawingLayoutLimits requestedLimits)
        : token(cancellationToken), limits(requestedLimits),
          deadline(std::chrono::steady_clock::now() + requestedLimits.maximumTime) {}
    void checkpoint(bool dimension = false) {
        if (token.stop_requested())
            throw DrawingLayoutInterrupted(DrawingLayoutInterruption::Cancelled,
                                           "Компоновка отменена");
        if (std::chrono::steady_clock::now() >= deadline ||
            (dimension && probes >= limits.maximumDimensionProbes))
            throw DrawingLayoutInterrupted(
                DrawingLayoutInterruption::LimitExceeded,
                "Достигнут лимит компоновки. Уменьшите число видов или измените масштаб.");
        if (dimension)
            ++probes;
    }
    std::stop_token token;
    DrawingLayoutLimits limits;
    std::chrono::steady_clock::time_point deadline;
    std::size_t probes = 0, bytes = 0;
    std::vector<DrawingPlacedDimension> *dimensions = nullptr;
    DrawingDimensionSource source = DrawingDimensionSource::Overall;
    std::size_t index = 0;
};
void initializeFont();
inline const QRectF contentArea{25, 22, 385, 215};
std::vector<std::size_t> requiredDetails(const application::BendingDrawing &);
application::DrawingView preparedView(const application::BendingDrawing &, DrawingViewKey);
void paintPlaced(QPainter &, const application::BendingDrawing &, const DrawingPlacedView &,
                 DrawingFlatSide, std::vector<QRectF> &, QRectF *selectionBounds = nullptr,
                 LayoutContext *context = nullptr);
void recordViews(DrawingSheetPlan &, LayoutContext &);
void paintRecorded(QPainter &, const DrawingPlacedView &, DrawingFlatSide);
void paintCompositionFrame(QPainter &, const application::BendingDrawing &, const DrawingTitle &,
                           const DrawingSheetPlan &, int);
void paintComposition(QPainter &, const application::BendingDrawing &, const DrawingTitle &, int);
} // namespace bending::ui::drawing
