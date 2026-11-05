#pragma once
#include "bending/core/ToolCatalog.h"
#include <QWidget>
#include <optional>
class QLabel;
class QHBoxLayout;
class QPushButton;
namespace bending::ui {
class ToolContourButton;
class ToolSelectionWidget;
// Graphical workspace; choices and inventory remain owned by ToolSelectionWidget.
class ToolWorkspace final : public QWidget {
  public:
    explicit ToolWorkspace(QWidget *parent = nullptr);
    void bindSelection(ToolSelectionWidget *);
    void setContext(const QString &);
    void refresh();

  private:
    ToolSelectionWidget *selection_ = nullptr;
    QWidget *profiles_;
    ToolContourButton *punchProfile_, *dieProfile_;
    QHBoxLayout *grooves_;
    QWidget *grooveRow_;
    std::optional<core::ToolPreset> shownDie_;
    QLabel *context_, *punch_, *die_;
    QPushButton *editPunch_, *editDie_;
};
} // namespace bending::ui
