#pragma once

#include "bending/application/BendingDrawing.h"
#include "bending/application/DocumentSession.h"
#include "bending/application/ProjectWriter.h"
#include "bending/application/SingleBendPreview.h"

#include <QObject>
#include <QString>

#include <functional>
#include <memory>

namespace bending::ui {
class ImportController final : public QObject {
    Q_OBJECT
  public:
    // Invoked on the GUI thread before committing the candidate. Empty text means success.
    using Presenter = std::function<QString(application::DocumentPtr, std::uint64_t)>;
    ImportController(std::shared_ptr<const application::DocumentImporter> importer,
                     Presenter presenter, QObject *parent = nullptr);
    ~ImportController() override;
    void open(std::filesystem::path path);
    void cancel();
    void closeDocument();
    void shutdown();
    application::ThicknessValidation setManualThickness(double valueMm);
    void useAutomaticThickness();
    bool setBendReview(std::size_t recognizedIndex, application::BendReview review);
    bool setBendTools(std::size_t recognizedIndex, std::optional<core::PressTools> tools);
    bool setBendToolAssignment(std::size_t recognizedIndex, core::ToolAssignment assignment);
    bool setBendTarget(std::size_t recognizedIndex, std::optional<core::BendTarget> target);
    void saveProject(std::filesystem::path path,
                     std::shared_ptr<const application::ProjectWriter> writer);
    [[nodiscard]] bool isSaving() const;
    [[nodiscard]] QString saveError() const;
    [[nodiscard]] const application::DocumentSession &session() const;
    [[nodiscard]] bool hasWork() const;
    bool
    prepareBendPreview(std::size_t recognizedIndex, bool fixedSecond,
                       std::shared_ptr<const application::SingleBendPreviewBuilder> builder,
                       application::BendPreviewMode mode = application::BendPreviewMode::Kinematic);
    void discardBendPreview();
    [[nodiscard]] application::BendPreviewPtr bendPreview() const;
    [[nodiscard]] const std::optional<application::BendPreviewError> &bendPreviewError() const;
    [[nodiscard]] bool isPreparingPreview() const;
    bool prepareDrawing(std::shared_ptr<const application::BendingDrawingBuilder> builder,
                        application::DrawingRequest request);
    [[nodiscard]] application::BendingDrawingPtr drawing() const;
    [[nodiscard]] std::optional<application::DrawingError> drawingError() const;
    [[nodiscard]] bool isPreparingDrawing() const;
    void cancelDrawing();

  signals:
    void stateChanged();

  private:
    class Impl;
    std::unique_ptr<Impl> impl_;
};
} // namespace bending::ui
