#pragma once

#include "bending/application/FemExperiment.h"
#include <QDialog>
#include <memory>

namespace bending::ui {
class FemDialog final : public QDialog {
    Q_OBJECT
  public:
    explicit FemDialog(std::shared_ptr<application::FemRunner> runner, QWidget *parent = nullptr);
    ~FemDialog() override;
    void setCadContext(QString description);
    [[nodiscard]] bool isRunning() const;

  protected:
    void closeEvent(QCloseEvent *event) override;
    void reject() override;

  private:
    class Impl;
    std::unique_ptr<Impl> impl_;
};
} // namespace bending::ui
