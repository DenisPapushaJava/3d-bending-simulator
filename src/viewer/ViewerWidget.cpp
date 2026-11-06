#include "bending/viewer/ViewerWidget.h"
#include "CollisionPresentation.h"
#include "Dimensions.h"
#include "PreviewRigidScene.h"
#include "SheetOrientationPresentation.h"
#include "bending/cad/occt/DocumentAccess.h"
#include "bending/cad/occt/FlatPatternAccess.h"
#include "bending/cad/occt/PreviewAccess.h"
#include <BRepBndLib.hxx>
#include <BRep_Builder.hxx>
#include <TopoDS_Compound.hxx>

#include <AIS_ColoredShape.hxx>
#include <AIS_InteractiveContext.hxx>
#include <AIS_Shape.hxx>
#include <AIS_ViewCube.hxx>
#include <Aspect_DisplayConnection.hxx>
#include <Aspect_NeutralWindow.hxx>
#include <BRepPrimAPI_MakeBox.hxx>
#include <BRep_Builder.hxx>
#include <Bnd_Box.hxx>
#include <Font_NameOfFont.hxx>
#include <Graphic3d_Camera.hxx>
#include <Graphic3d_TransformPers.hxx>
#include <Image_PixMap.hxx>
#include <OpenGl_Caps.hxx>
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
#include <QPointer>
#include <QResizeEvent>
#include <QStackedLayout>
#include <QTimer>
#include <QToolButton>
#include <QWheelEvent>

#include <algorithm>
#include <array>
#include <cmath>
#include <functional>
#include <numbers>
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

struct MachineAppearance {
    Quantity_Color color;
    double transparency;
    bool boundaries;
};
MachineAppearance machineAppearance(const cad::MachineDisplayComponent &component) {
    switch (component.displayRole) {
    case cad::MachineDisplayRole::Structure:
        return {Quantity_Color(component.moving ? .60 : .52, component.moving ? .66 : .58,
                               component.moving ? .72 : .64, Quantity_TOC_sRGB),
                .80, false};
    case cad::MachineDisplayRole::ToolMount:
        return {Quantity_Color(.61, .66, .70, Quantity_TOC_sRGB), .15, true};
    case cad::MachineDisplayRole::Backgauge:
        return {Quantity_Color(.40, .55, .57, Quantity_TOC_sRGB), .08, true};
    case cad::MachineDisplayRole::Unknown:
        // Unclassified/legacy display data retains the previous readable appearance.
        return {component.gauging
                    ? Quantity_Color(.28, .49, .58, Quantity_TOC_RGB)
                    : Quantity_Color(component.moving ? .40 : .31, component.moving ? .47 : .40,
                                     component.moving ? .55 : .48, Quantity_TOC_RGB),
                component.gauging ? .12 : .55, true};
    }
    throw std::logic_error("Unknown machine display role");
}

class OcctCanvas final : public QWidget {
    enum class CameraSpace : std::size_t { Cad, Flat, Kinematic, Press };
    enum class DisplayStyle { Edges, Shaded, Wireframe };
    enum class SceneSpace { Cad, Flat, Kinematic, Press };

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
        auto *refresh = navigationMenu_->addAction(QStringLiteral("Обновить вид"));
        refresh->setObjectName(QStringLiteral("refreshScene"));
        refresh->setShortcut(QKeySequence(Qt::Key_F5));
        refresh->setShortcutContext(Qt::WindowShortcut);
        refresh->setToolTip(QStringLiteral("Перерисовать сцену и панели без пересчёта гибки · F5"));
        addAction(refresh);
        connect(refresh, &QAction::triggered, this, [this] { refreshScene(); });
        machineVisibleAction_ = navigationMenu_->addAction(QStringLiteral("Показать станок"));
        machineVisibleAction_->setObjectName(QStringLiteral("showMachine"));
        machineVisibleAction_->setCheckable(true);
        machineVisibleAction_->setChecked(true);
        connect(machineVisibleAction_, &QAction::toggled, this, [this] {
            if (preview_)
                presentPreview(preview_, previewFrame_, revision_);
        });
        floorVisibleAction_ = navigationMenu_->addAction(QStringLiteral("Показать пол"));
        floorVisibleAction_->setObjectName(QStringLiteral("showFloor"));
        floorVisibleAction_->setCheckable(true);
        floorVisibleAction_->setChecked(true);
        floorVisibleAction_->setToolTip(
            QStringLiteral("Видимость пола не отключает проверку столкновений."));
        connect(floorVisibleAction_, &QAction::toggled, this, [this] {
            if (preview_)
                presentPreview(preview_, previewFrame_, revision_);
        });
        auto *fitMachine = navigationMenu_->addAction(QStringLiteral("Вписать станок"));
        fitMachineAction_ = fitMachine;
        fitMachine->setObjectName(QStringLiteral("fitMachine"));
        connect(fitMachine, &QAction::triggered, this, [this] {
            if (!ready_ || !preview_ || !machineVisibleAction_->isChecked())
                return;
            guarded([&] {
                fitPreview(*preview_, previewFrame_, true);
                view_->Redraw();
            });
        });
        enum class NavigationIcon { Home, Fit, Refresh };
        const auto icon = [](NavigationIcon type) {
            QIcon result;
            for (int ratio : {1, 2}) {
                QPixmap pixels(24 * ratio, 24 * ratio);
                pixels.setDevicePixelRatio(ratio);
                pixels.fill(Qt::transparent);
                QPainter painter(&pixels);
                painter.setRenderHint(QPainter::Antialiasing);
                painter.setPen(QPen(QColor("#1C2938"), 1.6));
                if (type == NavigationIcon::Home) {
                    painter.drawPolyline(QPolygonF{{3, 11}, {12, 3}, {21, 11}});
                    painter.drawPolyline(QPolygonF{{6, 10},
                                                   {6, 21},
                                                   {10, 21},
                                                   {10, 15},
                                                   {14, 15},
                                                   {14, 21},
                                                   {18, 21},
                                                   {18, 10}});
                } else if (type == NavigationIcon::Refresh) {
                    painter.drawArc(QRectF(4, 4, 16, 16), 45 * 16, 270 * 16);
                    painter.drawPolyline(QPolygonF{{13, 18}, {19, 18}, {19, 23}});
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
        home->setIcon(icon(NavigationIcon::Home));
        fit->setIcon(icon(NavigationIcon::Fit));
        refresh->setIcon(icon(NavigationIcon::Refresh));
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
        button(refresh, "refreshView")->setToolTip(refresh->toolTip());
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
        sideAction_ = navigationMenu_->addAction(QStringLiteral("Стороны A/B"));
        sideAction_->setObjectName(QStringLiteral("showSheetSides"));
        sideAction_->setCheckable(true);
        sideAction_->setEnabled(false);
        sideAction_->setToolTip(QStringLiteral("Две стороны листа и постоянная метка края 1"));
        connect(sideAction_, &QAction::toggled, this, [this] {
            if (preview_)
                presentPreview(preview_, previewFrame_, revision_);
            else if (flat_)
                presentFlat(flat_, revision_);
            else
                setHighlights(highlights_, revision_);
        });
        sideLegend_ = new QWidget(this);
        sideLegend_->setObjectName(QStringLiteral("sheetSideLegend"));
        sideLegend_->setAttribute(Qt::WA_NativeWindow);
        sideLegend_->setStyleSheet(
            QStringLiteral("QWidget#sheetSideLegend {background:#202A36;border:1px solid "
                           "#526171;border-radius:5px;}"
                           "QLabel {color:#E1E7EE;background:transparent;}"));
        auto *legendRow = new QHBoxLayout(sideLegend_);
        legendRow->setContentsMargins(9, 4, 4, 4);
        legendRow->setSpacing(10);
        sideLegendText_ = new QLabel(sideLegend_);
        legendRow->addWidget(sideLegendText_);
        nextEdge_ = new QToolButton(sideLegend_);
        nextEdge_->setObjectName(QStringLiteral("nextSheetEdge"));
        nextEdge_->setText(QStringLiteral("Другой край"));
        nextEdge_->setAccessibleName(QStringLiteral("Перенести метку 1 на другой край"));
        nextEdge_->setToolTip(
            QStringLiteral("Выбрать следующий свободный край базовой полки для метки 1"));
        nextEdge_->setStyleSheet(controls_->styleSheet());
        legendRow->addWidget(nextEdge_);
        connect(nextEdge_, &QToolButton::clicked, this, [this] {
            const auto &edges = sceneOrientation().edges;
            if (!edges.empty())
                edgeIndex_ = (edgeIndex_ + 1) % edges.size();
            refreshOrientation();
        });
        sideLegend_->hide();
        orbitLockAction_ = navigationMenu_->addAction(QStringLiteral("Обход вокруг Y"));
        orbitLockAction_->setObjectName(QStringLiteral("lockPressVertical"));
        orbitLockAction_->setCheckable(true);
        orbitLockAction_->setIconText(QStringLiteral("Y"));
        orbitLockAction_->setToolTip(QStringLiteral(
            "Выравнивает Y строго вверх, камеру — горизонтально. ПКМ: обход вокруг Y. "
            "СКМ: перенос, колесо: масштаб. Изометрия и вид сверху/снизу снимают якорь. "
            "Повторное нажатие возвращает свободное вращение."));
        orbitLockButton_ = new QToolButton(this);
        orbitLockButton_->setObjectName(QStringLiteral("pressOrbitLock"));
        orbitLockButton_->setAttribute(Qt::WA_NativeWindow);
        orbitLockButton_->setDefaultAction(orbitLockAction_);
        orbitLockButton_->setText(QStringLiteral("Y"));
        orbitLockButton_->setToolButtonStyle(Qt::ToolButtonTextBesideIcon);
        orbitLockButton_->setAccessibleName(QStringLiteral("Обход вокруг вертикальной оси Y"));
        orbitLockButton_->setFixedSize(62, 30);
        orbitLockButton_->setStyleSheet(controls_->styleSheet());
        QIcon anchor;
        for (int ratio : {1, 2})
            for (const auto state : {QIcon::Off, QIcon::On}) {
                QPixmap pixels(24 * ratio, 24 * ratio);
                pixels.setDevicePixelRatio(ratio);
                pixels.fill(Qt::transparent);
                QPainter painter(&pixels);
                painter.setRenderHint(QPainter::Antialiasing);
                painter.setPen(QPen(state == QIcon::On ? QColor(Qt::white) : QColor("#526171"), 1.7,
                                    Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));
                painter.drawEllipse(QPointF(12, 4), 2, 2);
                painter.drawLine(QPointF(12, 6), QPointF(12, 21));
                painter.drawLine(QPointF(8, 10), QPointF(16, 10));
                painter.drawArc(QRectF(4, 5, 16, 16), 180 * 16, 180 * 16);
                painter.drawPolyline(QPolygonF{{3, 16}, {4, 13}, {7, 15}});
                painter.drawPolyline(QPolygonF{{17, 15}, {20, 13}, {21, 16}});
                painter.end();
                anchor.addPixmap(pixels, QIcon::Normal, state);
            }
        orbitLockAction_->setIcon(anchor);
        orbitLockButton_->hide();
        dimensionsNotice_ = new QLabel(
            QStringLiteral("Размеры не помещаются. Увеличьте окно или впишите деталь (F)."), this);
        dimensionsNotice_->setObjectName(QStringLiteral("dimensionsNotice"));
        dimensionsNotice_->setWordWrap(true);
        dimensionsNotice_->setAttribute(Qt::WA_TransparentForMouseEvents);
        dimensionsNotice_->setStyleSheet(QStringLiteral(
            "QLabel { background: #fff3d6; color: #6b4b15; padding: 8px; border-radius: 5px; }"));
        dimensionsNotice_->hide();
        orbitLockAction_->setEnabled(false);
        connect(orbitLockAction_, &QAction::toggled, this, [this](bool locked) {
            if (locked && pressOrbitLocked())
                guarded([&] {
                    levelPressCamera();
                    refreshDimensions();
                    view_->Redraw();
                });
        });
    }

    ~OcctCanvas() override {
        release();
    }

    std::function<void(const QString &)> onError;
    std::function<void()> onReady;
    std::function<void(quint64, int)> onSelection;
    std::function<void(quint64, int)> onBendSelection;
    std::function<void()> onSurfaceChanged;

    QMenu *navigationMenu() const {
        return navigationMenu_;
    }
    QAction *sheetOrientationAction() const {
        return sideAction_;
    }
    void setBottomOverlay(QWidget *widget) {
        bottomOverlay_ = widget;
    }
    void setTopLeftOverlay(QWidget *widget) {
        topLeftOverlay_ = widget;
    }
    void layoutOrientationLegend() {
        positionSideLegend();
    }

    bool ready() const {
        return ready_;
    }

    void refreshScene() {
        guarded([&] {
            // Rebuild only GPU presentations from existing immutable shapes/meshes.
            // Keep selection, camera, machine filtering and the current simulation frame.
            for (const auto &object :
                 {Handle(AIS_InteractiveObject)(presentation_),
                  Handle(AIS_InteractiveObject)(evidence_), Handle(AIS_InteractiveObject)(support_),
                  Handle(AIS_InteractiveObject)(navigationCube_)}) {
                if (!object.IsNull())
                    context_->RecomputePrsOnly(object, false, true);
            }
            for (const auto &object : dimensions_.objects)
                context_->RecomputePrsOnly(object, false, true);
            for (const auto &object : sideMarkers_)
                context_->RecomputePrsOnly(object, false, true);
            rigidScene_->refresh();
            view_->MustBeResized();
            view_->ZFitAll();
            view_->Invalidate();
            view_->Redraw();
        });
        positionControls();
        if (onSurfaceChanged)
            onSurfaceChanged();
    }

    QString present(const TopoDS_Shape &shape, application::DocumentPtr document, quint64 revision,
                    bool diagnostic = false) {
        const bool sameSource = document && document == document_ && revision == revision_;
        const bool wasDerived = preview_ || flat_;
        if (sameSource)
            rememberSceneCamera();
        else
            sceneCameras_ = {};
        TopTools_IndexedMapOfShape faces, edges;
        TopExp::MapShapes(shape, TopAbs_FACE, faces);
        TopExp::MapShapes(shape, TopAbs_EDGE, edges);
        cad::SheetOrientationVisual orientation;
        if (const auto native = std::dynamic_pointer_cast<const cad::CadDocument>(document)) {
            const auto &tags = cad::OcctDocumentAccess::sheetOrientation(*native);
            if (tags.available &&
                tags.faces.size() == static_cast<std::size_t>(faces.Extent()) + 1) {
                for (int i = 1; i <= faces.Extent(); ++i)
                    if (tags.faces[i] != cad::SheetSide::Neutral)
                        orientation.sides[tags.faces[i] == cad::SheetSide::A ? 0 : 1].push_back(
                            faces(i));
                orientation.edges = tags.reference.edges;
            }
        }
        Handle(AIS_ColoredShape) candidate;
        if (ready_) {
            try {
                candidate = new AIS_ColoredShape(shape);
                candidate->Attributes()->SetFaceBoundaryDraw(displayStyle_ == DisplayStyle::Edges);
                candidate->Attributes()->SetAutoTriangulation(diagnostic);
                context_->SetColor(candidate, Quantity_Color(0.16, 0.62, 0.66, Quantity_TOC_RGB),
                                   false);
                if (sideAction_->isChecked())
                    applySheetSideColors(candidate, orientation);
                context_->Display(candidate,
                                  displayStyle_ == DisplayStyle::Wireframe ? AIS_WireFrame
                                                                           : AIS_Shaded,
                                  AIS_Shape::SelectionMode(TopAbs_FACE), false);
                if (dimensionsVisible_)
                    context_->Activate(candidate, AIS_Shape::SelectionMode(TopAbs_EDGE), false);
                if (!presentation_.IsNull()) {
                    context_->Remove(presentation_, false);
                }
                rigidScene_->clear();
                removeDimensions();
                if (!restoreSceneCamera(CameraSpace::Cad)) {
                    view_->SetProj(V3d_XposYnegZpos);
                    view_->FitAll(dimensionsVisible_ ? 0.3 : 0.1, false);
                }
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
        flat_.reset();
        shape_ = shape;
        faces_ = std::move(faces);
        edges_ = std::move(edges);
        if (!sameSource || wasDerived)
            measureSelection_ = {};
        measurements_.reset();
        if (const auto native = std::dynamic_pointer_cast<const cad::CadDocument>(document))
            measurements_ = cad::OcctDocumentAccess::measurements(*native);
        document_ = std::move(document);
        revision_ = revision;
        diagnostic_ = diagnostic;
        sourceOrientation_ = std::move(orientation);
        if (!sameSource)
            edgeIndex_ = 0;
        setHighlights(retainedHighlights, revision_);
        if (onSelection) {
            onSelection(revision_, 0);
        }
        return {};
    }

    void setToolSegmentColors(ToolSegmentColors colors) {
        if (segmentColors_ == colors)
            return;
        segmentColors_ = std::move(colors);
        if (preview_) {
            const auto preview = preview_;
            presentPreview(preview, previewFrame_, revision_);
        }
    }
    void updateRigidScene(const cad::PreviewFrame &frame) {
        std::vector<RigidDisplayBody> bodies;
        bodies.reserve(frame.machine.size() + 4);
        const std::array<Quantity_Color, 2> toolColors{
            Quantity_Color(.30, .36, .44, Quantity_TOC_RGB),
            Quantity_Color(.64, .70, .77, Quantity_TOC_RGB)};
        for (std::size_t tool = 0; tool < frame.tools.size(); ++tool) {
            if (frame.tools[tool].IsNull())
                continue;
            RigidDisplayBody body{tool == 0 ? "die" : "punch", frame.tools[tool], toolColors[tool]};
            const auto toLocal = frame.tools[tool].Location().Inverted();
            for (const auto &segment : frame.toolSegments[tool]) {
                const auto color = toolSegmentColor(segment.lengthMm, segmentColors_);
                body.colors.push_back({segment.shape.Moved(toLocal),
                                       Quantity_Color(color.redF(), color.greenF(), color.blueF(),
                                                      Quantity_TOC_sRGB)});
            }
            bodies.push_back(std::move(body));
        }
        for (const auto &c : frame.machine) {
            const auto appearance = machineAppearance(c);
            RigidDisplayBody body{c.id, c.shape, appearance.color, appearance.transparency,
                                  machineVisibleAction_->isChecked()};
            body.boundaries = appearance.boundaries;
            bodies.push_back(std::move(body));
        }
        if (frame.floor) {
            bodies.push_back({"floor-surface", frame.floor->surface,
                              Quantity_Color(.14, .19, .22, Quantity_TOC_RGB), .82,
                              floorVisibleAction_->isChecked()});
            bodies.push_back({"floor-grid", frame.floor->grid,
                              Quantity_Color(.25, .31, .36, Quantity_TOC_sRGB), 0,
                              floorVisibleAction_->isChecked()});
        }
        rigidScene_->update(std::move(bodies),
                            displayStyle_ == DisplayStyle::Wireframe ? AIS_WireFrame : AIS_Shaded,
                            displayStyle_ == DisplayStyle::Edges);
    }
    QString presentPreview(application::BendPreviewPtr preview, std::size_t index,
                           quint64 revision) {
        if (revision != revision_ || !document_ || !preview || index >= preview->frameCount()) {
            return QStringLiteral("Неактуальный результат просмотра сгиба");
        }
        const auto &frame = cad::OcctPreviewAccess::frame(*preview, index);
        const bool entering = preview_ != preview;
        // A new immutable result is still the same workspace. Reframe only when entering
        // from CAD or changing between the machine (Y-up) and kinematic (Z-up) spaces.
        const bool preserveView =
            preview_ && bool(preview_->toolSetup()) == bool(preview->toolSetup());
        if (entering && !preserveView)
            rememberSceneCamera();
        Handle(AIS_ColoredShape) candidate;
        if (ready_) {
            try {
                TopoDS_Compound display;
                BRep_Builder builder;
                builder.MakeCompound(display);
                // Both arrays describe the complete sheet. Keep the original cutout skin,
                // with its face identities; rigid tools are displayed independently below.
                const auto &regions =
                    frame.displayShape.IsNull() ? frame.regions : frame.displayRegions;
                for (const auto &region : regions)
                    if (!region.IsNull())
                        builder.Add(display, region);
                candidate = new CollisionPresentation(
                    display, frame.collisionHighlights, machineVisibleAction_->isChecked(),
                    floorVisibleAction_->isChecked(), frame.floorContacts, frame.backgaugeContacts);
                candidate->Attributes()->SetAutoTriangulation(false);
                candidate->Attributes()->SetFaceBoundaryDraw(displayStyle_ == DisplayStyle::Edges);
                const std::array<Quantity_Color, 3> colors{
                    Quantity_Color(0.15, 0.48, 0.65, Quantity_TOC_RGB),
                    Quantity_Color(1.0, 0.25, 0.04, Quantity_TOC_RGB),
                    Quantity_Color(0.16, 0.70, 0.51, Quantity_TOC_RGB)};
                for (std::size_t region = 0; region < regions.size(); ++region) {
                    TopTools_IndexedMapOfShape regionFaces;
                    TopExp::MapShapes(regions[region], TopAbs_FACE, regionFaces);
                    for (int face = 1; face <= regionFaces.Extent(); ++face) {
                        candidate->SetCustomColor(
                            regionFaces(face),
                            sideAction_->isChecked()
                                ? Quantity_Color(.57, .61, .65, Quantity_TOC_sRGB)
                                : colors[region]);
                        candidate->SetCustomTransparency(regionFaces(face), 0);
                        if (sideAction_->isChecked() && region == 1) {
                            const auto drawer = candidate->CustomAspects(regionFaces(face));
                            drawer->SetFaceBoundaryDraw(true);
                            drawer->SetFaceBoundaryAspect(new Prs3d_LineAspect(
                                Quantity_Color(.95, .70, .27, Quantity_TOC_sRGB), Aspect_TOL_SOLID,
                                2));
                        }
                    }
                }
                if (sideAction_->isChecked())
                    applySheetSideColors(candidate, frame.orientation);
                if (hoverPreviewBend_ && *hoverPreviewBend_ < frame.bendZones.size()) {
                    TopTools_IndexedMapOfShape faces;
                    TopExp::MapShapes(frame.bendZones[*hoverPreviewBend_], TopAbs_FACE, faces);
                    for (int f = 1; f <= faces.Extent(); ++f)
                        candidate->SetCustomColor(faces(f),
                                                  Quantity_Color(.92, .65, .22, Quantity_TOC_RGB));
                }
                context_->Display(candidate,
                                  displayStyle_ == DisplayStyle::Wireframe ? AIS_WireFrame
                                                                           : AIS_Shaded,
                                  -1, false);
                updateRigidScene(frame);
                if (!presentation_.IsNull()) {
                    context_->Remove(presentation_, false);
                }
                for (const auto &overlay : {evidence_, support_}) {
                    if (!overlay.IsNull()) {
                        context_->Remove(overlay, false);
                    }
                }
                context_->ClearSelected(false);
                if (entering && !preserveView &&
                    !restoreSceneCamera(preview->toolSetup() ? CameraSpace::Press
                                                             : CameraSpace::Kinematic)) {
                    if (preview->toolSetup()) {
                        // Y is vertical, Z runs along the bend line. From +Z the
                        // right-hand profile shows the operator's open throat to the left.
                        view_->SetProj(V3d_Zpos, true);
                        view_->SetUp(0.0, 1.0, 0.0);
                        if (orbitLockAction_->isChecked())
                            levelPressCamera();
                    } else {
                        view_->SetProj(V3d_XposYnegZpos);
                    }
                    fitPreview(*preview, index, preview->toolSetup() != nullptr);
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
        flat_.reset();
        previewFrame_ = index;
        refreshDimensions();
        refreshOrientation();
        if (onSelection) {
            onSelection(revision_, 0);
        }
        return {};
    }
    QString endPreview() {
        QString error;
        try {
            error = (preview_ || flat_) ? present(shape_, document_, revision_) : QString{};
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

    QString presentFlat(application::FlatPatternPtr flat, quint64 revision) {
        if (revision != revision_ || !document_ || !flat)
            return QStringLiteral("Неактуальная развёртка");
        const bool entering = flat_ != flat;
        if (entering)
            measureSelection_ = {};
        if (entering)
            rememberSceneCamera();
        Handle(AIS_ColoredShape) candidate;
        if (ready_) {
            try {
                candidate = new AIS_ColoredShape(cad::OcctFlatPatternAccess::shape(*flat));
                candidate->Attributes()->SetAutoTriangulation(false);
                candidate->Attributes()->SetFaceBoundaryDraw(displayStyle_ == DisplayStyle::Edges);
                candidate->SetColor(Quantity_Color(.25, .64, .73, Quantity_TOC_RGB));
                if (sideAction_->isChecked())
                    applySheetSideColors(candidate, cad::OcctFlatPatternAccess::orientation(*flat));
                for (const auto &line : cad::OcctFlatPatternAccess::bendLines(*flat)) {
                    candidate->SetCustomColor(line, Quantity_Color(1., .45, .08, Quantity_TOC_RGB));
                    candidate->SetCustomWidth(line, 2.5);
                }
                context_->Display(candidate,
                                  displayStyle_ == DisplayStyle::Wireframe ? AIS_WireFrame
                                                                           : AIS_Shaded,
                                  -1, false);
                for (const auto &old : {Handle(AIS_Shape)(presentation_), evidence_, support_})
                    if (!old.IsNull())
                        context_->Remove(old, false);
                rigidScene_->clear();
                removeDimensions();
                context_->ClearSelected(false);
                if (entering && !restoreSceneCamera(CameraSpace::Flat)) {
                    view_->SetProj(V3d_Zpos);
                    view_->FitAll(.25, false);
                }
                view_->ZFitAll();
            } catch (const Standard_Failure &e) {
                restore(candidate);
                return failureText(e);
            } catch (const std::exception &e) {
                restore(candidate);
                return QString::fromUtf8(e.what());
            }
        }
        presentation_ = candidate;
        evidence_.Nullify();
        support_.Nullify();
        preview_.reset();
        flat_ = std::move(flat);
        refreshFlatSelection();
        refreshDimensions();
        refreshOrientation();
        if (onSelection)
            onSelection(revision_, 0);
        return {};
    }

    void clearDocument(quint64 revision) {
        guarded([&] {
            rigidScene_->clear();
            context_->RemoveAll(false);
            context_->ClearSelected(false);
            view_->Redraw();
        });
        presentation_.Nullify();
        evidence_.Nullify();
        support_.Nullify();
        dimensions_.objects.clear();
        dimensions_.labels.clear();
        sideMarkers_.clear();
        sourceOrientation_ = {};
        highlights_ = {};
        shape_.Nullify();
        faces_.Clear();
        edges_.Clear();
        measurements_.reset();
        measureSelection_ = {};
        document_.reset();
        preview_.reset();
        flat_.reset();
        revision_ = revision;
        sceneCameras_ = {};
        diagnostic_ = false;
        updateDimensionButton();
        refreshOrientation();
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
        guarded([&] {
            if (!preview_ && !flat_ && !presentation_.IsNull()) {
                if (visible)
                    context_->Activate(presentation_, AIS_Shape::SelectionMode(TopAbs_EDGE), false);
                else
                    context_->Deactivate(presentation_, AIS_Shape::SelectionMode(TopAbs_EDGE));
            }
        });
        dimensionsAction_->setChecked(visible);
        refreshDimensions();
        if (visible) {
            fitAll();
        }
    }
    void highlightFaces(const std::vector<std::size_t> &indices, quint64 revision) {
        if (revision == revision_)
            measureSelection_ = {};
        setHighlights({indices, {}, false}, revision);
    }

    void highlightBend(const application::RecognizedBend &bend, quint64 revision) {
        if (revision == revision_)
            measureSelection_ = {};
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
        if (flat_) {
            guarded([&] { refreshFlatSelection(); });
            refreshDimensions();
            return;
        }
        if (preview_)
            return;
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
                context_->SetColor(
                    presentation_,
                    sideAction_->isChecked() ? Quantity_Color(.57, .61, .65, Quantity_TOC_sRGB)
                    : highlights_.bend       ? Quantity_Color(0.27, 0.35, 0.40, Quantity_TOC_RGB)
                                             : Quantity_Color(0.16, 0.62, 0.66, Quantity_TOC_RGB),
                    false);
                if (sideAction_->isChecked())
                    applySheetSideColors(presentation_, sourceOrientation_);
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
            evidence_ =
                outline(sideAction_->isChecked() && !highlights_.bend ? std::vector<std::size_t>{}
                                                                      : highlights_.primary,
                        highlights_.bend ? Quantity_Color(1.0, 0.85, 0.40, Quantity_TOC_RGB)
                                         : Quantity_Color(1.0, 0.65, 0.1, Quantity_TOC_RGB),
                        3.0);
            view_->Redraw();
        });
        refreshDimensions();
        refreshOrientation();
    }

    void fitAll() {
        if (shape_.IsNull()) {
            return;
        }
        guarded([&] {
            if (preview_) {
                fitPreview(*preview_, previewFrame_, false);
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
                    view_->SetProj(-1.0, 0.5, 3.0);
                    view_->SetUp(0.0, 1.0, 0.0);
                    releaseOrbitForInclinedView();
                    fitAll();
                    return;
                }
                break;
            case ViewerWidget::Orientation::Front:
                projection = yup ? V3d_Xneg : V3d_Yneg;
                break;
            case ViewerWidget::Orientation::Back:
                projection = yup ? V3d_Xpos : V3d_Ypos;
                break;
            case ViewerWidget::Orientation::Top:
                projection = yup ? V3d_Ypos : V3d_Zpos;
                break;
            case ViewerWidget::Orientation::Bottom:
                projection = yup ? V3d_Yneg : V3d_Zneg;
                break;
            case ViewerWidget::Orientation::Left:
                projection = yup ? V3d_Zneg : V3d_Xneg;
                break;
            case ViewerWidget::Orientation::Right:
                projection = yup ? V3d_Zpos : V3d_Xpos;
                break;
            }
            view_->SetProj(projection, yup);
            releaseOrbitForInclinedView();
            fitAll();
        });
    }

    void setDisplayStyle(DisplayStyle value) {
        displayStyle_ = value;
        guarded([&] {
            rigidScene_->setStyle(value == DisplayStyle::Wireframe ? AIS_WireFrame : AIS_Shaded,
                                  value == DisplayStyle::Edges);
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
            if (orbitLockButton_->isVisible())
                painter.drawPixmap(orbitLockButton_->pos(), orbitLockButton_->grab());
            if (sideLegend_->isVisible())
                painter.drawPixmap(sideLegend_->pos(), sideLegend_->grab());
        }
        return result;
    }

  protected:
    bool event(QEvent *event) override {
        const bool handled = QWidget::event(event);
        if (event->type() == QEvent::WinIdChange && ready_) {
            restoreCamera_ = new Graphic3d_Camera(view_->Camera());
            restoreRevision_ = revision_;
            restoreSpace_ = sceneSpace();
            release();
            attempted_ = false;
            update();
        } else if (event->type() == QEvent::WindowActivate) {
            guarded([&] { view_->Invalidate(); });
            positionControls();
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
        positionControls();
        updateNavigationCube();
        refreshDimensions();
        guarded([&] { view_->Redraw(); });
        if (onSurfaceChanged)
            onSurfaceChanged();
    }

    void showEvent(QShowEvent *) override {
        if (!window_.IsNull()) {
            window_->Map();
        }
        guarded([&] { view_->Invalidate(); });
        positionControls();
        if (onSurfaceChanged)
            onSurfaceChanged();
        update();
    }

    void mousePressEvent(QMouseEvent *event) override {
        setFocus();
        lastPosition_ = physicalPoint(event->position());
        guarded([&] {
            if (event->button() == Qt::RightButton) {
                if (!pressOrbitLocked())
                    view_->StartRotation(lastPosition_.x(), lastPosition_.y());
            } else if (event->button() == Qt::LeftButton) {
                context_->MoveTo(lastPosition_.x(), lastPosition_.y(), view_, false);
                const auto cubeOwner =
                    Handle(AIS_ViewCubeOwner)::DownCast(context_->DetectedOwner());
                if (!cubeOwner.IsNull()) {
                    view_->SetProj(cubeOwner->MainOrientation(), preview_ && preview_->toolSetup());
                    releaseOrbitForInclinedView();
                    fitAll();
                    return;
                }
                if (flat_) {
                    const auto *measures = flat_->info().measurements.get();
                    auto feature =
                        dimensionsVisible_ && measures &&
                                std::abs(view_->Camera()->Direction().Z()) >= .15
                            ? flatMeasurementAt(*measures, dimensionLayout(), lastPosition_)
                            : MeasurementSelection{};
                    if (feature.kind == MeasurementKind::Opening) {
                        selectMeasurement(feature);
                        return;
                    }
                    if (feature.kind == MeasurementKind::Edge &&
                        selectBase(feature, event->modifiers()))
                        return;
                    const auto hit = std::abs(view_->Camera()->Direction().Z()) < .15
                                         ? std::nullopt
                                         : flatBendAt(flat_->info(), dimensionLayout(),
                                                      lastPosition_, 8 * devicePixelRatioF());
                    const int source = hit && *hit < document_->metadata().bendAnalysis.bends.size()
                                           ? static_cast<int>(*hit)
                                           : -1;
                    if (source < 0 && feature.kind != MeasurementKind::None) {
                        if (!selectBase(feature, event->modifiers()))
                            selectMeasurement(feature);
                        return;
                    }
                    if (source >= 0)
                        highlightBend(document_->metadata().bendAnalysis.bends[source], revision_);
                    else
                        highlightFaces({}, revision_);
                    if (onBendSelection)
                        onBendSelection(revision_, source);
                } else if (!preview_) {
                    context_->SelectDetected(AIS_SelectionScheme_Replace);
                    context_->InitSelected();
                    int face = 0;
                    if (context_->MoreSelected() && context_->HasSelectedShape()) {
                        const auto shape = context_->SelectedShape();
                        if (shape.ShapeType() == TopAbs_FACE)
                            face = faces_.FindIndex(shape);
                        if (dimensionsVisible_ && measurements_) {
                            const auto edge = edges_.FindIndex(shape);
                            MeasurementSelection selected{
                                face > 0 ? MeasurementKind::Face : MeasurementKind::Edge,
                                static_cast<std::size_t>(face > 0 ? face : edge),
                                {}};
                            if (edge > 0 && selectBase(selected, event->modifiers()))
                                return;
                            for (const auto &opening : measurements_->openings)
                                if ((edge > 0 &&
                                     std::find(opening.edges.begin(), opening.edges.end(),
                                               static_cast<std::size_t>(edge)) !=
                                         opening.edges.end()) ||
                                    (face > 0 &&
                                     std::find(opening.faces.begin(), opening.faces.end(),
                                               static_cast<std::size_t>(face)) !=
                                         opening.faces.end())) {
                                    selected = {MeasurementKind::Opening, opening.id, {}};
                                    break;
                                }
                            if (face > 0 && selected.kind != MeasurementKind::Opening) {
                                const auto &bends = document_->metadata().bendAnalysis.bends;
                                for (std::size_t i = 0; i < bends.size(); ++i)
                                    if (bends[i].innerFace == static_cast<std::size_t>(face) ||
                                        bends[i].outerFace == static_cast<std::size_t>(face)) {
                                        highlightBend(bends[i], revision_);
                                        if (onBendSelection)
                                            onBendSelection(revision_, static_cast<int>(i));
                                        return;
                                    }
                            }
                            if (face > 0 || edge > 0) {
                                selectMeasurement(selected);
                                return;
                            }
                        }
                    }
                    if (dimensionsVisible_) {
                        selectMeasurement({});
                        return;
                    }
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
                if (pressOrbitLocked()) {
                    const auto camera = view_->Camera();
                    gp_Trsf orbit;
                    orbit.SetRotation(gp_Ax1(camera->Center(), gp_Dir(0, 1, 0)),
                                      -2 * std::numbers::pi * (position.x() - lastPosition_.x()) /
                                          physicalSize().width());
                    camera->Transform(orbit);
                    levelPressCamera();
                } else
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
        if (event->key() == Qt::Key_Escape && dimensionsVisible_ && !preview_) {
            selectMeasurement({});
            event->accept();
        } else if (event->key() == Qt::Key_F) {
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
                navigationCube_->Attributes()->TextAspect()->SetFont(Font_NOF_SANS_SERIF);
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
                label(V3d_Xpos, yup ? "СЗАДИ" : "СПРАВА");
                label(V3d_Xneg, yup ? "СПЕРЕДИ" : "СЛЕВА");
                label(yup ? V3d_Ypos : V3d_Zpos, "СВЕРХУ");
                label(yup ? V3d_Yneg : V3d_Zneg, "СНИЗУ");
                label(yup ? V3d_Zpos : V3d_Yneg, yup ? "СПРАВА" : "СПЕРЕДИ");
                label(yup ? V3d_Zneg : V3d_Ypos, yup ? "СЛЕВА" : "СЗАДИ");
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
        const bool press = preview_ && preview_->toolSetup();
        const bool machine = press && preview_->machineModel() != core::MachineModel::None;
        const bool floor = press && preview_->machineModel() == core::MachineModel::DemoPressV3;
        floorVisibleAction_->setVisible(floor);
        floorVisibleAction_->setEnabled(hasGeometry && floor);
        for (auto *action : {machineVisibleAction_, fitMachineAction_}) {
            action->setVisible(machine);
            action->setEnabled(hasGeometry && machine);
        }
        fitMachineAction_->setEnabled(hasGeometry && machine && machineVisibleAction_->isChecked());
        if (orbitLockAction_) {
            orbitLockAction_->setEnabled(hasGeometry && press);
            orbitLockButton_->setVisible(press);
            orbitLockButton_->setEnabled(hasGeometry && press);
            positionOrbitLock();
        }
        updateNavigationCube();
        dimensionsAction_->setEnabled(ready_ && document_ != nullptr);
        const auto description =
            dimensions_.labels.join(QStringLiteral("\n")) +
            (dimensions_.crowded
                 ? QStringLiteral("\nРазмеры не помещаются: увеличьте окно или приблизьте деталь.")
                 : QString{});
        const auto scope =
            flat_ ? QStringLiteral("Развёртка: выберите линию гиба или гиб в списке. Координаты — "
                                   "от края заготовки, не X упора.")
            : preview_ ? QStringLiteral("Размеры текущего положения. "
                                        "полок от касания зоны, по их габаритам.")
                       : QStringLiteral("Габариты CAD по X/Y/Z и подтверждённые наружные размеры "
                                        "полок. Для выбранного гиба — "
                                        "измерения исходной модели.");
        dimensionsButton_->setToolTip(
            QStringLiteral("Показать / скрыть размеры · Ctrl+D\nКлик — измерение; Esc — снять "
                           "выбор. Для отверстия Shift+клик по кромке — выбрать базу.\n%1\n%2")
                .arg(scope, description));
        dimensionsButton_->setAccessibleDescription(scope + QLatin1Char('\n') + description);
        if (dimensionsNotice_) {
            dimensionsNotice_->setFixedWidth(std::min(300, std::max(160, width() - 24)));
            dimensionsNotice_->adjustSize();
            dimensionsNotice_->move(12, topLeftOverlay_ && topLeftOverlay_->isVisible()
                                            ? topLeftOverlay_->geometry().bottom() + 8
                                            : 12);
            dimensionsNotice_->setVisible(dimensionsVisible_ && document_ && dimensions_.crowded);
            dimensionsNotice_->raise();
        }
    }
    void selectMeasurement(MeasurementSelection selection) {
        highlightFaces(selection.kind == MeasurementKind::Face && !flat_
                           ? std::vector<std::size_t>{selection.id}
                           : std::vector<std::size_t>{},
                       revision_);
        measureSelection_ = std::move(selection);
        guarded([&] { context_->ClearSelected(false); });
        refreshDimensions();
        if (onBendSelection)
            onBendSelection(revision_, -1);
    }
    bool selectBase(MeasurementSelection selection, Qt::KeyboardModifiers modifiers) {
        if (!modifiers.testFlag(Qt::ShiftModifier) ||
            measureSelection_.kind != MeasurementKind::Opening ||
            selection.kind != MeasurementKind::Edge)
            return false;
        const auto *measures = flat_ ? flat_->info().measurements.get() : measurements_.get();
        if (!measures)
            return false;
        const auto edge = std::find_if(measures->edges.begin(), measures->edges.end(),
                                       [&](const auto &e) { return e.id == selection.id; });
        const auto opening =
            std::find_if(measures->openings.begin(), measures->openings.end(),
                         [&](const auto &o) { return o.id == measureSelection_.id; });
        if (edge == measures->edges.end() || opening == measures->openings.end() ||
            !edge->straight || edge->bendBoundary ||
            (std::find(measures->datumEdges.begin(), measures->datumEdges.end(), edge->id) ==
                 measures->datumEdges.end() &&
             std::none_of(measures->faces.begin(), measures->faces.end(), [&](const auto &face) {
                 return std::find(opening->hostFaces.begin(), opening->hostFaces.end(), face.id) !=
                            opening->hostFaces.end() &&
                        std::find(face.outerEdges.begin(), face.outerEdges.end(), edge->id) !=
                            face.outerEdges.end();
             })))
            return true;
        const gp_Pnt a(edge->first.x, edge->first.y, edge->first.z),
            b(edge->second.x, edge->second.y, edge->second.z);
        gp_Pnt center(opening->center.x, opening->center.y, opening->center.z);
        const gp_Vec normal(opening->normal.x, opening->normal.y, opening->normal.z), ab(a, b);
        if (normal.SquareMagnitude() <= 1.e-12 || ab.SquareMagnitude() <= 1.e-12)
            return true;
        center.Translate(normal * (gp_Vec(center, a).Dot(normal) / normal.SquareMagnitude()));
        const double foot = gp_Vec(a, center).Dot(ab) / ab.SquareMagnitude();
        if (foot < -1.e-8 || foot > 1. + 1.e-8)
            return true;
        auto &bases = measureSelection_.bases;
        if (std::find(bases.begin(), bases.end(), edge->id) == bases.end()) {
            if (bases.size() == 2)
                bases.clear();
            bases.push_back(edge->id);
        }
        refreshDimensions();
        return true;
    }
    std::optional<std::size_t> selectedFlatBend() const {
        if (!flat_ || !document_ || !highlights_.measuredBend)
            return std::nullopt;
        const auto &bends = document_->metadata().bendAnalysis.bends;
        const auto &selected = *highlights_.measuredBend;
        for (std::size_t i = 0; i < bends.size(); ++i)
            if (bends[i].innerFace == selected.innerFace &&
                bends[i].outerFace == selected.outerFace)
                return i;
        return std::nullopt;
    }
    void refreshFlatSelection() {
        if (!ready_ || !flat_ || presentation_.IsNull())
            return;
        const auto selected = selectedFlatBend();
        const auto &lines = cad::OcctFlatPatternAccess::bendLines(*flat_);
        const auto &bends = flat_->info().bends;
        for (std::size_t i = 0; i < std::min(lines.size(), bends.size()); ++i) {
            const bool active = selected && bends[i].bendIndex == *selected;
            presentation_->SetCustomColor(lines[i],
                                          active ? Quantity_Color(1., .85, .4, Quantity_TOC_RGB)
                                                 : Quantity_Color(1., .45, .08, Quantity_TOC_RGB));
            presentation_->SetCustomWidth(lines[i], active ? 4. : 2.5);
        }
        context_->Redisplay(presentation_, false);
        if (!dimensionsVisible_)
            view_->Redraw();
    }
    DimensionLayout dimensionLayout() const {
        const auto separation = view_->Convert(qRound(50.0 * devicePixelRatioF()));
        double dx, dy, dz, ux, uy, uz;
        view_->Proj(dx, dy, dz);
        view_->Up(ux, uy, uz);
        const auto up = gp_Vec(ux, uy, uz).Normalized() * separation;
        const auto right = up.Crossed(gp_Vec(dx, dy, dz)).Normalized() * separation;
        DimensionLayout layout{right, up, {}, {}, {}};
        layout.project = [this](const gp_Pnt &point) {
            int x, y;
            view_->Convert(point.X(), point.Y(), point.Z(), x, y);
            return QPointF(x, y);
        };
        const auto size = physicalSize();
        const auto ratio = devicePixelRatioF();
        layout.viewport =
            QRectF(8 * ratio, 8 * ratio, size.width() - 16 * ratio, size.height() - 16 * ratio);
        layout.excluded.emplace_back(size.width() - 210 * ratio, 0, 210 * ratio, 220 * ratio);
        for (const auto *overlay :
             std::array<QWidget *, 3>{bottomOverlay_.data(), topLeftOverlay_.data(), sideLegend_})
            if (overlay && overlay->isVisible())
                layout.excluded.emplace_back(overlay->x() * ratio, overlay->y() * ratio,
                                             overlay->width() * ratio, overlay->height() * ratio);

        layout.unproject = [this](QPointF pixel, const gp_Pnt &anchor) {
            double x, y, z, rayX, rayY, rayZ;
            view_->ConvertWithProj(qRound(pixel.x()), qRound(pixel.y()), x, y, z, rayX, rayY, rayZ);
            const gp_Pnt origin(x, y, z);
            const gp_Vec ray(rayX, rayY, rayZ), normal(view_->Camera()->Direction());
            const auto denominator = ray.Dot(normal);
            return std::abs(denominator) > 1.e-12
                       ? origin.Translated(ray * (gp_Vec(origin, anchor).Dot(normal) / denominator))
                       : anchor;
        };
        return layout;
    }
    void refreshDimensions() {
        if (!dimensionsVisible_ && dimensions_.objects.empty()) {
            updateDimensionButton();
            return;
        }
        guarded([&] {
            removeDimensions();
            if (dimensionsVisible_ && document_) {
                const auto layout = dimensionLayout();
                dimensions_ =
                    flat_ ? flatPatternDimensions(flat_->info(), 14.0 * devicePixelRatioF(), layout,
                                                  selectedFlatBend(),
                                                  -view_->Camera()->Direction().Z(),
                                                  measureSelection_)
                    : preview_
                        ? previewDimensions(cad::OcctPreviewAccess::frame(*preview_, previewFrame_),
                                            preview_->model(), 14.0 * devicePixelRatioF(), layout,
                                            animationRunning_)
                        : documentDimensions(document_->metadata(), faces_,
                                             highlights_.measuredBend, 14.0 * devicePixelRatioF(),
                                             layout, measurements_.get(), measureSelection_);
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
                                  (preview_ || flat_) ? -1 : AIS_Shape::SelectionMode(TopAbs_FACE),
                                  false);
            }
            if (preview_)
                updateRigidScene(cad::OcctPreviewAccess::frame(*preview_, previewFrame_));
            else
                rigidScene_->clear();
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
    void fitPreview(const application::SingleBendPreview &preview, std::size_t index,
                    bool includeMachine) {
        const auto bounds = preview.bounds();
        Bnd_Box box;
        box.Add(gp_Pnt(bounds.minimum.x, bounds.minimum.y, bounds.minimum.z));
        box.Add(gp_Pnt(bounds.maximum.x, bounds.maximum.y, bounds.maximum.z));
        if (includeMachine && machineVisibleAction_->isChecked())
            for (const auto &component : cad::OcctPreviewAccess::frame(preview, index).machine)
                BRepBndLib::Add(component.shape, box);
        view_->FitAll(box, dimensionsVisible_ ? 0.3 : 0.1, false);
        reserveTransportSpace();
        view_->ZFitAll();
    }
    void reserveTransportSpace() {
        const bool bottomVisible = bottomOverlay_ && bottomOverlay_->isVisible();
        const bool topVisible = topLeftOverlay_ && topLeftOverlay_->isVisible();
        if (bottomVisible || topVisible) {
            const auto size = physicalSize();
            const int hudHeight = std::max(bottomVisible ? bottomOverlay_->height() : 0,
                                           topVisible ? topLeftOverlay_->height() : 0);
            const int inset = qRound((hudHeight + 24) * devicePixelRatioF());
            // Symmetric padding keeps the original orbit target while leaving
            // the lower part of the fitted geometry clear of the transport.
            const int available = std::max(size.height() / 3, size.height() - 2 * inset);
            const auto camera = view_->Camera();
            camera->SetScale(camera->Scale() * size.height() / available);
        }
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
            if (restoreRevision_ != revision_ || restoreSpace_ != sceneSpace())
                restoreCamera_.Nullify();
            // app configures runtime resources before starting any CAD worker.
            display_ = new Aspect_DisplayConnection();
            driver_ = new OpenGl_GraphicDriver(display_);
            // Explicit developer benchmark only; normal interactive rendering retains VSync.
            if (qEnvironmentVariableIntValue("BENDING_VIEWER_BENCHMARK_NO_VSYNC") == 1)
                driver_->ChangeOptions().swapInterval = 0;
            scene_ = new V3d_Viewer(driver_);
            Handle(V3d_AmbientLight) ambient = new V3d_AmbientLight();
            scene_->AddLight(ambient);
            // Broad fill from all sides: retain enough shading to read bends,
            // without a single dominant light or a dark back/underside.
            for (const auto &[direction, intensity] : std::array{
                     std::pair{gp_Dir(-1, -1, -1), 0.32f}, std::pair{gp_Dir(1, -1, 1), 0.24f},
                     std::pair{gp_Dir(1, 1, -1), 0.40f}, std::pair{gp_Dir(-1, 1, 1), 0.12f}}) {
                // Classic shading weights light by linear RGB, rather than the
                // ray/path-tracing Intensity() property.
                Handle(V3d_DirectionalLight) fill = new V3d_DirectionalLight(
                    direction, Quantity_Color(intensity, intensity, intensity, Quantity_TOC_RGB));
                scene_->AddLight(fill);
            }
            scene_->SetLightOn();
            context_ = new AIS_InteractiveContext(scene_);
            rigidScene_ = std::make_unique<PreviewRigidScene>(context_);
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
            if (flat_) {
                const auto flat = flat_;
                if (restoreCamera_.IsNull())
                    flat_.reset();
                const auto error = presentFlat(flat, revision_);
                if (!error.isEmpty())
                    throw std::runtime_error(error.toStdString());
            } else if (preview_) {
                // Rebuild native resources from the same immutable worker snapshot.
                const auto preview = preview_;
                const auto index = previewFrame_;
                if (restoreCamera_.IsNull())
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
            if (!restoreCamera_.IsNull()) {
                restoreCamera_->SetAspect(static_cast<double>(size.width()) / size.height());
                view_->SetCamera(restoreCamera_);
                restoreCamera_.Nullify();
                view_->ZFitAll();
                view_->Invalidate();
            }
            positionControls();
            if (onSurfaceChanged)
                onSurfaceChanged();
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
        rigidScene_.reset();
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
    std::unique_ptr<PreviewRigidScene> rigidScene_;
    Handle(AIS_Shape) evidence_;
    Handle(AIS_Shape) support_;
    FaceHighlights highlights_;
    bool cutoutsVisible_ = true;
    bool dimensionsVisible_ = false;
    QToolButton *dimensionsButton_ = nullptr;
    QAction *dimensionsAction_ = nullptr;
    QMenu *navigationMenu_ = nullptr;
    QAction *machineVisibleAction_ = nullptr;
    QAction *floorVisibleAction_ = nullptr;
    QAction *fitMachineAction_ = nullptr;
    QAction *orbitLockAction_ = nullptr;
    QPointer<QWidget> bottomOverlay_;
    QPointer<QWidget> topLeftOverlay_;
    std::array<Handle(Graphic3d_Camera), 4> sceneCameras_;
    CameraSpace cameraSpace() const {
        return flat_                   ? CameraSpace::Flat
               : !preview_             ? CameraSpace::Cad
               : preview_->toolSetup() ? CameraSpace::Press
                                       : CameraSpace::Kinematic;
    }
    void rememberSceneCamera() {
        if (ready_ && !view_.IsNull() && restoreCamera_.IsNull())
            sceneCameras_[static_cast<std::size_t>(cameraSpace())] =
                new Graphic3d_Camera(view_->Camera());
    }
    bool restoreSceneCamera(CameraSpace space) {
        const auto &saved = sceneCameras_[static_cast<std::size_t>(space)];
        if (saved.IsNull())
            return false;
        Handle(Graphic3d_Camera) camera = new Graphic3d_Camera(saved);
        const auto size = physicalSize();
        camera->SetAspect(static_cast<double>(size.width()) / size.height());
        view_->SetCamera(camera);
        return true;
    }
    QToolButton *orbitLockButton_ = nullptr;
    bool pressOrbitLocked() const {
        return orbitLockAction_ && orbitLockAction_->isChecked() && preview_ &&
               preview_->toolSetup();
    }
    void positionOrbitLock() {
        if (orbitLockButton_)
            orbitLockButton_->move(qMax(0, width() - 82 - orbitLockButton_->width() / 2), 174);
    }
    void levelPressCamera() {
        const auto camera = view_->Camera();
        const auto direction = camera->Direction();
        // Preserve azimuth, target, distance and scale; remove both pitch and roll.
        // At a top/bottom pole azimuth is undefined, so use the press home azimuth.
        camera->SetDirection(direction.X() * direction.X() + direction.Z() * direction.Z() > 1.e-10
                                 ? gp_Dir(direction.X(), 0, direction.Z())
                                 : gp_Dir(1, 0, -3));
        camera->SetUp(gp_Dir(0, 1, 0));
    }
    void releaseOrbitForInclinedView() {
        if (!pressOrbitLocked())
            return;
        // Honour explicit cube/menu views, without showing a checked anchor on
        // an inclined camera or silently replacing the requested projection.
        if (std::abs(view_->Camera()->Direction().Y()) > 1.e-10)
            orbitLockAction_->setChecked(false);
        else
            levelPressCamera();
    }
    QWidget *controls_ = nullptr;
    DisplayStyle displayStyle_ = DisplayStyle::Edges;
    Handle(AIS_ViewCube) navigationCube_;
    double cubeRatio_ = 0;
    bool animationRunning_ = false;
    QLabel *dimensionsNotice_ = nullptr;
    DimensionAnnotations dimensions_;
    std::shared_ptr<const application::ModelMeasurements> measurements_;
    MeasurementSelection measureSelection_;
    TopTools_IndexedMapOfShape edges_;
    application::DocumentPtr document_;
    application::BendPreviewPtr preview_;

  public:
    void setAnimationRunning(bool running) {
        if (animationRunning_ == running)
            return;
        animationRunning_ = running;
        refreshDimensions();
    }
    void highlightPreviewBend(std::optional<std::size_t> i) {
        if (hoverPreviewBend_ == i)
            return;
        hoverPreviewBend_ = i;
        if (preview_)
            presentPreview(preview_, previewFrame_, revision_);
    }

  private:
    cad::SheetOrientationVisual sourceOrientation_;
    QAction *sideAction_ = nullptr;
    QWidget *sideLegend_ = nullptr;
    QLabel *sideLegendText_ = nullptr;
    QToolButton *nextEdge_ = nullptr;
    std::size_t edgeIndex_ = 0;
    std::vector<Handle(AIS_InteractiveObject)> sideMarkers_;
    const cad::SheetOrientationVisual &sceneOrientation() const {
        if (preview_)
            return cad::OcctPreviewAccess::frame(*preview_, previewFrame_).orientation;
        if (flat_)
            return cad::OcctFlatPatternAccess::orientation(*flat_);
        return sourceOrientation_;
    }
    void positionSideLegend() {
        if (!sideLegend_)
            return;
        sideLegend_->adjustSize();
        const int bottom =
            bottomOverlay_ && bottomOverlay_->isVisible() ? bottomOverlay_->y() : height();
        sideLegend_->move(12, std::max(12, bottom - sideLegend_->height() - 12));
        sideLegend_->raise();
    }
    void refreshOrientation() {
        const bool available = document_ && !sourceOrientation_.sides[0].empty() &&
                               !sourceOrientation_.sides[1].empty();
        sideAction_->setEnabled(available || (document_ && sideAction_->isChecked()));
        const bool removedMarkers = !sideMarkers_.empty();
        if (ready_)
            for (const auto &object : sideMarkers_)
                context_->Remove(object, false);
        sideMarkers_.clear();
        const auto &orientation = sceneOrientation();
        const bool visible = document_ && sideAction_->isChecked();
        sideLegend_->setVisible(visible);
        if (!visible) {
            if (ready_ && removedMarkers)
                view_->Redraw();
            return;
        }
        const bool mapped = !orientation.sides[0].empty() && !orientation.sides[1].empty();
        auto legend = QStringLiteral("<span style='color:#668AA6'>●</span> A &nbsp; <span "
                                     "style='color:#AE756E'>●</span> B");
        if (!orientation.edges.empty())
            legend += QStringLiteral(" &nbsp; · &nbsp; край <b>1</b>");
        sideLegendText_->setText(mapped ? legend
                                        : QStringLiteral("Стороны в этом состоянии недоступны"));
        nextEdge_->setVisible(mapped && !orientation.edges.empty());
        nextEdge_->setEnabled(orientation.edges.size() > 1);
        if (ready_ && mapped && !orientation.edges.empty()) {
            try {
                sideMarkers_ =
                    sheetEdgeMarker(orientation.edges[edgeIndex_ % orientation.edges.size()],
                                    14 * devicePixelRatioF());
                for (const auto &object : sideMarkers_)
                    context_->Display(object, 0, -1, false);
            } catch (const Standard_Failure &) {
                sideMarkers_.clear();
            }
        }
        positionSideLegend();
        if (ready_)
            view_->Redraw();
    }
    std::optional<std::size_t> hoverPreviewBend_;
    ToolSegmentColors segmentColors_;
    application::FlatPatternPtr flat_;
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
    Handle(Graphic3d_Camera) restoreCamera_;
    quint64 restoreRevision_ = 0;
    SceneSpace restoreSpace_ = SceneSpace::Cad;

    SceneSpace sceneSpace() const {
        return flat_      ? SceneSpace::Flat
               : preview_ ? (preview_->toolSetup() ? SceneSpace::Press : SceneSpace::Kinematic)
                          : SceneSpace::Cad;
    }

    void positionControls() {
        controls_->adjustSize();
        controls_->move(qMax(12, width() - controls_->width() - 12), 12);
        controls_->raise();
        controls_->update();
        positionOrbitLock();
        orbitLockButton_->raise();
        orbitLockButton_->update();
        positionSideLegend();
    }
};

} // namespace

class ViewerWidget::Impl {
  public:
    bool overlayLayoutQueued = false;
    QPointer<QWidget> bottomOverlay;
    QPointer<QWidget> topLeftOverlay;
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
    impl_->canvas->onBendSelection = [this](quint64 revision, int index) {
        emit bendSelectionChanged(revision, index);
    };
    impl_->canvas->onSurfaceChanged = [this] { queueOverlayLayout(); };
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
    auto result = impl_->canvas ? impl_->canvas->captureImage(includeControls) : QImage{};
    if (!result.isNull() && includeControls)
        for (const auto &overlay : {impl_->bottomOverlay, impl_->topLeftOverlay})
            if (overlay && overlay->isVisible()) {
                QPainter painter(&result);
                painter.drawPixmap(overlay->mapTo(impl_->canvas, QPoint()), overlay->grab());
            }
    return result;
}
void ViewerWidget::setBottomOverlay(QWidget *widget) {
    if (impl_->canvas)
        impl_->canvas->setBottomOverlay(widget);
    if (impl_->bottomOverlay && impl_->bottomOverlay != widget) {
        impl_->bottomOverlay->removeEventFilter(this);
        impl_->bottomOverlay->hide();
    }
    impl_->bottomOverlay = widget;
    if (!widget)
        return;
    auto *host = impl_->canvas ? static_cast<QWidget *>(impl_->canvas) : this;
    widget->setParent(host);
    if (impl_->canvas)
        widget->setAttribute(Qt::WA_NativeWindow);
    host->installEventFilter(this);
    widget->installEventFilter(this);
    widget->hide();
    queueOverlayLayout();
}
void ViewerWidget::setTopLeftOverlay(QWidget *widget) {
    if (impl_->canvas)
        impl_->canvas->setTopLeftOverlay(widget);
    if (impl_->topLeftOverlay && impl_->topLeftOverlay != widget) {
        impl_->topLeftOverlay->removeEventFilter(this);
        impl_->topLeftOverlay->hide();
    }
    impl_->topLeftOverlay = widget;
    if (!widget)
        return;
    auto *host = impl_->canvas ? static_cast<QWidget *>(impl_->canvas) : this;
    widget->setParent(host);
    if (impl_->canvas)
        widget->setAttribute(Qt::WA_NativeWindow);
    host->installEventFilter(this);
    widget->installEventFilter(this);
    widget->hide();
    queueOverlayLayout();
}
void ViewerWidget::queueOverlayLayout() {
    if (impl_->overlayLayoutQueued)
        return;
    impl_->overlayLayoutQueued = true;
    // LayoutRequest/Show filters run before Qt has finished arranging child widgets.
    QTimer::singleShot(0, this, [this] {
        impl_->overlayLayoutQueued = false;
        layoutOverlays();
    });
}
void ViewerWidget::layoutOverlays() {
    auto *widget = impl_->bottomOverlay.data();
    if (widget && widget->parentWidget()) {
        widget->ensurePolished();
        if (widget->layout())
            widget->layout()->activate();
        const auto size = widget->parentWidget()->size();
        const int availableWidth = std::max(1, size.width() - 24);
        const bool expands = widget->sizePolicy().expandingDirections().testFlag(Qt::Horizontal);
        const int width = expands
                              ? availableWidth
                              : std::max(1, std::min(availableWidth, widget->sizeHint().width()));
        const int height = std::max(1, std::min(size.height() - 24, widget->sizeHint().height()));
        const QRect target((size.width() - width) / 2, size.height() - height - 12, width, height);
        if (widget->geometry() != target)
            widget->setGeometry(target);
        widget->raise();
        widget->update();
    }
    widget = impl_->topLeftOverlay.data();
    if (widget && widget->parentWidget()) {
        widget->ensurePolished();
        const auto size = widget->parentWidget()->size();
        // Keep the view cube and the transport clear, including in a narrow window.
        const int width = std::max(1, std::min(size.width() - 140, widget->sizeHint().width()));
        const int bottom = impl_->bottomOverlay && impl_->bottomOverlay->isVisible()
                               ? impl_->bottomOverlay->y()
                               : size.height();
        const int height =
            std::max(1, std::min({bottom - 24, size.height() / 2, widget->sizeHint().height()}));
        const QRect target(12, 12, width, height);
        if (widget->geometry() != target)
            widget->setGeometry(target);
        widget->raise();
        widget->update();
    }
    if (impl_->canvas)
        impl_->canvas->layoutOrientationLegend();
}
bool ViewerWidget::eventFilter(QObject *watched, QEvent *event) {
    const auto matches = [watched](const auto &widget) {
        return widget && (watched == widget || watched == widget->parentWidget());
    };
    if ((matches(impl_->bottomOverlay) || matches(impl_->topLeftOverlay)) &&
        (event->type() == QEvent::Resize || event->type() == QEvent::Show ||
         event->type() == QEvent::LayoutRequest || event->type() == QEvent::WinIdChange ||
         event->type() == QEvent::WindowActivate))
        queueOverlayLayout();
    return QWidget::eventFilter(watched, event);
}

QMenu *ViewerWidget::navigationMenu() const {
    return impl_->canvas ? impl_->canvas->navigationMenu() : nullptr;
}
QAction *ViewerWidget::sheetOrientationAction() const {
    return impl_->canvas ? impl_->canvas->sheetOrientationAction() : nullptr;
}

void ViewerWidget::fitAll() {
    if (impl_->canvas) {
        impl_->canvas->fitAll();
    }
}
void ViewerWidget::refreshScene() {
    if (impl_->canvas)
        impl_->canvas->refreshScene();
    queueOverlayLayout();
}
void ViewerWidget::setToolSegmentColors(ToolSegmentColors colors) {
    if (impl_->canvas)
        impl_->canvas->setToolSegmentColors(std::move(colors));
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
void ViewerWidget::setAnimationRunning(bool running) {
    if (impl_->canvas)
        impl_->canvas->setAnimationRunning(running);
}
void ViewerWidget::highlightPreviewBend(std::optional<std::size_t> i) {
    if (impl_->canvas)
        impl_->canvas->highlightPreviewBend(i);
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

namespace bending::viewer {
QString ViewerWidget::presentFlatPattern(std::shared_ptr<const application::FlatPattern> flat,
                                         quint64 revision) {
    return impl_->canvas ? impl_->canvas->presentFlat(std::move(flat), revision) : impl_->error;
}
} // namespace bending::viewer
