#pragma once
#include "bending/application/BendingDrawing.h"
#include <QImage>
#include <QLineF>
#include <QRectF>
#include <QString>
#include <chrono>
#include <stdexcept>
#include <stop_token>

class QPainter;
namespace bending::ui {
enum class DrawingLayout { Classical, Profiles };
enum class DrawingViewKind { Projection, Section, Profile, Detail, Auxiliary, Flat, Isometric };
struct DrawingViewKey {
    DrawingViewKind kind;
    std::size_t index = 0;
    bool operator==(const DrawingViewKey &) const = default;
};
struct DrawingViewChoice {
    DrawingViewKey key;
    bool visible = true;
    std::optional<double> scale = {}; // Standard paper scale; projections share it.
};
enum class DrawingFlatSide { Automatic, First, Opposite };
struct DrawingFlatOrientation {
    bool reversed = false;
    std::optional<bool> sideA = {};
    std::size_t up = 0, down = 0;
};
enum class DrawingDimensionSource { Axial, Hole, Section, Overall };
struct DrawingPlacedDimension {
    DrawingDimensionSource source;
    std::size_t index;
    application::DrawingDimension measurement;
    QString text;
    QRectF textBounds;
    std::array<QLineF, 3> lines;
};
struct DrawingViewRecording;
struct DrawingPlacedView {
    DrawingViewKey key;
    int page = 0;
    QRectF area;
    QPointF centre;
    double scale = 1;
    QRectF selectionBounds = {}; // Body and annotations; independent of allocated layout cell.
    std::vector<DrawingPlacedDimension> dimensions = {};
    std::shared_ptr<const DrawingViewRecording> recording = {};
};
struct DrawingLayoutLimits {
    std::chrono::milliseconds maximumTime{30000};
    std::size_t maximumDimensionProbes = 1000000;
    std::size_t maximumRecordingBytes = 64 * 1024 * 1024;
    // Numeric fits within one compose only. Zero selects the uncached reference path;
    // larger values are capped at 4096. Exhaustion stops caching, not layout search.
    std::size_t maximumCachedFits = 4096;
};
enum class DrawingLayoutInterruption { Cancelled, LimitExceeded };
class DrawingLayoutInterrupted final : public std::runtime_error {
  public:
    DrawingLayoutInterrupted(DrawingLayoutInterruption cause, const char *message)
        : std::runtime_error(message), reason(cause) {}
    DrawingLayoutInterruption reason;
};
struct DrawingSheetPlan {
    application::BendingDrawingPtr source;
    std::vector<DrawingViewChoice> choices;
    DrawingFlatSide flatSide = DrawingFlatSide::Automatic;
    std::vector<DrawingPlacedView> views;
    int pages = 0;
    application::DrawingMeasurementCoverage coverage;
    std::size_t dimensionProbes = 0;
    std::size_t recordingBytes = 0;
    std::size_t fitCalls = 0, fitCacheHits = 0, fitCacheEntries = 0;
};
struct DrawingTitle {
    QString name;
    QString designation;
    QString material;
    QString execution;
    std::optional<int> quantity;
    DrawingLayout layout = DrawingLayout::Classical;
    std::shared_ptr<const DrawingSheetPlan> plan = {};
};
class DrawingSheet final {
  public:
    static constexpr double widthMm = 420;
    static constexpr double heightMm = 297;
    [[nodiscard]] static std::vector<DrawingViewChoice>
    defaultViews(const application::BendingDrawing &);
    [[nodiscard]] static QString viewName(const application::BendingDrawing &, DrawingViewKey);
    [[nodiscard]] static DrawingFlatOrientation
    flatOrientation(const application::FlatPatternInfo &,
                    DrawingFlatSide = DrawingFlatSide::Automatic);
    // Numeric paper layout, bound to an immutable drawing; no CAD/GPU calls.
    [[nodiscard]] static std::shared_ptr<const DrawingSheetPlan>
    compose(application::BendingDrawingPtr, const std::vector<DrawingViewChoice> &,
            DrawingFlatSide = DrawingFlatSide::Automatic, std::stop_token = {},
            DrawingLayoutLimits = {});
    [[nodiscard]] static int pageCount(const application::BendingDrawing &drawing,
                                       const DrawingTitle &title = {});
    [[nodiscard]] static application::DrawingMeasurementCoverage
    coverage(const application::BendingDrawing &drawing, const DrawingTitle &title = {});
    // Painter coordinates must already be millimetres. Preview and PDF use the same layout.
    static void paint(QPainter &painter, const application::BendingDrawing &drawing,
                      const DrawingTitle &title, int page = 0);
    [[nodiscard]] static QImage preview(const application::BendingDrawing &drawing,
                                        const DrawingTitle &title, int page = 0, int width = 1260);
    // Atomic replacement; error text on failure, no partial target file.
    [[nodiscard]] static QString writePdf(const QString &path,
                                          const application::BendingDrawing &drawing,
                                          const DrawingTitle &title);
};
} // namespace bending::ui
