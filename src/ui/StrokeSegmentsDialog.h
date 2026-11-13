#pragma once

#include "bending/application/DocumentSession.h"

#include <QDialog>
#include <functional>

namespace bending::ui {
class StrokeSegmentsDialog final : public QDialog {
  public:
    explicit StrokeSegmentsDialog(QWidget *parent) : QDialog(parent) {}
    bool edit(const application::DocumentSession &session, std::size_t bend,
              const std::function<void(const application::BendStroke &)> &highlight);
    [[nodiscard]] const std::vector<application::StrokeEdit> &edits() const {
        return edits_;
    }
    [[nodiscard]] std::size_t selectedBend() const {
        return selectedBend_;
    }

  private:
    std::vector<application::StrokeEdit> edits_;
    std::size_t selectedBend_ = 0;
};
} // namespace bending::ui
