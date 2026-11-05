#pragma once
#include "bending/core/ToolCatalog.h"
#include <QDialog>
#include <optional>

class QLabel;
class QPushButton;
class QTableWidget;
namespace bending::ui {
// Edits one captured assembly; workshop stock and the other tool are never changed.
class ToolAssemblyDialog final : public QDialog {
  public:
    ToolAssemblyDialog(core::ToolPreset, std::optional<core::ToolAssembly>, QWidget *parent);
    [[nodiscard]] const std::optional<core::ToolAssembly> &assembly() const {
        return result_;
    }

  private:
    void rebuild(int selected);
    void refresh();
    core::ToolPreset tool_;
    core::ToolAssembly draft_;
    std::optional<core::ToolAssembly> initial_, result_;
    QTableWidget *rows_;
    QWidget *diagram_;
    QLabel *status_;
    QPushButton *apply_, *add_, *remove_, *up_, *down_;
};
} // namespace bending::ui
