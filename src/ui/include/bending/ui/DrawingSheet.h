#pragma once
#include "bending/application/BendingDrawing.h"
#include <QImage>
#include <QRectF>
#include <QString>

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
struct DrawingPlacedView {
    DrawingViewKey key;
    int page = 0;
    QRectF area;
    QPointF centre;
    double scale = 1;
    QRectF selectionBounds = {}; // Body and annotations; independent of allocated layout cell.
};
struct DrawingSheetPlan {
    application::BendingDrawingPtr source;
    std::vector<DrawingViewChoice> choices;
    DrawingFlatSide flatSide = DrawingFlatSide::Automatic;
    std::vector<DrawingPlacedView> views;
    int pages = 0;
    application::DrawingMeasurementCoverage coverage;
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
            DrawingFlatSide = DrawingFlatSide::Automatic);
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
