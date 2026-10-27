#pragma once

#include "bending/core/PressTools.h"
#include <QWidget>
#include <optional>

namespace bending::ui {
// Passive 2D rendering of the same idealized profiles used by the CAD adapter.
class ToolProfilesWidget final : public QWidget {
  public:
    explicit ToolProfilesWidget(QWidget *parent = nullptr, bool enlarged = false);
    void setTools(std::optional<core::PressTools> tools);

  protected:
    void paintEvent(QPaintEvent *event) override;

  private:
    std::optional<core::PressTools> tools_;
    bool enlarged_;
};
} // namespace bending::ui
