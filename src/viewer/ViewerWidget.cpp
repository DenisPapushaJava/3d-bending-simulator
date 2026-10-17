#include "bending/viewer/ViewerWidget.h"

#include <AIS_InteractiveContext.hxx>
#include <AIS_Shape.hxx>
#include <Aspect_DisplayConnection.hxx>
#include <Aspect_NeutralWindow.hxx>
#include <BRepPrimAPI_MakeBox.hxx>
#include <Image_PixMap.hxx>
#include <OpenGl_GraphicDriver.hxx>
#include <Prs3d_Drawer.hxx>
#include <Standard_Failure.hxx>
#include <TopExp.hxx>
#include <TopTools_IndexedMapOfShape.hxx>
#include <V3d_AmbientLight.hxx>
#include <V3d_DirectionalLight.hxx>
#include <V3d_View.hxx>
#include <V3d_Viewer.hxx>

#include <QCoreApplication>
#include <QDir>
#include <QGuiApplication>
#include <QKeyEvent>
#include <QLabel>
#include <QMouseEvent>
#include <QResizeEvent>
#include <QStackedLayout>
#include <QTimer>
#include <QWheelEvent>

#include <cmath>
#include <functional>
#include <stdexcept>
#include <utility>

namespace bending::viewer {
namespace {

// This primitive belongs to the rendering spike. It is not an imported/project document.
class OcctCanvas final : public QWidget {
  public:
    explicit OcctCanvas(QWidget *parent) : QWidget(parent) {
        setObjectName(QStringLiteral("cadCanvas"));
        setAttribute(Qt::WA_NativeWindow);
        setAttribute(Qt::WA_PaintOnScreen);
        setAttribute(Qt::WA_NoSystemBackground);
        setMouseTracking(true);
        setFocusPolicy(Qt::StrongFocus);
    }

    ~OcctCanvas() override {
        release();
    }

    std::function<void(const QString &)> onError;
    std::function<void()> onReady;
    std::function<void(const QString &)> onSelection;

    bool ready() const {
        return ready_;
    }

    void fitAll() {
        guarded([&] {
            view_->FitAll(0.1, false);
            view_->ZFitAll();
            view_->Redraw();
        });
    }

    void setOrientation(ViewerWidget::Orientation orientation) {
        guarded([&] {
            const auto projection = orientation == ViewerWidget::Orientation::Top ? V3d_Zpos
                                    : orientation == ViewerWidget::Orientation::Front
                                        ? V3d_Yneg
                                        : V3d_XposYnegZpos;
            view_->SetProj(projection);
            fitAll();
        });
    }

    QImage captureImage() {
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
                context_->SelectDetected(AIS_SelectionScheme_Replace);
                context_->InitSelected();
                QString description;
                if (context_->MoreSelected() && context_->HasSelectedShape()) {
                    const int face = faces_.FindIndex(context_->SelectedShape());
                    description = QStringLiteral("Тестовый примитив · грань %1").arg(face);
                }
                if (onSelection) {
                    onSelection(description);
                }
                view_->Redraw();
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
    QSize physicalSize() const {
        return {qMax(1, qRound(width() * devicePixelRatioF())),
                qMax(1, qRound(height() * devicePixelRatioF()))};
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
            // Packaged resources take precedence over system discovery; external overrides survive.
            const auto shaders =
                QDir(QCoreApplication::applicationDirPath())
                    .filePath(QStringLiteral("../share/opencascade/resources/Shaders"));
            if (!qEnvironmentVariableIsSet("CSF_ShadersDirectory") && QDir(shaders).exists()) {
                qputenv("CSF_ShadersDirectory", QDir(shaders).absolutePath().toUtf8());
            }
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

            const auto shape = BRepPrimAPI_MakeBox(100.0, 60.0, 20.0).Shape();
            TopExp::MapShapes(shape, TopAbs_FACE, faces_);
            Handle(AIS_Shape) presentation = new AIS_Shape(shape);
            presentation->Attributes()->SetFaceBoundaryDraw(true);
            context_->Display(presentation, AIS_Shaded, AIS_Shape::SelectionMode(TopAbs_FACE),
                              false);
            context_->SetColor(presentation, Quantity_Color(0.16, 0.62, 0.66, Quantity_TOC_RGB),
                               false);
            ready_ = true;
            setOrientation(ViewerWidget::Orientation::Isometric);
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
    impl_->canvas->onSelection = [this](const QString &text) { emit selectionChanged(text); };
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

QImage ViewerWidget::captureImage() {
    return impl_->canvas ? impl_->canvas->captureImage() : QImage{};
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

} // namespace bending::viewer
