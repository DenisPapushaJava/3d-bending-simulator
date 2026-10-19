#pragma once

#include "bending/core/ToolCatalog.h"
#include <QMainWindow>

#include "bending/application/DocumentImporter.h"
#include "bending/application/ProjectWriter.h"
#include "bending/application/SingleBendPreview.h"
#include <filesystem>
#include <functional>
#include <memory>
#include <string_view>
#include <variant>

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
               core::ToolCatalog catalog = {});
    ~MainWindow() override;
    void openStep(const QString &path);
    [[nodiscard]] ImportController &importController() const;
    void setToolCatalogLoader(
        std::function<std::variant<core::ToolCatalog, std::string>(const std::filesystem::path &)>
            loader);

  protected:
    void closeEvent(QCloseEvent *event) override;

  private:
    class Impl;
    std::unique_ptr<Impl> impl_;
};
} // namespace bending::ui
