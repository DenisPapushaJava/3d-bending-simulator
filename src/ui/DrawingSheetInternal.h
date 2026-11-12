#pragma once
#include "bending/ui/DrawingSheet.h"
#include <QByteArray>
#include <QHash>
#include <QPainter>
#include <map>
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
    bool compactDimensions;
    QString sheetLinks;
    bool essentialDimensions;
    std::size_t omittedDimensions;
    std::vector<DrawingDimensionId> retainedDimensions;
    bool smallOptionalText;
};
} // namespace bending::ui
namespace bending::ui::drawing {
inline bool optionalDimension(DrawingDimensionSource source, bool flat, bool plate,
                              const application::DrawingDimension &dimension) {
    if (dimension.openingPatternReference)
        return false;
    switch (source) {
    case DrawingDimensionSource::Section:
        return flat || plate;
    case DrawingDimensionSource::Hole:
        return true;
    case DrawingDimensionSource::Axial:
        return flat;
    case DrawingDimensionSource::Overall:
        return false;
    }
    return false;
}
// The frame and title block retain their paper dimensions in both formats.
inline constexpr double titleBlockWidth = 185, titleBlockHeight = 55;
inline QRectF frameArea(QSizeF paper) {
    return {20, 5, paper.width() - 25, paper.height() - 10};
}
inline QRectF contentArea(QSizeF paper) {
    const auto frame = frameArea(paper);
    return {25, 22, frame.right() - 30, frame.bottom() - titleBlockHeight - 22};
}
class LayoutContext {
  public:
    struct RetentionLimit {};
    struct RetentionBudget {
        std::chrono::steady_clock::time_point deadline;
        std::size_t probes;
    };
    LayoutContext(std::stop_token cancellationToken, DrawingLayoutLimits requestedLimits)
        : token(cancellationToken), limits(requestedLimits),
          deadline(std::chrono::steady_clock::now() + requestedLimits.maximumTime) {}
    void checkpoint(bool dimension = false) {
        if (token.stop_requested())
            throw DrawingLayoutInterrupted(DrawingLayoutInterruption::Cancelled,
                                           "Компоновка отменена");
        if (retentionBudget && (std::chrono::steady_clock::now() >= retentionBudget->deadline ||
                                (dimension && probes >= retentionBudget->probes)))
            throw RetentionLimit{};
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
    std::optional<RetentionBudget> retentionBudget;
    std::vector<DrawingPlacedDimension> *dimensions = nullptr;
    std::size_t *omittedDimensions = nullptr;
    QHash<QString, double> dimensionTextWidths;
    // Fixed for this compose, shared by fit probes and final vector recording.
    const std::vector<DrawingViewChoice> *choices = nullptr;
    DrawingDimensionSource source = DrawingDimensionSource::Overall;
    std::size_t index = 0;
    const application::DrawingView &view(const application::BendingDrawing &, DrawingViewKey);

  private:
    std::map<std::pair<DrawingViewKind, std::size_t>, application::DrawingView> views_;
};
void initializeFont();
application::DrawingView preparedView(const application::BendingDrawing &, DrawingViewKey);
application::DrawingView selectedView(const application::BendingDrawing &, DrawingViewKey,
                                      const std::vector<DrawingViewChoice> *);
void paintPlaced(QPainter &, const application::BendingDrawing &, const DrawingPlacedView &,
                 DrawingFlatSide, std::vector<QRectF> &, QRectF *selectionBounds,
                 LayoutContext &);
void recordViews(DrawingSheetPlan &, LayoutContext &);
void paintRecorded(QPainter &, const DrawingPlacedView &, DrawingFlatSide);
void paintCompositionFrame(QPainter &, const application::BendingDrawing &, const DrawingTitle &,
                           const DrawingSheetPlan &, int);
void paintComposition(QPainter &, const application::BendingDrawing &, const DrawingTitle &, int);
} // namespace bending::ui::drawing
