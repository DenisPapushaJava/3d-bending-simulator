#pragma once
#include "bending/core/ToolSegments.h"
#include <QStringList>
#include <QWidget>
#include <array>
#include <functional>
class QLineEdit;
class QLabel;
class QBoxLayout;
class QPushButton;
namespace bending::ui {
class HornSectionEditor final : public QWidget {
    Q_OBJECT
  public:
    explicit HornSectionEditor(QWidget *parent = nullptr);
    void setSection(double length, double height, core::SegmentShape,
                    std::optional<core::HornRelief>, QStringList draft = {});
    [[nodiscard]] QStringList draft() const;
    static QString read(const QStringList &, double length, double height,
                        std::optional<core::HornRelief> &);
    std::function<void()> onEdited;

  protected:
    void resizeEvent(QResizeEvent *) override;
    bool eventFilter(QObject *, QEvent *) override;

  private:
    void refresh();
    QWidget *diagram_;
    QBoxLayout *body_;
    QLabel *title_, *status_;
    QPushButton *preset_;
    std::array<QLineEdit *, 3> fields_;
    double length_ = 100, height_ = 130;
    core::SegmentShape shape_ = core::SegmentShape::LeftHorn;
    bool loading_ = false;
};
} // namespace bending::ui
