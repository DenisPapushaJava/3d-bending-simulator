#pragma once

#include <QDialog>
#include <memory>

class QLabel;

namespace bending::application {
class FlatPatternBuilder;
}

namespace bending::ui {
class ImportController;
class FlatDxfPreview;

class FlatDxfDialog final : public QDialog {
  public:
    FlatDxfDialog(ImportController &controller,
                  std::shared_ptr<const application::FlatPatternBuilder> builder,
                  const QString &modelPath, QWidget *parent);
    void setDimensionDecimals(int decimals);

  protected:
    void keyPressEvent(QKeyEvent *event) override;

  private:
    void refreshSize();
    FlatDxfPreview *preview_ = nullptr;
    QLabel *size_ = nullptr;
    int dimensionDecimals_;
    bool rebuildPending_ = false;
};
} // namespace bending::ui
