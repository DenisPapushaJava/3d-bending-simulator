#pragma once
#include "bending/application/BendRoute.h"
#include "bending/application/DocumentSession.h"
#include <QWidget>
#include <functional>
class QLabel;
class QPushButton;
class QComboBox;
class QHBoxLayout;
namespace bending::ui {
class RoutePanel final : public QWidget {
    Q_OBJECT
  public:
    explicit RoutePanel(QWidget *);
    void refresh(const application::DocumentSession &, application::BendRoutePtr,
                 std::optional<application::BendRouteError>, bool busy, bool preparing,
                 bool toolsAvailable);
    void select(int step);
    void setWithTools(bool, bool perBend = false);
    [[nodiscard]] bool perBendTools() const;
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
    [[nodiscard]] std::size_t operation() const {
        return selected_ > 0 ? static_cast<std::size_t>(selected_ - 1) : 0;
    }
    [[nodiscard]] QWidget *stepStrip() const {
        return stepStrip_;
    }
    std::function<void(bool automatic, bool withTools)> onPrepare;
    std::function<void(int)> onSelect;
    std::function<void(std::vector<std::string>)> onReorder;
    std::function<void()> onModeChanged;
    std::function<void(std::size_t)> onEditTool;
    std::function<void()> onExpandSearch;

  private:
    void resizeEvent(QResizeEvent *) override;
    void updateStatusHeight();
    void move(int delta);
    application::BendRoutePtr route_;
    std::vector<std::string> ids_;
    std::vector<std::size_t> indices_;
    std::vector<QPushButton *> steps_;
    int selected_ = 0;
    QComboBox *mode_;
    QPushButton *automatic_, *manual_, *earlier_, *later_, *editTool_, *expandSearch_;
    bool expandedSearch_ = false;
    QLabel *status_;
    QHBoxLayout *strip_;
    QHBoxLayout *details_;
    QWidget *stepStrip_;
};
} // namespace bending::ui
