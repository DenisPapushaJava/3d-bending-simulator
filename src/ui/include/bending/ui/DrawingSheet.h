#pragma once
#include "bending/application/BendingDrawing.h"
#include <QImage>
#include <QString>

class QPainter;
namespace bending::ui {
struct DrawingTitle {
    QString name;
    QString designation;
    QString material;
    QString execution;
    std::optional<int> quantity;
};
class DrawingSheet final {
  public:
    static constexpr double widthMm = 420;
    static constexpr double heightMm = 297;
    [[nodiscard]] static int pageCount(const application::BendingDrawing &drawing);
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
