#pragma once
#include "bending/core/ToolCatalog.h"
#include <QDialog>
#include <functional>
class QCheckBox;
class QComboBox;
class QLabel;
class QTableWidget;
namespace bending::ui {
class ToolStockDialog final : public QDialog {
    Q_OBJECT
  public:
    ToolStockDialog(core::ToolCatalog, std::function<QString(core::ToolPreset)>, QWidget *);

  private:
    void load();
    void add(core::ToolSegment);
    void save();
    core::ToolCatalog catalog_;
    std::function<QString(core::ToolPreset)> writer_;
    QComboBox *tool_;
    QCheckBox *enabled_;
    QTableWidget *rows_;
    QLabel *status_;
};
} // namespace bending::ui
