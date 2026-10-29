#pragma once
#include "bending/core/ToolCatalog.h"
#include <QDialog>
#include <functional>
class QCheckBox;
class QComboBox;
class QLabel;
class QTableWidget;
class QSpinBox;
class QPushButton;
namespace bending::ui {
class ToolStockDialog final : public QDialog {
    Q_OBJECT
  public:
    ToolStockDialog(core::ToolCatalog, std::function<QString(core::ToolPreset)>, QWidget *);
    std::function<void()> onColorsChanged;

  private:
    void load();
    void add(core::ToolSegment);
    void addSet(bool available);
    void updateSet();
    void updateColors();
    void updateEditing();
    void chooseColor(double);
    void resetColor(double);
    core::ToolKind toolKind() const;
    void save();
    core::ToolCatalog catalog_;
    std::function<QString(core::ToolPreset)> writer_;
    QComboBox *tool_;
    QComboBox *set_;
    QSpinBox *copies_;
    QPushButton *addSet_;
    QLabel *setDescription_;
    QCheckBox *enabled_;
    QTableWidget *rows_;
    QLabel *status_;
    bool loading_ = false;
    bool inventoryEdited_ = false;
};
} // namespace bending::ui
