#pragma once
#include "bending/core/ToolCatalog.h"
#include <QPainterPath>
#include <QToolButton>
#include <optional>
namespace bending::ui {
// A real contour only. A missing drawing has an explicit empty state, never a generated wedge.
class ToolContourButton final : public QToolButton {
  public:
    explicit ToolContourButton(core::ToolKind, QWidget *parent = nullptr, bool compact = false);
    void setTool(std::optional<core::ToolPreset>, std::size_t groove = 0, bool reversed = false);
    void setIssue(QString);

  protected:
    void paintEvent(QPaintEvent *) override;
    void keyPressEvent(QKeyEvent *) override;

  private:
    core::ToolKind kind_;
    bool compact_;
    std::optional<core::ToolPreset> tool_;
    std::size_t groove_ = 0;
    bool reversed_ = false;
    QPainterPath path_;
    QString heading_, dimensions_, issue_;
};
} // namespace bending::ui
