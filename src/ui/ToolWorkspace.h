#pragma once
#include "bending/core/ToolCatalog.h"
#include <QWidget>
#include <optional>
class QLabel;
namespace bending::ui {
class ToolProfilesWidget;
// Passive workspace; choices and inventory remain owned by ToolSelectionWidget.
class ToolWorkspace final : public QWidget {
  public:
    explicit ToolWorkspace(QWidget *parent = nullptr);
    void setChoice(std::optional<core::PressTools>, std::optional<core::ToolAssignment>);

  private:
    ToolProfilesWidget *profiles_;
    QLabel *punch_, *die_;
    std::optional<core::PressTools> tools_;
    std::optional<core::ToolAssignment> assignment_;
};
} // namespace bending::ui
