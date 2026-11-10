#pragma once
#include "bending/application/DocumentSession.h"
#include <QDialog>

class QComboBox;
class QDoubleSpinBox;
class QLabel;
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
    ImportController &controller_;
    application::DocumentPtr document_;
    std::uint64_t revision_, editRevision_;
    std::optional<double> manual_, automatic_;
    core::UnfoldSettings settings_;
    bool applying_ = false;
    QComboBox *material_;
    QDoubleSpinBox *thickness_;
    QLabel *origin_, *error_, *rule_;
    QPushButton *continue_, *automaticButton_;
};
} // namespace bending::ui
