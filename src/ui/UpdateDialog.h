#pragma once
#include <QDialog>
#include <functional>
namespace bending::update {
class UpdateClient;
}
namespace bending::ui {
class UpdateDialog final : public QDialog {
  public:
    UpdateDialog(update::UpdateClient &client, std::function<void()> apply, QWidget *parent);
    void exitStatus(const QString &message, bool waiting);
    ~UpdateDialog() override;

  protected:
    void done(int result) override;

  private:
    class Impl;
    std::unique_ptr<Impl> impl_;
};
} // namespace bending::ui
