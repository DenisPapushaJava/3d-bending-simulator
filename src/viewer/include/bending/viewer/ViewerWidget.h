#pragma once

#include <QImage>
#include <QWidget>

#include <cstddef>
#include <memory>
#include <vector>

namespace bending::application {
class Document;
struct RecognizedBend;
class SingleBendPreview;
} // namespace bending::application
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
    // Empty result means accepted. Geometry is prepared by the importer before publication.
    [[nodiscard]] QString presentDocument(std::shared_ptr<const application::Document> document,
                                          quint64 revision);
    void clearDocument(quint64 revision);
    void highlightFaces(const std::vector<std::size_t> &indices, quint64 revision);
    // Distinguishes the bend zone from its adjacent planar flanges.
    void highlightBend(const application::RecognizedBend &bend, quint64 revision);
    [[nodiscard]] QString
    presentBendPreview(std::shared_ptr<const application::SingleBendPreview> preview,
                       std::size_t frameIndex, quint64 documentRevision);
    [[nodiscard]] QString endBendPreview();
    // Explicit diagnostic scene, never shown by the normal application startup.
    void showDiagnosticBox();

  signals:
    void renderingChanged(bool ready);
    void selectionChanged(quint64 documentRevision, int faceIndex);

  private:
    class Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace bending::viewer
