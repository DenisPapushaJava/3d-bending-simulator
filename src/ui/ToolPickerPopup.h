#pragma once
#include "bending/core/ToolCatalog.h"
#include <QDialog>
#include <functional>
#include <optional>
namespace bending::ui {
class ToolPickerPopup final : public QDialog {
  public:
    enum { ImportDxf = 2 };
    ToolPickerPopup(core::ToolCatalog, core::ToolKind,
                    const std::optional<core::ToolPreset> &current, std::size_t groove,
                    std::function<QString(const core::ToolPreset &, std::size_t)>, bool canImport,
                    QWidget *anchor, bool punchReversed);
    [[nodiscard]] const std::optional<std::pair<core::ToolPreset, std::size_t>> &choice() const {
        return choice_;
    }

  private:
    void showEvent(QShowEvent *) override;
    void positionAtAnchor();
    core::ToolCatalog tools_;
    std::optional<std::pair<core::ToolPreset, std::size_t>> choice_;
};
} // namespace bending::ui
