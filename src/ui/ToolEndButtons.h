#pragma once
#include "bending/core/ToolCatalog.h"
#include <QWidget>
#include <array>
#include <functional>
#include <memory>
class QToolButton;
namespace bending::ui {
// Passive shared controls for the workspace and detailed editor.
class ToolEndButtons final : public QWidget {
  public:
    explicit ToolEndButtons(QWidget *parent = nullptr);
    void setSource(const core::ToolPreset &, const std::optional<core::ToolAssembly> &);
    std::function<void(const core::ToolPreset &, bool right, std::optional<std::size_t>)>
        onSelected;

  private:
    std::shared_ptr<const core::ToolPreset> source_;
    std::array<QToolButton *, 2> buttons_{};
    std::array<std::optional<std::size_t>, 2> chosen_{};
};
} // namespace bending::ui
