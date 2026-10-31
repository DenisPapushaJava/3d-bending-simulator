#pragma once
#include "bending/application/ToolProfileImport.h"
#include "bending/core/ToolGeometry.h"
#include <QDialog>
#include <functional>
#include <memory>
class QComboBox;
class QLineEdit;
class QDoubleSpinBox;
class QCheckBox;
class QLabel;
class QPushButton;
class QVBoxLayout;
class QTableWidget;
class QDragEnterEvent;
class QDragMoveEvent;
class QDropEvent;
class QToolButton;
class QHBoxLayout;
namespace bending::ui {
class ImportController;
class ToolDrawingWidget;
class ToolImportDialog final : public QDialog {
    Q_OBJECT
  public:
    ToolImportDialog(ImportController &, std::shared_ptr<const application::ToolProfileImporter>,
                     core::ToolCatalog, std::function<QString(core::ToolPreset)>, QWidget *,
                     std::optional<core::ToolKind> initialKind = {});
  public slots:
    void loadFile(const QString &);
    void accept() override;
    void reject() override;

  protected:
    void dragEnterEvent(QDragEnterEvent *) override;
    void dragMoveEvent(QDragMoveEvent *) override;
    void dropEvent(QDropEvent *) override;

  private:
    void updatePreview();
    void selectTarget();
    void setBusy(bool);
    void measureProfile(const std::optional<core::ToolDrawing> &);
    void showGrooves();
    std::optional<core::ToolDrawing> drawing() const;
    std::optional<core::ToolPreset> preset() const;
    ImportController &controller_;
    std::shared_ptr<const application::ToolProfileImporter> importer_;
    core::ToolCatalog targets_;
    std::function<QString(core::ToolPreset)> save_;
    std::optional<application::ToolProfileFile> file_;
    QComboBox *target_, *kind_, *contour_, *units_;
    QLineEdit *code_, *manufacturer_;
    QDoubleSpinBox *length_, *rotation_;
    QCheckBox *mirror_;
    QLabel *status_, *notices_, *source_, *sizeLabel_;
    QWidget *form_, *previewHost_, *advanced_, *grooveHost_;
    QToolButton *advancedToggle_;
    QVBoxLayout *previewLayout_;
    QHBoxLayout *grooveButtons_;
    ToolDrawingWidget *preview_ = nullptr;
    QLabel *emptyPreview_;
    QPushButton *saveButton_, *chooseButton_;
    QTableWidget *grooves_;
    std::vector<core::DieGroove> grooveSettings_;
    std::vector<double> measuredOpenings_;
    std::optional<core::DetectedPunchProfile> measuredPunch_;
    std::optional<core::ToolProfileSize> measuredSize_;
    std::optional<core::ToolProfileSize> previewSize_;
    std::optional<core::ToolDrawing> measuredDrawing_, previewDrawing_;
    int measuredKind_ = -1;
    bool busy_ = false;
    bool kindChosen_ = false;
};
} // namespace bending::ui
