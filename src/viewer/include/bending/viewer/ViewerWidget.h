#pragma once

#include "bending/viewer/ToolSegmentColors.h"
#include <QImage>
#include <QWidget>

#include <cstddef>
#include <memory>
#include <optional>
#include <vector>

class QMenu;
class QAction;
namespace bending::application {
class Document;
struct RecognizedBend;
struct BendInteriorContourDiagnostic;
class SingleBendPreview;
class FlatPattern;
} // namespace bending::application
namespace bending::viewer {

// OCCT and native-window resources stay behind this Qt-only presentation boundary.
class ViewerWidget final : public QWidget {
    Q_OBJECT

  public:
    enum class Orientation { Isometric, Top, Front, Back, Left, Right, Bottom };

    explicit ViewerWidget(QWidget *parent = nullptr);
    ~ViewerWidget() override;

    [[nodiscard]] bool isReady() const;
    [[nodiscard]] QString errorString() const;
    // Shared actions, owned by the viewer; null when the native backend is unavailable.
    [[nodiscard]] QMenu *navigationMenu() const;
    [[nodiscard]] QAction *sheetOrientationAction() const;
    // Captures the OCCT framebuffer, including native surfaces QWidget::grab cannot capture.
    [[nodiscard]] QImage captureImage(bool includeControls = false);
    // Qt-only HUD attachment. The viewer owns positioning above its native canvas.
    void setBottomOverlay(QWidget *);
    void setTopLeftOverlay(QWidget *);
    void setToolSegmentColors(ToolSegmentColors);
    // Redraws the current presentations/HUD without changing the camera or CAD inputs.
    void refreshScene();
    void fitAll();
    void setCutoutsVisible(bool visible);
    void setDimensionsVisible(bool visible);
    // Presentation hint only: animation keeps the active angle and hides detailed dimensions.
    void setAnimationRunning(bool running);
    [[nodiscard]] bool dimensionsVisible() const;
    void setOrientation(Orientation orientation);
    // Empty result means accepted. Geometry is prepared by the importer before publication.
    [[nodiscard]] QString presentDocument(std::shared_ptr<const application::Document> document,
                                          quint64 revision);
    void clearDocument(quint64 revision);
    void highlightFaces(const std::vector<std::size_t> &indices, quint64 revision);
    // Distinguishes the bend zone from its adjacent planar flanges.
    void highlightBend(const application::RecognizedBend &bend, quint64 revision);
    void highlightPreviewBend(std::optional<std::size_t>);
    void highlightBendDiagnostic(const application::BendInteriorContourDiagnostic &diagnostic,
                                 quint64 revision);
    [[nodiscard]] QString
    presentBendPreview(std::shared_ptr<const application::SingleBendPreview> preview,
                       std::size_t frameIndex, quint64 documentRevision);
    [[nodiscard]] QString endBendPreview();
    [[nodiscard]] QString presentFlatPattern(std::shared_ptr<const application::FlatPattern>,
                                             quint64 documentRevision);
    // Explicit diagnostic scene, never shown by the normal application startup.
    void showDiagnosticBox();

  signals:
    void renderingChanged(bool ready);
    void selectionChanged(quint64 documentRevision, int faceIndex);
    // Source bend index on the flat pattern, -1 clears selection; never a route step.
    void bendSelectionChanged(quint64 documentRevision, int bendIndex);

  private:
    class Impl;
    std::unique_ptr<Impl> impl_;
    void queueOverlayLayout();
    void layoutOverlays();

  protected:
    bool eventFilter(QObject *, QEvent *) override;
};

} // namespace bending::viewer
