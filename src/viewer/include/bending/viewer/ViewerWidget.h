#pragma once

#include <QImage>
#include <QWidget>

#include <memory>

namespace bending::viewer {

// OCCT and native-window resources stay behind this Qt-only presentation boundary.
class ViewerWidget final : public QWidget {
    Q_OBJECT

  public:
    enum class Orientation { Isometric, Top, Front };

    explicit ViewerWidget(QWidget *parent = nullptr);
    ~ViewerWidget() override;

    [[nodiscard]] bool isReady() const;
    [[nodiscard]] QString errorString() const;
    // Captures the OCCT framebuffer, including native surfaces QWidget::grab cannot capture.
    [[nodiscard]] QImage captureImage();
    void fitAll();
    void setOrientation(Orientation orientation);

  signals:
    void renderingChanged(bool ready);
    void selectionChanged(const QString &description);

  private:
    class Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace bending::viewer
