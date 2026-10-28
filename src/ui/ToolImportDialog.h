#pragma once
#include "bending/application/ToolProfileImport.h"
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
namespace bending::ui {
class ImportController;
class ToolImportDialog final : public QDialog {
    Q_OBJECT
  public:
    ToolImportDialog(ImportController &, std::shared_ptr<const application::ToolProfileImporter>,
                     core::ToolCatalog, std::function<QString(core::ToolPreset)>, QWidget *);
  public slots:
    void loadFile(const QString &);
    void accept() override;
    void reject() override;

  private:
    void updatePreview();
    void selectTarget();
    void setBusy(bool);
    std::optional<core::ToolDrawing> drawing() const;
    std::optional<core::ToolPreset> preset() const;
    ImportController &controller_;
    std::shared_ptr<const application::ToolProfileImporter> importer_;
    core::ToolCatalog targets_;
    std::function<QString(core::ToolPreset)> save_;
    std::optional<application::ToolProfileFile> file_;
    QComboBox *target_, *kind_, *contour_, *units_;
    QLineEdit *code_, *manufacturer_, *openings_;
    QDoubleSpinBox *length_, *angle_, *radius_, *rotation_;
    QCheckBox *mirror_;
    QLabel *status_, *notices_, *source_;
    QWidget *form_, *previewHost_;
    QVBoxLayout *previewLayout_;
    QPushButton *saveButton_, *chooseButton_;
    bool busy_ = false;
};
} // namespace bending::ui
