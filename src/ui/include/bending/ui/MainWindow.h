#pragma once

#include "bending/core/ToolCatalog.h"
#include <QMainWindow>

#include "bending/application/BendingDrawing.h"
#include "bending/application/DocumentImporter.h"
#include "bending/application/FlatPattern.h"
#include "bending/application/ProjectWriter.h"
#include "bending/application/SingleBendPreview.h"
#include "bending/application/ToolLibrary.h"
#include "bending/application/ToolProfileImport.h"
#include <filesystem>
#include <functional>
#include <memory>
#include <string_view>
#include <variant>

namespace bending::update {
class UpdateClient;
}
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
    // Interactive imports prompt by default; scripted workflows may collect inputs themselves.
    void setAutomaticPartInputPrompt(bool enabled);
    [[nodiscard]] ImportController &importController() const;
    void setToolCatalogLoader(
        std::function<std::variant<core::ToolCatalog, std::string>(const std::filesystem::path &)>
            loader);
    void setToolProfileImport(std::shared_ptr<const application::ToolProfileImporter>,
                              application::ToolLibraryStorage, core::ToolCatalog starter = {});
    void setDrawingBuilder(std::shared_ptr<const application::BendingDrawingBuilder> builder);
    void setFlatPatternBuilder(std::shared_ptr<const application::FlatPatternBuilder> builder);
    // Borrowed client; app keeps it alive until after this window is destroyed.
    void setUpdateClient(update::UpdateClient *client);
    void requestUpdateRestart(std::function<QString(const QString &)> installer);

  protected:
    void closeEvent(QCloseEvent *event) override;
    bool eventFilter(QObject *watched, QEvent *event) override;

  private:
    class Impl;
    std::unique_ptr<Impl> impl_;
};
} // namespace bending::ui
