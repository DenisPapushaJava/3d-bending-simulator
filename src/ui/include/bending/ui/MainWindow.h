#pragma once

#include <QMainWindow>

#include <string_view>

namespace bending::ui {

// Views are borrowed for construction only; the window copies them into Qt strings.
struct RuntimeInfo {
    std::string_view applicationVersion;
    std::string_view cadVersion;
};

class MainWindow final : public QMainWindow {
  public:
    explicit MainWindow(RuntimeInfo runtime);
};
} // namespace bending::ui
