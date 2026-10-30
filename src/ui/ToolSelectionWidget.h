#pragma once

#include "bending/application/ToolProfileImport.h"
#include "bending/core/ToolCatalog.h"
#include "bending/core/ToolSelection.h"
#include <QPointer>
#include <QWidget>
#include <filesystem>
#include <functional>
#include <optional>
#include <variant>

class QComboBox;
class QCheckBox;
class QDoubleSpinBox;
class QLabel;
class QLineEdit;
class QPushButton;
class QToolButton;
class QDialog;

namespace bending::ui {
// UI drafts and local preferences; only MainWindow commits a choice to DocumentSession.
class ImportController;
class ToolSelectionWidget final : public QWidget {
  public:
    ToolSelectionWidget(core::ToolCatalog catalog, QWidget *parent);
    void setChoice(const std::optional<core::PressTools> &tools,
                   const std::optional<core::ToolAssignment> &assignment);
    [[nodiscard]] std::optional<core::PressTools> parameters() const;
    // Display only: a geometrically valid profile may still fail the die/thickness rule.
    [[nodiscard]] std::optional<core::PressTools> previewParameters() const {
        return rawParameters();
    }
    [[nodiscard]] std::optional<core::ToolAssignment> assignment() const;
    [[nodiscard]] bool usesCatalog() const;
    [[nodiscard]] QString summary() const;
    [[nodiscard]] QString openingIssue() const;
    [[nodiscard]] QString shortSummary() const;
    void rememberChoice();
    [[nodiscard]] std::optional<core::ToolPreset> selectedTool(core::ToolKind) const;
    [[nodiscard]] std::size_t selectedGroove() const;
    [[nodiscard]] std::optional<core::ToolAssembly> selectedAssembly(core::ToolKind kind) const {
        return kind == core::ToolKind::Punch ? punchAssembly_ : dieAssembly_;
    }
    [[nodiscard]] bool punchReversed() const {
        return punchReversed_;
    }
    [[nodiscard]] QString optionIssue(const core::ToolPreset &, std::size_t groove) const;
    [[nodiscard]] QString geometryIssue() const;
    [[nodiscard]] static QString assignmentIssue(const std::optional<core::ToolAssignment> &);
    void showPicker(core::ToolKind, QWidget *anchor);
    bool chooseTool(const core::ToolPreset &, std::size_t groove);
    void setSelectionRequest(std::optional<core::ToolSelectionRequest> request,
                             bool allBends = false);
    void setToolProfileImport(ImportController &,
                              std::shared_ptr<const application::ToolProfileImporter>,
                              application::ToolLibraryWriter);
    std::function<std::variant<core::ToolCatalog, std::string>(const std::filesystem::path &)>
        loadCatalog;
    std::function<void()> onChanged;
    std::function<void()> onAppearanceChanged;

  private:
    struct Pair {
        QString punchId;
        QString dieId;
        int groove = 0;
        bool operator==(const Pair &) const = default;
    };
    [[nodiscard]] std::optional<core::PressTools> rawParameters() const;
    bool chooseDrawingGeometry();
    void changed();
    void updateDetails();
    void populateGrooves(std::size_t selected = 0);
    void selectPreset(QComboBox *combo, const core::ToolPreset &preset);
    [[nodiscard]] const core::ToolPreset *preset(const QComboBox *combo) const;
    [[nodiscard]] Pair currentPair() const;
    void selectPair(const Pair &pair);
    void refreshFavorites();
    void recommend();
    void fitAssemblies();
    void showDrawings();
    void importCatalog();
    void importDxf();
    QString saveImportedTool(core::ToolPreset);
    core::ToolCatalog catalog_;
    std::size_t workshopSize_ = 0;
    QCheckBox *realGeometry_;
    QCheckBox *demoMachine_;
    QLabel *geometryScope_;
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
    QWidget *catalogDetails_;
    QToolButton *detailsToggle_;
    QPushButton *drawings_;
    QPushButton *recommend_;
    QPushButton *stock_;
    QDoubleSpinBox *segmentGap_;
    QLabel *assemblySummary_;
    QPushButton *importDxf_;
    QLabel *recommendation_;
    std::optional<core::ToolSelectionRequest> selectionRequest_;
    bool allBends_ = false;
    QDoubleSpinBox *clearance_;
    std::vector<Pair> favoritePairs_;
    bool updating_ = false;
    QPointer<QDialog> picker_;
    bool punchReversed_ = false;
    core::MachineModel machineModel_ = core::MachineModel::DemoPressV2;
    std::optional<core::ToolAssembly> punchAssembly_, dieAssembly_;
    ImportController *controller_ = nullptr;
    std::shared_ptr<const application::ToolProfileImporter> profileImporter_;
    application::ToolLibraryWriter libraryWriter_;
};
} // namespace bending::ui
