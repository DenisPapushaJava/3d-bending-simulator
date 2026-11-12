#pragma once
#include "bending/application/ProjectDrawingSettings.h"
#include <QImage>
#include <QLineF>
#include <QRectF>
#include <QString>
#include <chrono>
#include <stdexcept>
#include <stop_token>
#include <utility>

class QPainter;
namespace bending::ui {
enum class DrawingLayout { Classical, Profiles };
using application::DrawingViewKind;
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
using application::DrawingFlatSide;
using application::DrawingPaperFormat;
struct DrawingFlatOrientation {
    bool reversed = false;
    std::optional<bool> sideA = {};
    std::size_t up = 0, down = 0;
};
enum class DrawingDimensionSource { Axial, Hole, Section, Overall };
using DrawingDimensionId = std::pair<DrawingDimensionSource, std::size_t>;
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
    bool compactDimensions = false;
    QString sheetLinks = {}; // References to related projections on other sheets.
    bool essentialDimensions = false;
    std::size_t omittedDimensions = 0;
    std::vector<DrawingDimensionId> retainedDimensions = {}; // Optional dimensions in fallback.
    bool smallOptionalText = false;
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
    DrawingPaperFormat paperFormat = DrawingPaperFormat::A3;
    std::size_t omittedDimensions = 0;
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
    [[nodiscard]] static QSizeF paperSizeMm(DrawingPaperFormat format) {
        return format == DrawingPaperFormat::A4 ? QSizeF(297, 210) : QSizeF(widthMm, heightMm);
    }
    [[nodiscard]] static QString paperName(DrawingPaperFormat format) {
        return format == DrawingPaperFormat::A4 ? QStringLiteral("A4") : QStringLiteral("A3");
    }
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
            DrawingLayoutLimits = {}, DrawingPaperFormat = DrawingPaperFormat::A3);
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
