#pragma once
#include <QFrame>
#include <QString>
#include <vector>
class QLabel;
class QScrollArea;
class QToolButton;
namespace bending::ui {
enum class BendMessageKind {
    Collision,
    ToolLength,
    Support,
    CheckIssue,
    Approximation,
    Scope,
    Backgauge,
    SetupChange
};
enum class BendMessageSeverity { Information, Warning, Error };
struct BendMessage {
    BendMessageKind kind;
    BendMessageSeverity severity;
    QString title;
    QString detail;
    bool operator==(const BendMessage &) const = default;
};
// Current scene messages only: no timers, history, CAD access or document mutation.
class BendMessagePanel final : public QFrame {
    Q_OBJECT
  public:
    explicit BendMessagePanel(QWidget *parent = nullptr);
    void setMessages(std::vector<BendMessage>);
    void setActive(bool);
    QSize sizeHint() const override;

  private:
    void refresh();
    bool active_ = false;
    std::vector<BendMessage> messages_;
    QToolButton *heading_;
    QLabel *summary_, *details_;
    QScrollArea *scroll_;
};
} // namespace bending::ui
