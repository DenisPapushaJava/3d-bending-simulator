#pragma once
#include "bending/application/BendingDrawing.h"
#include <QDialog>
namespace bending::ui {
class ImportController;
class DrawingDialog final : public QDialog {
  public:
    DrawingDialog(ImportController &controller,
                  std::shared_ptr<const application::BendingDrawingBuilder> builder,
                  std::optional<std::size_t> selectedFace, QWidget *parent);
    ~DrawingDialog() override;

  protected:
    void showEvent(QShowEvent *event) override;
    void done(int result) override;

  private:
    bool initialPositionSet_ = false;
    class Impl;
    std::unique_ptr<Impl> impl_;
};
} // namespace bending::ui
