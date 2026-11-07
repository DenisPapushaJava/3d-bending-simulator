#pragma once
#include "bending/core/ToolCatalog.h"
#include <QDialog>
#include <QPointer>
#include <functional>
class QCheckBox;
class QComboBox;
class QLabel;
class QTableWidget;
class QSpinBox;
class QPushButton;
namespace bending::ui {
class HornSectionEditor;
class ToolStockDialog final : public QDialog {
    Q_OBJECT
  public:
    ToolStockDialog(core::ToolCatalog, std::function<QString(core::ToolPreset)>, QWidget *,
                    bool embedded = false, std::vector<core::ToolSegment> reference = {});
    std::function<void()> onColorsChanged;
    void selectTool(const std::string &id);
    void markDirty() {
        inventoryEdited_ = true;
    }
    bool apply() {
        save();
        return !inventoryEdited_;
    }

  private:
    void load();
    void add(core::ToolSegment);
    void addSet(bool available);
    void updateSet();
    void updateColors();
    void updateHornNotice();
    void updateHornEditor();
    std::optional<core::ToolSegment> sectionAt(int);
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
    QLabel *hornNotice_;
    HornSectionEditor *hornEditor_;
    QPointer<QComboBox> editingHorn_;
    bool loading_ = false;
    bool inventoryEdited_ = false;
    bool embedded_ = false;
    std::vector<core::ToolSegment> reference_;
};
} // namespace bending::ui
