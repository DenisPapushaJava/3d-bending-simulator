#pragma once

#include <QDialog>
#include <memory>

namespace bending::application {
class FlatPatternBuilder;
}

namespace bending::ui {
class ImportController;

class FlatDxfDialog final : public QDialog {
  public:
    FlatDxfDialog(ImportController &controller,
                  std::shared_ptr<const application::FlatPatternBuilder> builder,
                  const QString &modelPath, QWidget *parent);

  protected:
    void keyPressEvent(QKeyEvent *event) override;

  private:
    bool rebuildPending_ = false;
};
} // namespace bending::ui
