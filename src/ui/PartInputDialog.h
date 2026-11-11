#pragma once
#include "bending/application/DocumentSession.h"
#include <QDialog>

class QComboBox;
class QDoubleSpinBox;
class QLabel;
class QLineEdit;
class QPushButton;
namespace bending::ui {
class ImportController;
QString partInputsSummary(const application::DocumentSession &session);
class PartInputDialog final : public QDialog {
  public:
    PartInputDialog(ImportController &controller, QWidget *parent);

  private:
    void refreshDraft();
    void applyMaterialRule();
    [[nodiscard]] std::optional<core::UnfoldSettings> selectedMaterialRule() const;
    void refreshFactor();
    ImportController &controller_;
    application::DocumentPtr document_;
    std::uint64_t revision_, editRevision_;
    std::optional<double> manual_, automatic_;
    core::UnfoldSettings settings_;
    std::optional<core::UnfoldSettings> categoryRule_;
    bool applying_ = false;
    bool validFactor_ = true;
    QComboBox *material_;
    QDoubleSpinBox *thickness_;
    QLabel *origin_, *error_;
    QLineEdit *factor_;
    QPushButton *continue_, *automaticButton_, *categoryFactorButton_;
};
} // namespace bending::ui
