#pragma once
#include "bending/ui/DrawingSheet.h"
#include <QPainter>
namespace bending::ui::drawing {
inline const QRectF contentArea{25, 22, 385, 215};
std::vector<std::size_t> requiredDetails(const application::BendingDrawing &);
application::DrawingView preparedView(const application::BendingDrawing &, DrawingViewKey);
void paintPlaced(QPainter &, const application::BendingDrawing &, const DrawingPlacedView &,
                 DrawingFlatSide, std::vector<QRectF> &, QRectF *selectionBounds = nullptr);
void paintCompositionFrame(QPainter &, const application::BendingDrawing &, const DrawingTitle &,
                           const DrawingSheetPlan &, int);
void paintComposition(QPainter &, const application::BendingDrawing &, const DrawingTitle &, int);
} // namespace bending::ui::drawing
