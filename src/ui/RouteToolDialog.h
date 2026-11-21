#pragma once

#include "bending/core/ToolCatalog.h"

#include <QDialog>
#include <array>
#include <functional>

class QLabel;
class QPushButton;
namespace bending::ui {
class ToolContourButton;

class RouteToolDialog final : public QDialog {
  public:
    enum { Advanced = 2 };
    struct Tool {
        core::ToolPreset profile;
        std::optional<core::ToolAssembly> assembly;
        std::size_t groove = 0;
        bool reversed = false;
        bool operator==(const Tool &) const = default;
    };
    using Changes = std::array<std::optional<Tool>, 2>; // Punch, die.
    RouteToolDialog(QWidget *parent, QString scope, core::ToolCatalog, Changes displayed,
                    std::array<bool, 2> mixed, double span, std::optional<double> thickness,
                    double maximumGap, bool advanced,
                    std::function<QString(const Changes &)> validate);
    void openFor(core::ToolKind, std::optional<std::size_t> piece);

  private:
    struct Row {
        ToolContourButton *profile = nullptr;
        QPushButton *sections = nullptr;
        QLabel *summary = nullptr;
        bool mixed = false;
        std::optional<std::size_t> piece;
    };
    void pick(std::size_t);
    void editSections(std::size_t, std::optional<std::size_t> piece = {});
    void refreshRow(std::size_t);
    core::ToolCatalog catalog_;
    Changes displayed_;
    Changes changes_;
    std::array<Row, 2> rows_;
    double span_, maximumGap_;
    std::optional<double> thickness_;
    QLabel *error_;
    QPushButton *apply_;
};
} // namespace bending::ui
