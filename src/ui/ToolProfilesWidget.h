#pragma once

#include "bending/core/PressTools.h"
#include "bending/core/ToolCatalog.h"
#include <QWidget>
#include <optional>

namespace bending::ui {
// Passive 2D rendering of the selected idealized or drawing-based tool contours.
class ToolProfilesWidget final : public QWidget {
  public:
    explicit ToolProfilesWidget(QWidget *parent = nullptr, bool enlarged = false);
    void setTools(std::optional<core::PressTools> tools);
    void setDrawingAssignment(std::optional<core::ToolAssignment> assignment);

  protected:
    void paintEvent(QPaintEvent *event) override;

  private:
    std::optional<core::PressTools> tools_;
    bool enlarged_;
    std::optional<core::ToolAssignment> drawing_;
};
} // namespace bending::ui
