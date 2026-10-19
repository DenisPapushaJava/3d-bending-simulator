#pragma once

#include "bending/core/ToolCatalog.h"
#include <QWidget>
#include <array>
#include <functional>
#include <optional>

class QComboBox;
class QDoubleSpinBox;
class QLabel;
class QLineEdit;
class QPushButton;
class QToolButton;

namespace bending::ui {
// UI drafts and local preferences; only MainWindow commits a choice to DocumentSession.
class ToolProfilesWidget;
class ToolSelectionWidget final : public QWidget {
  public:
    ToolSelectionWidget(core::ToolCatalog catalog, QWidget *parent);
    void setChoice(const std::optional<core::PressTools> &tools,
                   const std::optional<core::ToolAssignment> &assignment);
    [[nodiscard]] std::optional<core::PressTools> parameters() const;
    [[nodiscard]] std::optional<core::ToolAssignment> assignment() const;
    [[nodiscard]] bool usesCatalog() const;
    [[nodiscard]] QString summary() const;
    [[nodiscard]] QString shortSummary() const;
    void rememberChoice();
    std::function<void()> onChanged;

  private:
    struct Pair {
        QString punchId;
        QString dieId;
        int groove = 0;
        bool operator==(const Pair &) const = default;
    };
    void changed();
    void updateDetails();
    void populateGrooves(std::size_t selected = 0);
    void selectPreset(QComboBox *combo, const core::ToolPreset &preset);
    [[nodiscard]] const core::ToolPreset *preset(const QComboBox *combo) const;
    [[nodiscard]] Pair currentPair() const;
    void selectPair(const Pair &pair);
    void refreshFavorites();
    core::ToolCatalog catalog_;
    QComboBox *mode_;
    QComboBox *punch_;
    QComboBox *die_;
    QComboBox *groove_;
    QComboBox *favorites_;
    QPushButton *favorite_;
    QLabel *details_;
    QLabel *sources_;
    QLineEdit *machine_;
    QWidget *catalogPanel_;
    QWidget *manualPanel_;
    QWidget *catalogDetails_;
    QToolButton *detailsToggle_;
    ToolProfilesWidget *profiles_;
    QPushButton *enlargeProfiles_;
    std::array<QDoubleSpinBox *, 7> fields_{};
    std::vector<Pair> favoritePairs_;
    bool updating_ = false;
};
} // namespace bending::ui
