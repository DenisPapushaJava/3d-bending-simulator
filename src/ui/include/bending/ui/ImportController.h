#pragma once

#include "bending/application/DocumentSession.h"
#include "bending/application/ProjectWriter.h"

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
    void saveProject(std::filesystem::path path,
                     std::shared_ptr<const application::ProjectWriter> writer);
    [[nodiscard]] bool isSaving() const;
    [[nodiscard]] QString saveError() const;
    [[nodiscard]] const application::DocumentSession &session() const;
    [[nodiscard]] bool hasWork() const;

  signals:
    void stateChanged();

  private:
    class Impl;
    std::unique_ptr<Impl> impl_;
};
} // namespace bending::ui
