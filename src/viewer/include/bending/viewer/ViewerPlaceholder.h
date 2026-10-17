#pragma once

#include <QWidget>

namespace bending::viewer {

// Keeps the future OCCT/Qt context adapter separate from the main window.
class ViewerPlaceholder final : public QWidget {
  public:
    explicit ViewerPlaceholder(QWidget *parent = nullptr);

  protected:
    void paintEvent(QPaintEvent *event) override;
};

} // namespace bending::viewer
