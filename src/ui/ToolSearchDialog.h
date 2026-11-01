#pragma once
#include "bending/application/BendRoute.h"
#include <QDialog>
#include <array>
#include <functional>
class QLabel;
class QPushButton;
class QProgressBar;
namespace bending::ui {
class ToolSearchDialog final : public QDialog {
    Q_OBJECT
  public:
    explicit ToolSearchDialog(QWidget *parent);
    void begin();
    void progress(const application::BendRouteProgress &, const QString &tool);
    void finish(bool resumable);
    void reject() override;
    std::function<void()> onCancel, onQuick, onManual, onContinue;

  private:
    QLabel *title_, *tool_, *detail_;
    std::array<QLabel *, 3> steps_;
    QProgressBar *busyIndicator_;
    QPushButton *action_;
    bool busy_ = false, resumable_ = false;
};
} // namespace bending::ui
