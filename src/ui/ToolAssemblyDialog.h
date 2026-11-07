#pragma once
#include "bending/core/ToolCatalog.h"
#include <QDialog>
#include <optional>

class QLabel;
class QPushButton;
class QTableWidget;
class QDoubleSpinBox;
namespace bending::ui {
class ToolEndButtons;
// Edits one captured assembly; workshop stock and the other tool are never changed.
class ToolAssemblyDialog final : public QDialog {
  public:
    ToolAssemblyDialog(core::ToolPreset, std::optional<core::ToolAssembly>, QWidget *parent,
                       std::optional<double> requiredMm = {}, double maximumGapMm = 0);
    void selectPiece(std::size_t);
    [[nodiscard]] const std::optional<core::ToolAssembly> &assembly() const {
        return result_;
    }

  private:
    void rebuild(int selected);
    void refresh();
    void setEnd(std::size_t side, std::optional<std::size_t>);
    void fitMiddle();
    core::ToolPreset tool_;
    core::ToolAssembly draft_;
    std::optional<core::ToolAssembly> initial_, result_;
    QTableWidget *rows_;
    QWidget *diagram_;
    QLabel *status_;
    QPushButton *apply_, *add_, *remove_, *up_, *down_;
    ToolEndButtons *ends_ = nullptr;
    QDoubleSpinBox *target_ = nullptr;
    QPushButton *fitMiddle_ = nullptr;
    double maximumGapMm_;
    QString notice_;
};
} // namespace bending::ui
