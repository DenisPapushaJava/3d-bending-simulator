#pragma once

#include <QMainWindow>

#include "bending/application/DocumentImporter.h"
#include "bending/application/FemExperiment.h"
#include "bending/application/ProjectWriter.h"
#include "bending/application/SingleBendPreview.h"
#include <memory>
#include <string_view>

namespace bending::ui {
class ImportController;

// Views are borrowed for construction only; the window copies them into Qt strings.
struct RuntimeInfo {
    std::string_view applicationVersion;
    std::string_view cadVersion;
};

class MainWindow final : public QMainWindow {
  public:
    MainWindow(RuntimeInfo runtime, std::shared_ptr<const application::DocumentImporter> importer,
               std::shared_ptr<const application::ProjectWriter> writer = {},
               std::shared_ptr<const application::SingleBendPreviewBuilder> previewBuilder = {},
               std::shared_ptr<application::FemRunner> femRunner = {});
    ~MainWindow() override;
    void openStep(const QString &path);
    [[nodiscard]] ImportController &importController() const;

  protected:
    void closeEvent(QCloseEvent *event) override;

  private:
    class Impl;
    std::unique_ptr<Impl> impl_;
};
} // namespace bending::ui
