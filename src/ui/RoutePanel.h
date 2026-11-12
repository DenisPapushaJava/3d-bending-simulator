#pragma once
#include "bending/application/BendRoute.h"
#include "bending/application/DocumentSession.h"
#include <QWidget>
#include <functional>
class QLabel;
class QPushButton;
class QComboBox;
class QHBoxLayout;
class QScrollArea;
namespace bending::ui {
[[nodiscard]] QString routeFailureReason(const application::BendRouteError &);
class RoutePanel final : public QWidget {
    Q_OBJECT
  public:
    explicit RoutePanel(QWidget *);
    void refresh(const application::DocumentSession &, application::BendRoutePtr,
                 std::optional<application::BendRouteError>, bool busy, bool preparing,
                 bool toolsAvailable, application::RouteDraftPtr draft = {},
                 std::optional<application::BendRouteError> draftError = {},
                 bool preparingDraft = false);
    void select(int step, bool notify = true);
    void setWithTools(bool);
    [[nodiscard]] bool withTools() const;
    void setExpandedSearch(bool expanded) {
        expandedSearch_ = expanded;
    }
    [[nodiscard]] std::optional<std::size_t> selectedBend() const {
        return selected_ > 0 && operation() < indices_.size() ? std::optional{indices_[operation()]}
                                                              : std::nullopt;
    }
    [[nodiscard]] int selected() const {
        return selected_;
    }
    [[nodiscard]] int stepCount() const {
        return static_cast<int>(ids_.size());
    }
    [[nodiscard]] std::size_t operation() const {
        return selected_ > 0 ? static_cast<std::size_t>(selected_ - 1) : 0;
    }
    [[nodiscard]] QWidget *stepStrip() const {
        return stepStrip_;
    }
    [[nodiscard]] QString statusText() const;
    [[nodiscard]] QWidget *reportWidget() const;
    std::function<void(bool automatic, bool withTools)> onPrepare;
    std::function<void(int)> onSelect;
    std::function<void(std::optional<std::size_t>)> onHover;
    std::function<bool(std::vector<std::string>, bool grouped)> onReorder;
    std::function<void()> onModeChanged;
    std::function<void(std::size_t)> onEditTool;
    std::function<void()> onExpandSearch;

  private:
    void move(int delta);
    void moveTo(int from, int to, bool selectMovedStep);
    void undo();
    void redo();
    struct OrderEdit {
        std::vector<std::string> before, after;
        int previousStep, nextStep;
        bool grouped = false;
    };
    std::vector<OrderEdit> undo_, redo_;
    std::optional<std::vector<std::string>> groupedDragOrder_;
    std::uint64_t documentRevision_ = 0;
    QPushButton *undoButton_, *redoButton_;
    application::BendRoutePtr route_;
    std::vector<std::string> ids_;
    std::vector<std::string> sourceIds_;
    std::vector<std::vector<std::string>> draftGroups_;
    std::vector<std::size_t> indices_;
    std::vector<QPushButton *> steps_;
    int selected_ = 0;
    QComboBox *mode_;
    QPushButton *automatic_, *manual_, *earlier_, *later_, *editTool_, *expandSearch_;
    bool expandedSearch_ = false;
    QLabel *status_;
    QHBoxLayout *strip_;
    QWidget *stepStrip_;
    QScrollArea *stepScroll_;
};
} // namespace bending::ui
