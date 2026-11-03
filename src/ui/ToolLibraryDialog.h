#pragma once
#include "bending/application/ToolLibrary.h"
#include "bending/application/ToolProfileImport.h"
#include <QDialog>
#include <functional>
#include <optional>
#include <unordered_map>
class QListWidget;
class QLineEdit;
class QComboBox;
class QCheckBox;
class QLabel;
class QPushButton;
class QStackedWidget;
class QVBoxLayout;
class QDoubleSpinBox;
namespace bending::ui {
class ImportController;
class ToolDrawingWidget;
class ToolStockDialog;
class ToolImportDialog;
class ToolLibraryDialog final : public QDialog {
    Q_OBJECT
  public:
    ToolLibraryDialog(application::ToolLibrary &, ImportController &,
                      std::shared_ptr<const application::ToolProfileImporter>,
                      std::function<QString(const core::ToolPreset &, std::size_t)>, QWidget *,
                      std::optional<core::ToolKind> = {}, std::string selectedId = {},
                      std::size_t groove = 0);
    std::function<void()> onLibraryChanged, onColorsChanged;
    std::function<void()> onImportCatalog;
    [[nodiscard]] const std::optional<std::pair<core::ToolPreset, std::size_t>> &choice() const {
        return choice_;
    }
    void startImport(bool replace = false);
    void accept() override;
    void reject() override;
  public slots:
    void loadFile(const QString &);

  protected:
    void dragEnterEvent(QDragEnterEvent *) override;
    void dragMoveEvent(QDragMoveEvent *) override;
    void dropEvent(QDropEvent *) override;

  private:
    void refreshList();
    void select();
    void loadCard();
    bool saveCurrent();
    void updateGroove();
    void changed();
    std::optional<core::ToolPreset> selected() const;
    application::ToolLibrary &library_;
    ImportController &controller_;
    std::shared_ptr<const application::ToolProfileImporter> importer_;
    std::function<QString(const core::ToolPreset &, std::size_t)> issue_;
    QListWidget *list_;
    QLineEdit *search_, *name_, *manufacturer_;
    QDoubleSpinBox *length_;
    QComboBox *filter_, *groove_;
    QCheckBox *archive_;
    QLabel *status_, *source_, *parameters_, *starterSections_;
    QPushButton *use_, *remove_, *replace_;
    QStackedWidget *pages_;
    QWidget *card_, *stockHost_;
    QVBoxLayout *stockLayout_;
    ToolDrawingWidget *drawing_;
    ToolStockDialog *stock_ = nullptr;
    ToolImportDialog *import_ = nullptr;
    std::string selectedId_;
    std::unordered_map<std::string, int> grooveChoices_;
    bool loading_ = false, showingArchive_ = false;
    std::optional<std::pair<core::ToolPreset, std::size_t>> choice_;
};
} // namespace bending::ui
