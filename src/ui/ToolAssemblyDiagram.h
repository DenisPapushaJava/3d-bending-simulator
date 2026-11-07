#pragma once
#include "bending/core/ToolCatalog.h"
#include <QIcon>
#include <QWidget>
#include <functional>

namespace bending::ui {
QIcon toolHornEndIcon(bool right, qreal devicePixelRatio);
// Passive numeric schematic with a captured profile and parent-owned draft callbacks.
class ToolAssemblyDiagram final : public QWidget {
  public:
    explicit ToolAssemblyDiagram(std::optional<core::ToolPreset>, QWidget *parent);
    void setTool(std::optional<core::ToolPreset>);
    std::function<std::optional<std::vector<core::ToolSectionInterval>>()> intervals;
    std::function<double()> required;
    std::function<int()> selected;
    std::function<void(int)> onSelected;

  protected:
    void paintEvent(QPaintEvent *) override;
    void mousePressEvent(QMouseEvent *) override;

  private:
    std::optional<core::ToolPreset> tool;
};
} // namespace bending::ui
