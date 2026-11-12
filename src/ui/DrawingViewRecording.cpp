#include "DrawingSheetInternal.h"
#include <QPicture>

namespace bending::ui::drawing {
void recordViews(DrawingSheetPlan &plan, LayoutContext &context) {
    for (int page = 0; page < plan.pages; ++page) {
        std::vector<QRectF> occupied;
        for (auto &at : plan.views) {
            if (at.page != page)
                continue;
            context.checkpoint();
            QPicture picture;
            QPainter painter(&picture);
            painter.setRenderHint(QPainter::Antialiasing);
            at.dimensions.clear();
            context.dimensions = &at.dimensions;
            at.omittedDimensions = 0;
            context.omittedDimensions = &at.omittedDimensions;
            paintPlaced(painter, *plan.source, at, plan.flatSide, occupied, &at.selectionBounds,
                        context);
            context.dimensions = nullptr;
            context.omittedDimensions = nullptr;
            plan.omittedDimensions += at.omittedDimensions;
            if (!painter.end())
                throw std::runtime_error("Не удалось подготовить изображение вида");
            context.checkpoint();
            const auto bytes = static_cast<std::size_t>(picture.size());
            if (context.bytes > context.limits.maximumRecordingBytes ||
                bytes > context.limits.maximumRecordingBytes - context.bytes)
                throw DrawingLayoutInterrupted(
                    DrawingLayoutInterruption::LimitExceeded,
                    "Достигнут лимит памяти чертежа. Уменьшите число видов.");
            context.bytes += bytes;
            at.recording = std::make_shared<const DrawingViewRecording>(DrawingViewRecording{
                QByteArray(picture.data(), picture.size()), at.key, at.page, at.area, at.centre,
                at.scale, plan.flatSide, at.compactDimensions, at.sheetLinks,
                at.essentialDimensions, at.omittedDimensions, at.retainedDimensions,
                at.smallOptionalText});
        }
    }
}

void paintRecorded(QPainter &painter, const DrawingPlacedView &at, DrawingFlatSide side) {
    const auto &record = at.recording;
    if (!record || record->key != at.key || record->page != at.page || record->area != at.area ||
        record->centre != at.centre || record->scale != at.scale || record->side != side ||
        record->compactDimensions != at.compactDimensions || record->sheetLinks != at.sheetLinks ||
        record->essentialDimensions != at.essentialDimensions ||
        record->omittedDimensions != at.omittedDimensions ||
        record->retainedDimensions != at.retainedDimensions ||
        record->smallOptionalText != at.smallOptionalText)
        throw std::runtime_error("Устаревшее изображение вида. Повторите компоновку.");
    // QPicture playback keeps vector paths and text in the PDF. Each replay owns
    // its stream cursor; immutable commands can be shared by preview/export.
    QPicture picture;
    picture.setData(record->commands.constData(), static_cast<uint>(record->commands.size()));
    painter.save();
    // QPicture applies its own device-DPI conversion. Our coordinates are already
    // paper millimetres, including the caller's preview/PDF transform.
    painter.scale(static_cast<double>(picture.logicalDpiX()) / painter.device()->logicalDpiX(),
                  static_cast<double>(picture.logicalDpiY()) / painter.device()->logicalDpiY());
    const bool ok = picture.play(&painter);
    painter.restore();
    if (!ok)
        throw std::runtime_error("Не удалось отобразить подготовленный вид");
}
} // namespace bending::ui::drawing
