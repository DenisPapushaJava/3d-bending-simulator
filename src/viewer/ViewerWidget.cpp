#include "bending/viewer/ViewerWidget.h"
#include "Dimensions.h"
#include "bending/cad/occt/DocumentAccess.h"
#include "bending/cad/occt/PreviewAccess.h"

#include <AIS_ColoredShape.hxx>
#include <AIS_InteractiveContext.hxx>
#include <AIS_Shape.hxx>
#include <AIS_ViewCube.hxx>
#include <Aspect_DisplayConnection.hxx>
#include <Aspect_NeutralWindow.hxx>
#include <BRepPrimAPI_MakeBox.hxx>
#include <BRep_Builder.hxx>
#include <Bnd_Box.hxx>
#include <Graphic3d_TransformPers.hxx>
#include <Image_PixMap.hxx>
#include <OpenGl_GraphicDriver.hxx>
#include <Prs3d_Drawer.hxx>
#include <Prs3d_LineAspect.hxx>
#include <Prs3d_TextAspect.hxx>
#include <Standard_Failure.hxx>
#include <TopExp.hxx>
#include <TopTools_IndexedMapOfShape.hxx>
#include <TopoDS_Compound.hxx>
#include <V3d_AmbientLight.hxx>
#include <V3d_DirectionalLight.hxx>
#include <V3d_View.hxx>
#include <V3d_Viewer.hxx>

#include <QAction>
#include <QActionGroup>
#include <QGuiApplication>
#include <QHBoxLayout>
#include <QKeyEvent>
#include <QLabel>
#include <QMenu>
#include <QMouseEvent>
#include <QPainter>
#include <QResizeEvent>
#include <QStackedLayout>
#include <QTimer>
#include <QToolButton>
#include <QWheelEvent>

#include <algorithm>
#include <array>
#include <cmath>
#include <functional>
#include <stdexcept>
#include <utility>

namespace bending::viewer {
namespace {

QString failureText(const Standard_Failure &error) {
    const auto *message = error.GetMessageString();
    return message && *message ? QString::fromUtf8(message)
                               : QStringLiteral("Ошибка отображения OCCT");
}

struct FaceHighlights {
    std::vector<std::size_t> primary;
    std::vector<std::size_t> flanges;
    bool bend = false;
    std::vector<std::size_t> cutouts{};
    std::optional<application::RecognizedBend> measuredBend{};
};

class OcctCanvas final : public QWidget {
    enum class DisplayStyle { Edges, Shaded, Wireframe };

  public:
    explicit OcctCanvas(QWidget *parent) : QWidget(parent) {
        setObjectName(QStringLiteral("cadCanvas"));
        setAttribute(Qt::WA_NativeWindow);
        setAttribute(Qt::WA_PaintOnScreen);
        setAttribute(Qt::WA_NoSystemBackground);
        setMouseTracking(true);
        setFocusPolicy(Qt::StrongFocus);
        controls_ = new QWidget(this);
        controls_->setObjectName(QStringLiteral("viewportControls"));
        controls_->setAttribute(Qt::WA_NativeWindow);
        controls_->setStyleSheet(QStringLiteral(
            "QWidget#viewportControls {background:#202A36;}"
            "QToolButton {background:#F7F9FB;color:#1C2938;border:1px solid #DCE3E9;"
            "border-radius:5px;padding:5px;}"
            "QToolButton:hover {background:#E5F3F3;}"
            "QToolButton:checked {background:#087F83;color:white;border-color:#087F83;}"
            "QToolButton:focus {border:2px solid #4DADFF;}"
            "QToolButton:disabled {background:#34404E;color:#9AABB9;border-color:#526171;}"));
        auto *row = new QHBoxLayout(controls_);
        row->setContentsMargins(0, 0, 0, 0);
        row->setSpacing(4);
        navigationMenu_ = new QMenu(QStringLiteral("Просмотр сцены"), this);
        navigationMenu_->setObjectName(QStringLiteral("viewportViewMenu"));
        const auto orientation = [&](const QString &label, ViewerWidget::Orientation value,
                                     int shortcut, const char *name) {
            auto *action = navigationMenu_->addAction(label);
            action->setObjectName(QString::fromLatin1(name));
            action->setShortcut(QKeySequence(QStringLiteral("Ctrl+%1").arg(shortcut)));
            action->setShortcutContext(Qt::WidgetWithChildrenShortcut);
            addAction(action);
            connect(action, &QAction::triggered, this, [this, value] { setOrientation(value); });
            return action;
        };
        auto *home = orientation(QStringLiteral("Изометрия"), ViewerWidget::Orientation::Isometric,
                                 7, "viewIsometric");
        orientation(QStringLiteral("Спереди"), ViewerWidget::Orientation::Front, 1, "viewFront");
        orientation(QStringLiteral("Сзади"), ViewerWidget::Orientation::Back, 2, "viewBack");
        orientation(QStringLiteral("Слева"), ViewerWidget::Orientation::Left, 3, "viewLeft");
        orientation(QStringLiteral("Справа"), ViewerWidget::Orientation::Right, 4, "viewRight");
        orientation(QStringLiteral("Сверху"), ViewerWidget::Orientation::Top, 5, "viewTop");
        orientation(QStringLiteral("Снизу"), ViewerWidget::Orientation::Bottom, 6, "viewBottom");
        navigationMenu_->addSeparator();
        auto *display = navigationMenu_->addMenu(QStringLiteral("Отображение"));
        auto *styles = new QActionGroup(this);
        const auto addDisplayStyle = [&](const QString &label, DisplayStyle value,
                                         const char *name) {
            auto *action = display->addAction(label);
            action->setObjectName(QString::fromLatin1(name));
            action->setCheckable(true);
            action->setChecked(value == displayStyle_);
            styles->addAction(action);
            connect(action, &QAction::triggered, this, [this, value] { setDisplayStyle(value); });
        };
        addDisplayStyle(QStringLiteral("С рёбрами"), DisplayStyle::Edges, "displayEdges");
        addDisplayStyle(QStringLiteral("Без рёбер"), DisplayStyle::Shaded, "displayShaded");
        addDisplayStyle(QStringLiteral("Каркас"), DisplayStyle::Wireframe, "displayWireframe");
        navigationMenu_->addSeparator();
        auto *fit = navigationMenu_->addAction(QStringLiteral("Вписать модель"));
        fit->setObjectName(QStringLiteral("fitModel"));
        fit->setShortcut(QKeySequence(Qt::Key_F));
        fit->setShortcutContext(Qt::WidgetWithChildrenShortcut);
        addAction(fit);
        connect(fit, &QAction::triggered, this, [this] { fitAll(); });
        const auto icon = [](bool isHome) {
            QIcon result;
            for (int ratio : {1, 2}) {
                QPixmap pixels(24 * ratio, 24 * ratio);
                pixels.setDevicePixelRatio(ratio);
                pixels.fill(Qt::transparent);
                QPainter painter(&pixels);
                painter.setRenderHint(QPainter::Antialiasing);
                painter.setPen(QPen(QColor("#1C2938"), 1.6));
                if (isHome) {
                    painter.drawPolyline(QPolygonF{{3, 11}, {12, 3}, {21, 11}});
                    painter.drawPolyline(QPolygonF{{6, 10},
                                                   {6, 21},
                                                   {10, 21},
                                                   {10, 15},
                                                   {14, 15},
                                                   {14, 21},
                                                   {18, 21},
                                                   {18, 10}});
                } else {
                    for (int x : {0, 1})
                        for (int y : {0, 1}) {
                            const QPointF corner(x ? 21 : 3, y ? 21 : 3);
                            painter.drawLine(corner, corner + QPointF(x ? -5 : 5, 0));
                            painter.drawLine(corner, corner + QPointF(0, y ? -5 : 5));
                        }
                    painter.drawRect(QRectF(8, 8, 8, 8));
                }
                painter.end();
                result.addPixmap(pixels);
            }
            return result;
        };
        home->setIcon(icon(true));
        fit->setIcon(icon(false));
        const auto button = [&](QAction *action, const char *name) {
            auto *result = new QToolButton(controls_);
            result->setObjectName(QString::fromLatin1(name));
            result->setDefaultAction(action);
            result->setAccessibleName(action->text());
            result->setToolTip(action->text() +
                               QStringLiteral(" (%1)").arg(action->shortcut().toString()));
            result->setMinimumSize(34, 34);
            row->addWidget(result);
            return result;
        };
        button(home, "homeView");
        button(fit, "fitView");
        auto *menuButton = new QToolButton(controls_);
        menuButton->setObjectName(QStringLiteral("viewOptions"));
        menuButton->setText(QStringLiteral("Вид"));
        menuButton->setAccessibleName(QStringLiteral("Ракурсы и отображение модели"));
        menuButton->setMenu(navigationMenu_);
        menuButton->setPopupMode(QToolButton::InstantPopup);
        menuButton->setMinimumSize(56, 34);
        row->addWidget(menuButton);
        dimensionsAction_ = navigationMenu_->addAction(QStringLiteral("Размеры"));
        dimensionsAction_->setCheckable(true);
        dimensionsAction_->setShortcut(QKeySequence(QStringLiteral("Ctrl+D")));
        dimensionsAction_->setShortcutContext(Qt::WindowShortcut);
        addAction(dimensionsAction_);
        dimensionsButton_ = button(dimensionsAction_, "showDimensions");
        dimensionsButton_->setToolButtonStyle(Qt::ToolButtonTextOnly);
        dimensionsButton_->setMinimumWidth(104);
        dimensionsButton_->setAccessibleName(QStringLiteral("Показать или скрыть размеры модели"));
        connect(dimensionsAction_, &QAction::toggled, this,
                [this](bool visible) { setDimensionsVisible(visible); });
        updateDimensionButton();
    }

    ~OcctCanvas() override {
        release();
    }

    std::function<void(const QString &)> onError;
    std::function<void()> onReady;
    std::function<void(quint64, int)> onSelection;

    QMenu *navigationMenu() const {
        return navigationMenu_;
    }

    bool ready() const {
        return ready_;
    }

    QString present(const TopoDS_Shape &shape, application::DocumentPtr document, quint64 revision,
                    bool diagnostic = false) {
        TopTools_IndexedMapOfShape faces;
        TopExp::MapShapes(shape, TopAbs_FACE, faces);
        Handle(AIS_ColoredShape) candidate;
        if (ready_) {
            try {
                candidate = new AIS_ColoredShape(shape);
                candidate->Attributes()->SetFaceBoundaryDraw(displayStyle_ == DisplayStyle::Edges);
                candidate->Attributes()->SetAutoTriangulation(diagnostic);
                context_->SetColor(candidate, Quantity_Color(0.16, 0.62, 0.66, Quantity_TOC_RGB),
                                   false);
                context_->Display(candidate,
                                  displayStyle_ == DisplayStyle::Wireframe ? AIS_WireFrame
                                                                           : AIS_Shaded,
                                  AIS_Shape::SelectionMode(TopAbs_FACE), false);
                if (!presentation_.IsNull()) {
                    context_->Remove(presentation_, false);
                }
                removeDimensions();
                view_->SetProj(V3d_XposYnegZpos);
                view_->FitAll(dimensionsVisible_ ? 0.3 : 0.1, false);
                view_->ZFitAll();
                context_->ClearSelected(false);
                view_->Redraw();
            } catch (const Standard_Failure &error) {
                restore(candidate);
                return failureText(error);
            } catch (const std::exception &error) {
                restore(candidate);
                return QString::fromUtf8(error.what());
            }
        }
        const auto retainedHighlights = document && document == document_ && revision == revision_
                                            ? highlights_
                                            : FaceHighlights{};
        presentation_ = candidate;
        preview_.reset();
        shape_ = shape;
        faces_ = std::move(faces);
        document_ = std::move(document);
        revision_ = revision;
        diagnostic_ = diagnostic;
        setHighlights(retainedHighlights, revision_);
        if (onSelection) {
            onSelection(revision_, 0);
        }
        return {};
    }

    QString presentPreview(application::BendPreviewPtr preview, std::size_t index,
                           quint64 revision) {
        if (revision != revision_ || !document_ || !preview || index >= preview->frameCount()) {
            return QStringLiteral("Неактуальный результат просмотра сгиба");
        }
        const auto &frame = cad::OcctPreviewAccess::frame(*preview, index);
        const bool entering = preview_ != preview;
        Handle(AIS_ColoredShape) candidate;
        if (ready_) {
            try {
                candidate = new AIS_ColoredShape(frame.displayShape.IsNull() ? frame.shape
                                                                             : frame.displayShape);
                candidate->Attributes()->SetAutoTriangulation(false);
                candidate->Attributes()->SetFaceBoundaryDraw(displayStyle_ == DisplayStyle::Edges);
                const std::array<Quantity_Color, 3> colors{
                    Quantity_Color(0.15, 0.48, 0.65, Quantity_TOC_RGB),
                    Quantity_Color(1.0, 0.25, 0.04, Quantity_TOC_RGB),
                    Quantity_Color(0.16, 0.70, 0.51, Quantity_TOC_RGB)};
                const auto &regions =
                    frame.displayShape.IsNull() ? frame.regions : frame.displayRegions;
                for (std::size_t region = 0; region < regions.size(); ++region) {
                    TopTools_IndexedMapOfShape regionFaces;
                    TopExp::MapShapes(regions[region], TopAbs_FACE, regionFaces);
                    for (int face = 1; face <= regionFaces.Extent(); ++face) {
                        candidate->SetCustomColor(regionFaces(face), colors[region]);
                    }
                }
                const std::array<Quantity_Color, 2> toolColors{
                    Quantity_Color(0.30, 0.36, 0.44, Quantity_TOC_RGB),
                    Quantity_Color(0.64, 0.70, 0.77, Quantity_TOC_RGB)};
                for (std::size_t tool = 0; tool < frame.tools.size(); ++tool) {
                    if (!frame.tools[tool].IsNull()) {
                        candidate->SetCustomColor(frame.tools[tool], toolColors[tool]);
                    }
                }
                context_->Display(candidate,
                                  displayStyle_ == DisplayStyle::Wireframe ? AIS_WireFrame
                                                                           : AIS_Shaded,
                                  -1, false);
                if (!presentation_.IsNull()) {
                    context_->Remove(presentation_, false);
                }
                for (const auto &overlay : {evidence_, support_}) {
                    if (!overlay.IsNull()) {
                        context_->Remove(overlay, false);
                    }
                }
                context_->ClearSelected(false);
                if (entering) {
                    if (preview->toolSetup()) {
                        view_->SetProj(1.0, 0.5, 3.0);
                        view_->SetUp(0.0, 1.0, 0.0);
                    } else {
                        view_->SetProj(V3d_XposYnegZpos);
                    }
                    fitPreview(*preview);
                }
                view_->ZFitAll();
                if (!dimensionsVisible_) {
                    view_->Redraw();
                }
            } catch (const Standard_Failure &error) {
                restore(candidate);
                return failureText(error);
            } catch (const std::exception &error) {
                restore(candidate);
                return QString::fromUtf8(error.what());
            }
        }
        presentation_ = candidate;
        evidence_.Nullify();
        support_.Nullify();
        preview_ = std::move(preview);
        previewFrame_ = index;
        refreshDimensions();
        if (onSelection) {
            onSelection(revision_, 0);
        }
        return {};
    }
    QString endPreview() {
        QString error;
        try {
            error = preview_ ? present(shape_, document_, revision_) : QString{};
        } catch (const Standard_Failure &failure) {
            error = failureText(failure);
        } catch (const std::exception &failure) {
            error = QString::fromUtf8(failure.what());
        }
        if (!error.isEmpty()) {
            fail(error);
        }
        return error;
    }

    void clearDocument(quint64 revision) {
        guarded([&] {
            context_->RemoveAll(false);
            context_->ClearSelected(false);
            view_->Redraw();
        });
        presentation_.Nullify();
        evidence_.Nullify();
        support_.Nullify();
        dimensions_.objects.clear();
        dimensions_.labels.clear();
        highlights_ = {};
        shape_.Nullify();
        faces_.Clear();
        document_.reset();
        preview_.reset();
        revision_ = revision;
        diagnostic_ = false;
        updateDimensionButton();
        if (onSelection) {
            onSelection(revision_, 0);
        }
    }

    void setCutoutsVisible(bool visible) {
        cutoutsVisible_ = visible;
        setHighlights(highlights_, revision_);
    }
    bool dimensionsVisible() const {
        return dimensionsVisible_;
    }
    void setDimensionsVisible(bool visible) {
        if (dimensionsVisible_ == visible) {
            return;
        }
        dimensionsVisible_ = visible;
        dimensionsAction_->setChecked(visible);
        refreshDimensions();
        if (visible) {
            fitAll();
        }
    }
    void highlightFaces(const std::vector<std::size_t> &indices, quint64 revision) {
        setHighlights({indices, {}, false}, revision);
    }

    void highlightBend(const application::RecognizedBend &bend, quint64 revision) {
        setHighlights({{bend.innerFace, bend.outerFace},
                       std::vector<std::size_t>(bend.flangeFaces.begin(), bend.flangeFaces.end()),
                       true,
                       {},
                       bend},
                      revision);
    }

    void highlightBendDiagnostic(const application::BendInteriorContourDiagnostic &diagnostic,
                                 quint64 revision) {
        const auto &bend = diagnostic.candidate;
        setHighlights({{bend.innerFace, bend.outerFace},
                       std::vector<std::size_t>(bend.flangeFaces.begin(), bend.flangeFaces.end()),
                       true,
                       diagnostic.boundaryFaces,
                       bend},
                      revision);
    }

    void setHighlights(FaceHighlights state, quint64 revision) {
        if (revision != revision_) {
            return;
        }
        const auto sanitize = [&](std::vector<std::size_t> &indices) {
            std::erase_if(indices, [&](auto i) {
                return i == 0 || i > static_cast<std::size_t>(faces_.Extent());
            });
            std::sort(indices.begin(), indices.end());
            indices.erase(std::unique(indices.begin(), indices.end()), indices.end());
        };
        sanitize(state.primary);
        sanitize(state.flanges);
        sanitize(state.cutouts);
        state.bend = state.bend && !state.primary.empty();
        highlights_ = std::move(state);
        if (preview_) {
            return;
        }
        guarded([&] {
            for (const auto &overlay : {evidence_, support_}) {
                if (!overlay.IsNull()) {
                    context_->Remove(overlay, false);
                }
            }
            evidence_.Nullify();
            support_.Nullify();
            if (!presentation_.IsNull()) {
                presentation_->ClearCustomAspects();
                context_->SetColor(presentation_,
                                   highlights_.bend
                                       ? Quantity_Color(0.27, 0.35, 0.40, Quantity_TOC_RGB)
                                       : Quantity_Color(0.16, 0.62, 0.66, Quantity_TOC_RGB),
                                   false);
                if (highlights_.bend) {
                    for (const auto i : highlights_.primary) {
                        const auto &face = faces_(static_cast<int>(i));
                        presentation_->SetCustomColor(
                            face, Quantity_Color(1.0, 0.20, 0.025, Quantity_TOC_RGB));
                        const auto drawer = presentation_->CustomAspects(face);
                        drawer->SetShadingModel(Graphic3d_TypeOfShadingModel_Unlit);
                        drawer->SetFaceBoundaryDraw(true);
                        drawer->SetFaceBoundaryAspect(
                            new Prs3d_LineAspect(Quantity_Color(1.0, 0.85, 0.40, Quantity_TOC_RGB),
                                                 Aspect_TOL_SOLID, 3.0));
                    }
                }
                auto cutouts = highlights_.cutouts;
                if (document_ && cutoutsVisible_) {
                    const auto &known = document_->metadata().closedCutouts.boundaryFaces;
                    cutouts.insert(cutouts.end(), known.begin(), known.end());
                }
                if (cutoutsVisible_) {
                    for (const auto i : cutouts) {
                        if (i == 0 || i > static_cast<std::size_t>(faces_.Extent())) {
                            continue;
                        }
                        const auto &face = faces_(static_cast<int>(i));
                        presentation_->SetCustomColor(
                            face, Quantity_Color(1.0, 0.025, 0.20, Quantity_TOC_RGB));
                        const auto drawer = presentation_->CustomAspects(face);
                        drawer->SetShadingModel(Graphic3d_TypeOfShadingModel_Unlit);
                        drawer->SetFaceBoundaryDraw(true);
                        drawer->SetFaceBoundaryAspect(
                            new Prs3d_LineAspect(Quantity_Color(1.0, 0.55, 0.65, Quantity_TOC_RGB),
                                                 Aspect_TOL_SOLID, 2.0));
                    }
                }
                context_->Redisplay(presentation_, false);
            }
            if (highlights_.bend) {
                support_ = outline(highlights_.flanges,
                                   Quantity_Color(0.30, 0.50, 0.60, Quantity_TOC_RGB), 1.0);
            }
            evidence_ = outline(highlights_.primary,
                                highlights_.bend ? Quantity_Color(1.0, 0.85, 0.40, Quantity_TOC_RGB)
                                                 : Quantity_Color(1.0, 0.65, 0.1, Quantity_TOC_RGB),
                                3.0);
            view_->Redraw();
        });
        refreshDimensions();
    }

    void fitAll() {
        if (shape_.IsNull()) {
            return;
        }
        guarded([&] {
            if (preview_) {
                fitPreview(*preview_);
            } else {
                view_->FitAll(dimensionsVisible_ ? 0.3 : 0.1, false);
            }
            view_->ZFitAll();
            refreshDimensions();
            view_->Redraw();
        });
    }

    void setOrientation(ViewerWidget::Orientation orientation) {
        guarded([&] {
            const bool yup = preview_ && preview_->toolSetup();
            V3d_TypeOfOrientation projection = V3d_XposYnegZpos;
            switch (orientation) {
            case ViewerWidget::Orientation::Isometric:
                if (yup) {
                    view_->SetProj(1.0, 0.5, 3.0);
                    view_->SetUp(0.0, 1.0, 0.0);
                    fitAll();
                    return;
                }
                break;
            case ViewerWidget::Orientation::Front:
                projection = yup ? V3d_Zpos : V3d_Yneg;
                break;
            case ViewerWidget::Orientation::Back:
                projection = yup ? V3d_Zneg : V3d_Ypos;
                break;
            case ViewerWidget::Orientation::Top:
                projection = yup ? V3d_Ypos : V3d_Zpos;
                break;
            case ViewerWidget::Orientation::Bottom:
                projection = yup ? V3d_Yneg : V3d_Zneg;
                break;
            case ViewerWidget::Orientation::Left:
                projection = V3d_Xneg;
                break;
            case ViewerWidget::Orientation::Right:
                projection = V3d_Xpos;
                break;
            }
            view_->SetProj(projection, yup);
            fitAll();
        });
    }

    void setDisplayStyle(DisplayStyle value) {
        displayStyle_ = value;
        guarded([&] {
            if (!presentation_.IsNull()) {
                presentation_->Attributes()->SetFaceBoundaryDraw(value == DisplayStyle::Edges);
                context_->SetDisplayMode(
                    presentation_, value == DisplayStyle::Wireframe ? AIS_WireFrame : AIS_Shaded,
                    false);
                context_->Redisplay(presentation_, false);
                view_->Redraw();
            }
        });
    }

    QImage captureImage(bool includeControls) {
        QImage result;
        guarded([&] {
            Image_PixMap pixels;
            const auto size = physicalSize();
            if (!view_->ToPixMap(pixels, size.width(), size.height(), Graphic3d_BT_RGB)) {
                throw std::runtime_error("OCCT framebuffer read failed");
            }
            result = QImage(size, QImage::Format_RGB32);
            for (int y = 0; y < size.height(); ++y) {
                auto *row = reinterpret_cast<QRgb *>(result.scanLine(y));
                for (int x = 0; x < size.width(); ++x) {
                    const auto color = pixels.PixelColor(x, y).GetRGB();
                    row[x] = qRgb(qRound(color.Red() * 255), qRound(color.Green() * 255),
                                  qRound(color.Blue() * 255));
                }
            }
        });
        if (!result.isNull() && includeControls) {
            result.setDevicePixelRatio(devicePixelRatioF());
            QPainter painter(&result);
            painter.drawPixmap(controls_->pos(), controls_->grab());
        }
        return result;
    }

  protected:
    bool event(QEvent *event) override {
        const bool handled = QWidget::event(event);
        if (event->type() == QEvent::WinIdChange && ready_) {
            release();
            attempted_ = false;
            update();
        }
        return handled;
    }

    QPaintEngine *paintEngine() const override {
        return nullptr;
    }

    void paintEvent(QPaintEvent *) override {
        if (!attempted_) {
            initialize();
        }
        guarded([&] { view_->Redraw(); });
    }

    void resizeEvent(QResizeEvent *) override {
        controls_->adjustSize();
        controls_->move(qMax(12, width() - controls_->width() - 12), 12);
        updateNavigationCube();
        refreshDimensions();
        guarded([&] { view_->Redraw(); });
    }

    void showEvent(QShowEvent *) override {
        if (!window_.IsNull()) {
            window_->Map();
        }
        update();
    }

    void mousePressEvent(QMouseEvent *event) override {
        setFocus();
        lastPosition_ = physicalPoint(event->position());
        guarded([&] {
            if (event->button() == Qt::RightButton) {
                view_->StartRotation(lastPosition_.x(), lastPosition_.y());
            } else if (event->button() == Qt::LeftButton) {
                context_->MoveTo(lastPosition_.x(), lastPosition_.y(), view_, false);
                const auto cubeOwner =
                    Handle(AIS_ViewCubeOwner)::DownCast(context_->DetectedOwner());
                if (!cubeOwner.IsNull()) {
                    view_->SetProj(cubeOwner->MainOrientation(), preview_ && preview_->toolSetup());
                    fitAll();
                    return;
                }
                if (!preview_) {
                    context_->SelectDetected(AIS_SelectionScheme_Replace);
                    context_->InitSelected();
                    int face = 0;
                    if (context_->MoreSelected() && context_->HasSelectedShape())
                        face = faces_.FindIndex(context_->SelectedShape());
                    if (onSelection)
                        onSelection(revision_, face);
                    view_->Redraw();
                }
            }
        });
        event->accept();
    }

    void mouseMoveEvent(QMouseEvent *event) override {
        const auto position = physicalPoint(event->position());
        guarded([&] {
            if (event->buttons().testFlag(Qt::RightButton)) {
                view_->Rotation(position.x(), position.y());
            } else if (event->buttons().testFlag(Qt::MiddleButton)) {
                view_->Pan(position.x() - lastPosition_.x(), lastPosition_.y() - position.y());
            } else if (event->buttons() == Qt::NoButton) {
                context_->MoveTo(position.x(), position.y(), view_, false);
            }
            if (event->buttons().testFlag(Qt::RightButton) ||
                event->buttons().testFlag(Qt::MiddleButton)) {
                refreshDimensions();
            }
            view_->Redraw();
        });
        lastPosition_ = position;
        event->accept();
    }

    void mouseReleaseEvent(QMouseEvent *event) override {
        event->accept();
    }

    void wheelEvent(QWheelEvent *event) override {
        guarded([&] {
            const double steps =
                qBound(-8.0, static_cast<double>(event->angleDelta().y()) / 120.0, 8.0);
            view_->SetZoom(std::exp(steps * 0.15));
            refreshDimensions();
            view_->Redraw();
        });
        event->accept();
    }

    void keyPressEvent(QKeyEvent *event) override {
        if (event->key() == Qt::Key_F) {
            fitAll();
            event->accept();
        } else {
            QWidget::keyPressEvent(event);
        }
    }

  private:
    Handle(AIS_Shape) outline(const std::vector<std::size_t> &indices, const Quantity_Color &color,
                              double width) {
        if (indices.empty()) {
            return {};
        }
        TopoDS_Compound compound;
        BRep_Builder builder;
        builder.MakeCompound(compound);
        for (const auto index : indices) {
            builder.Add(compound, faces_(static_cast<int>(index)));
        }
        Handle(AIS_Shape) result = new AIS_Shape(compound);
        result->Attributes()->SetAutoTriangulation(false);
        context_->SetColor(result, color, false);
        context_->SetWidth(result, width, false);
        context_->Display(result, AIS_WireFrame, -1, false);
        return result;
    }

    void removeDimensions() {
        for (const auto &object : dimensions_.objects) {
            context_->Remove(object, false);
        }
        dimensions_ = {};
    }
    void updateNavigationCube() {
        if (!ready_ || context_.IsNull())
            return;
        guarded([&] {
            if (shape_.IsNull()) {
                if (!navigationCube_.IsNull() && context_->IsDisplayed(navigationCube_))
                    context_->Remove(navigationCube_, false);
                return;
            }
            const bool yup = preview_ && preview_->toolSetup();
            const double ratio = devicePixelRatioF();
            const bool created = navigationCube_.IsNull();
            if (created) {
                navigationCube_ = new AIS_ViewCube();
                navigationCube_->SetAutoStartAnimation(false);
                navigationCube_->SetDrawAxes(false);
                navigationCube_->SetBoxColor(Quantity_Color(.86, .90, .94, Quantity_TOC_RGB));
                navigationCube_->SetTextColor(Quantity_Color(.08, .12, .18, Quantity_TOC_RGB));
                navigationCube_->Attributes()->TextAspect()->Aspect()->SetTextFontAspect(
                    Font_FA_Bold);
            }
            const bool changed = created || navigationCube_->IsYup() != yup || cubeRatio_ != ratio;
            if (changed) {
                navigationCube_->SetYup(yup, false);
                navigationCube_->SetSize(64 * ratio);
                navigationCube_->SetFontHeight(12 * ratio);
                const auto label = [&](V3d_TypeOfOrientation side, const char *value) {
                    navigationCube_->SetBoxSideLabel(side, value);
                };
                label(V3d_Xpos, "СПРАВА");
                label(V3d_Xneg, "СЛЕВА");
                label(yup ? V3d_Ypos : V3d_Zpos, "СВЕРХУ");
                label(yup ? V3d_Yneg : V3d_Zneg, "СНИЗУ");
                label(yup ? V3d_Zpos : V3d_Yneg, "СПЕРЕДИ");
                label(yup ? V3d_Zneg : V3d_Ypos, "СЗАДИ");
                navigationCube_->SetTransformPersistence(new Graphic3d_TransformPers(
                    Graphic3d_TMF_TriedronPers, Aspect_TOTP_RIGHT_UPPER,
                    Graphic3d_Vec2i(qRound(82 * ratio), qRound(116 * ratio))));
                cubeRatio_ = ratio;
                if (context_->IsDisplayed(navigationCube_))
                    context_->Redisplay(navigationCube_, false);
            }
            const bool hidden = !context_->IsDisplayed(navigationCube_);
            if (hidden)
                context_->Display(navigationCube_, 0, 0, false);
            if (changed || hidden)
                view_->Redraw();
        });
    }

    void updateDimensionButton() {
        const bool hasGeometry = ready_ && !shape_.IsNull();
        controls_->setVisible(!shape_.IsNull());
        controls_->setEnabled(hasGeometry);
        navigationMenu_->setEnabled(hasGeometry);
        for (auto *action : navigationMenu_->findChildren<QAction *>())
            action->setEnabled(hasGeometry);
        updateNavigationCube();
        dimensionsAction_->setEnabled(ready_ && document_ != nullptr);
        const auto description = dimensions_.labels.join(QStringLiteral("\n"));
        const auto scope = preview_
                               ? QStringLiteral("Размеры текущего положения. A/B — прямые участки "
                                                "полок от касания зоны, по их габаритам.")
                               : QStringLiteral("Габариты CAD по X/Y/Z. Для выбранного гиба — "
                                                "измерения исходной модели.");
        dimensionsButton_->setToolTip(
            QStringLiteral("Показать / скрыть размеры · Ctrl+D\n%1\n%2").arg(scope, description));
        dimensionsButton_->setAccessibleDescription(scope + QLatin1Char('\n') + description);
    }
    void refreshDimensions() {
        if (!dimensionsVisible_ && dimensions_.objects.empty()) {
            updateDimensionButton();
            return;
        }
        guarded([&] {
            removeDimensions();
            if (dimensionsVisible_ && document_) {
                const auto separation = view_->Convert(qRound(50.0 * devicePixelRatioF()));
                double dx, dy, dz, ux, uy, uz;
                view_->Proj(dx, dy, dz);
                view_->Up(ux, uy, uz);
                const auto up = gp_Vec(ux, uy, uz).Normalized() * separation;
                const auto right = up.Crossed(gp_Vec(dx, dy, dz)).Normalized() * separation;
                const DimensionLayout layout{right, up};
                dimensions_ =
                    preview_
                        ? previewDimensions(cad::OcctPreviewAccess::frame(*preview_, previewFrame_),
                                            preview_->model(), 14.0 * devicePixelRatioF(), layout)
                        : documentDimensions(document_->metadata(), faces_,
                                             highlights_.measuredBend, 14.0 * devicePixelRatioF(),
                                             layout);
                for (const auto &object : dimensions_.objects) {
                    context_->Display(object, 0, -1, false);
                }
            }
            view_->Redraw();
        });
        updateDimensionButton();
    }

    void restore(const Handle(AIS_Shape) & candidate) {
        try {
            if (!candidate.IsNull()) {
                context_->Remove(candidate, false);
            }
            if (!presentation_.IsNull()) {
                context_->Display(presentation_, AIS_Shaded,
                                  preview_ ? -1 : AIS_Shape::SelectionMode(TopAbs_FACE), false);
            }
            view_->Redraw();
            refreshDimensions();
        } catch (const Standard_Failure &error) {
            fail(QString::fromUtf8(error.GetMessageString()));
        }
    }
    QSize physicalSize() const {
        return {qMax(1, qRound(width() * devicePixelRatioF())),
                qMax(1, qRound(height() * devicePixelRatioF()))};
    }
    void fitPreview(const application::SingleBendPreview &preview) {
        const auto bounds = preview.bounds();
        Bnd_Box box;
        box.Add(gp_Pnt(bounds.minimum.x, bounds.minimum.y, bounds.minimum.z));
        box.Add(gp_Pnt(bounds.maximum.x, bounds.maximum.y, bounds.maximum.z));
        view_->FitAll(box, dimensionsVisible_ ? 0.3 : 0.1, false);
        view_->ZFitAll();
    }

    QPoint physicalPoint(QPointF point) const {
        return {qRound(point.x() * devicePixelRatioF()), qRound(point.y() * devicePixelRatioF())};
    }

    template <typename Operation> void guarded(Operation operation) {
        if (!ready_) {
            return;
        }
        try {
            const auto size = physicalSize();
            if (window_->SetSize(size.width(), size.height())) {
                view_->MustBeResized();
            }
            operation();
        } catch (const Standard_Failure &error) {
            fail(QString::fromUtf8(error.GetMessageString()));
        } catch (const std::exception &error) {
            fail(QString::fromUtf8(error.what()));
        }
    }

    void initialize() {
        attempted_ = true;
        try {
            // app configures runtime resources before starting any CAD worker.
            display_ = new Aspect_DisplayConnection();
            driver_ = new OpenGl_GraphicDriver(display_);
            scene_ = new V3d_Viewer(driver_);
            Handle(V3d_AmbientLight) ambient = new V3d_AmbientLight();
            ambient->SetIntensity(0.3f);
            scene_->AddLight(ambient);
            scene_->AddLight(new V3d_DirectionalLight(gp_Dir(-0.2, 0.5, -1.0)));
            scene_->SetLightOn();
            context_ = new AIS_InteractiveContext(scene_);
            view_ = scene_->CreateView();
            window_ = new Aspect_NeutralWindow();
#ifdef _WIN32
            window_->SetNativeHandle(reinterpret_cast<Aspect_Drawable>(winId()));
#else
            window_->SetNativeHandle(static_cast<Aspect_Drawable>(winId()));
#endif
            const auto size = physicalSize();
            window_->SetSize(size.width(), size.height());
            view_->SetWindow(window_);
            view_->SetBackgroundColor(Quantity_Color(0.125, 0.165, 0.21, Quantity_TOC_sRGB));
            view_->MustBeResized();

            ready_ = true;
            updateDimensionButton();
            if (preview_) {
                // Rebuild native resources from the same immutable worker snapshot.
                const auto preview = preview_;
                const auto index = previewFrame_;
                preview_.reset();
                const auto error = presentPreview(preview, index, revision_);
                if (!error.isEmpty()) {
                    throw std::runtime_error(error.toStdString());
                }
            } else if (!shape_.IsNull()) {
                const auto error = present(shape_, document_, revision_, diagnostic_);
                if (!error.isEmpty()) {
                    throw std::runtime_error(error.toStdString());
                }
            }
            if (ready_ && onReady) {
                onReady();
            }
        } catch (const Standard_Failure &error) {
            fail(QString::fromUtf8(error.GetMessageString()));
        } catch (const std::exception &error) {
            fail(QString::fromUtf8(error.what()));
        }
    }

    void fail(const QString &message) {
        ready_ = false;
        updateDimensionButton();
        if (onError) {
            onError(message.isEmpty() ? QStringLiteral("Ошибка графического контекста OCCT")
                                      : message);
        }
    }

    void release() noexcept {
        ready_ = false;
        try {
            if (!view_.IsNull()) {
                view_->Remove();
            }
        } catch (const Standard_Failure &) {
            // The OS can already have removed a native surface while shutting down.
        }
        presentation_.Nullify();
        evidence_.Nullify();
        support_.Nullify();
        dimensions_ = {};
        navigationCube_.Nullify();
        context_.Nullify();
        view_.Nullify();
        scene_.Nullify();
        driver_.Nullify();
        window_.Nullify();
        display_.Nullify();
    }

    bool attempted_ = false;
    bool ready_ = false;
    QPoint lastPosition_;
    TopoDS_Shape shape_;
    Handle(AIS_ColoredShape) presentation_;
    Handle(AIS_Shape) evidence_;
    Handle(AIS_Shape) support_;
    FaceHighlights highlights_;
    bool cutoutsVisible_ = true;
    bool dimensionsVisible_ = false;
    QToolButton *dimensionsButton_ = nullptr;
    QAction *dimensionsAction_ = nullptr;
    QMenu *navigationMenu_ = nullptr;
    QWidget *controls_ = nullptr;
    DisplayStyle displayStyle_ = DisplayStyle::Edges;
    Handle(AIS_ViewCube) navigationCube_;
    double cubeRatio_ = 0;
    DimensionAnnotations dimensions_;
    application::DocumentPtr document_;
    application::BendPreviewPtr preview_;
    std::size_t previewFrame_ = 0;
    quint64 revision_ = 0;
    bool diagnostic_ = false;
    TopTools_IndexedMapOfShape faces_;
    Handle(Aspect_DisplayConnection) display_;
    Handle(OpenGl_GraphicDriver) driver_;
    Handle(V3d_Viewer) scene_;
    Handle(AIS_InteractiveContext) context_;
    Handle(V3d_View) view_;
    Handle(Aspect_NeutralWindow) window_;
};

} // namespace

class ViewerWidget::Impl {
  public:
    OcctCanvas *canvas = nullptr; // Qt parent owns the canvas and its native resources.
    QLabel *errorPage = nullptr;
    QStackedLayout *layout = nullptr;
    QString error;
};

ViewerWidget::ViewerWidget(QWidget *parent) : QWidget(parent), impl_(std::make_unique<Impl>()) {
    setObjectName(QStringLiteral("viewer"));
    setAccessibleName(QStringLiteral("Трёхмерный CAD-просмотрщик"));
    setMinimumSize(400, 300);
    impl_->layout = new QStackedLayout(this);
    impl_->layout->setContentsMargins(0, 0, 0, 0);
    impl_->errorPage = new QLabel(this);
    impl_->errorPage->setAlignment(Qt::AlignCenter);
    impl_->errorPage->setWordWrap(true);
    impl_->errorPage->setMargin(24);
    impl_->errorPage->setStyleSheet(QStringLiteral("background:#202A36;color:#DCE3E9;"));
    impl_->layout->addWidget(impl_->errorPage);

    const auto reportError = [this](const QString &message) {
        impl_->error = message;
        impl_->errorPage->setText(QStringLiteral("3D-просмотр недоступен\n\n%1").arg(message));
        impl_->layout->setCurrentWidget(impl_->errorPage);
        emit renderingChanged(false);
    };
    const auto platform = QGuiApplication::platformName();
    if (platform != QStringLiteral("xcb") && platform != QStringLiteral("windows")) {
        reportError(QStringLiteral("Нужен Windows или Linux/X11 (QT_QPA_PLATFORM=xcb). "
                                   "Текущая платформа: %1")
                        .arg(platform));
        return;
    }
    impl_->canvas = new OcctCanvas(this);
    impl_->layout->addWidget(impl_->canvas);
    impl_->canvas->onReady = [this] {
        impl_->error.clear();
        emit renderingChanged(true);
    };
    impl_->canvas->onSelection = [this](quint64 revision, int face) {
        emit selectionChanged(revision, face);
    };
    impl_->canvas->onError = [this, reportError](const QString &message) {
        // Switch pages after the native paint/input event has completed.
        QTimer::singleShot(0, this, [reportError, message] { reportError(message); });
    };
    impl_->layout->setCurrentWidget(impl_->canvas);
}

ViewerWidget::~ViewerWidget() = default;

bool ViewerWidget::isReady() const {
    return impl_->canvas && impl_->canvas->ready();
}

QString ViewerWidget::errorString() const {
    return impl_->error;
}

QImage ViewerWidget::captureImage(bool includeControls) {
    return impl_->canvas ? impl_->canvas->captureImage(includeControls) : QImage{};
}

QMenu *ViewerWidget::navigationMenu() const {
    return impl_->canvas ? impl_->canvas->navigationMenu() : nullptr;
}

void ViewerWidget::fitAll() {
    if (impl_->canvas) {
        impl_->canvas->fitAll();
    }
}

void ViewerWidget::setOrientation(Orientation orientation) {
    if (impl_->canvas) {
        impl_->canvas->setOrientation(orientation);
    }
}

QString ViewerWidget::presentDocument(application::DocumentPtr document, quint64 revision) {
    try {
        const auto native = std::dynamic_pointer_cast<const cad::CadDocument>(document);
        if (!native) {
            return QStringLiteral("Просмотрщик не поддерживает этот тип CAD-документа");
        }
        return impl_->canvas ? impl_->canvas->present(cad::OcctDocumentAccess::shape(*native),
                                                      std::move(document), revision)
                             : QString{};
    } catch (const Standard_Failure &error) {
        return failureText(error);
    } catch (const std::exception &error) {
        return QString::fromUtf8(error.what());
    }
}

void ViewerWidget::clearDocument(quint64 revision) {
    if (impl_->canvas) {
        impl_->canvas->clearDocument(revision);
    }
    emit selectionChanged(revision, 0);
}

void ViewerWidget::setCutoutsVisible(bool visible) {
    if (impl_->canvas) {
        impl_->canvas->setCutoutsVisible(visible);
    }
}
void ViewerWidget::setDimensionsVisible(bool visible) {
    if (impl_->canvas) {
        impl_->canvas->setDimensionsVisible(visible);
    }
}
bool ViewerWidget::dimensionsVisible() const {
    return impl_->canvas && impl_->canvas->dimensionsVisible();
}
void ViewerWidget::highlightFaces(const std::vector<std::size_t> &indices, quint64 revision) {
    if (impl_->canvas) {
        impl_->canvas->highlightFaces(indices, revision);
    }
}

void ViewerWidget::highlightBend(const application::RecognizedBend &bend, quint64 revision) {
    if (impl_->canvas) {
        impl_->canvas->highlightBend(bend, revision);
    }
}

void ViewerWidget::highlightBendDiagnostic(
    const application::BendInteriorContourDiagnostic &diagnostic, quint64 revision) {
    if (impl_->canvas) {
        impl_->canvas->highlightBendDiagnostic(diagnostic, revision);
    }
}

QString ViewerWidget::presentBendPreview(application::BendPreviewPtr preview,
                                         std::size_t frameIndex, quint64 revision) {
    try {
        return impl_->canvas
                   ? impl_->canvas->presentPreview(std::move(preview), frameIndex, revision)
                   : QString{};
    } catch (const Standard_Failure &error) {
        return failureText(error);
    } catch (const std::exception &error) {
        return QString::fromUtf8(error.what());
    }
}
QString ViewerWidget::endBendPreview() {
    return impl_->canvas ? impl_->canvas->endPreview() : QString{};
}

void ViewerWidget::showDiagnosticBox() {
    if (impl_->canvas) {
        const auto error =
            impl_->canvas->present(BRepPrimAPI_MakeBox(100.0, 60.0, 20.0).Shape(), {}, 0, true);
        if (!error.isEmpty()) {
            impl_->error = error;
        }
    }
}

} // namespace bending::viewer
