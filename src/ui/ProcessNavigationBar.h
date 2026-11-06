#pragma once
#include <QToolBar>
#include <array>

class QAction;
class QButtonGroup;
namespace bending::ui {
class ProcessStepButton;

// Passive navigation: the window decides whether/how to enter each stage.
// Stage IDs are 0: part, 1: tools, 2: bending. Drawing remains a separate action.
class ProcessNavigationBar final : public QToolBar {
    Q_OBJECT
  public:
    explicit ProcessNavigationBar(QAction &drawing, QWidget *parent = nullptr);
    void setCurrentStage(int stage);
    void setStageStatus(int stage, const QString &status);

  signals:
    void stageRequested(int stage);

  private:
    QButtonGroup *group_;
    std::array<ProcessStepButton *, 3> steps_{};
};
} // namespace bending::ui
