#include "bending/ui/FemDialog.h"

#include <QCheckBox>
#include <QCloseEvent>
#include <QComboBox>
#include <QDesktopServices>
#include <QDir>
#include <QFormLayout>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QLabel>
#include <QPlainTextEdit>
#include <QPointer>
#include <QProgressBar>
#include <QPushButton>
#include <QScrollArea>
#include <QTextCursor>
#include <QUrl>
#include <QVBoxLayout>
#include <cstdint>

namespace bending::ui {
namespace {
QString pathText(const std::filesystem::path &path) {
#ifdef _WIN32
    return QString::fromStdWString(path.native());
#else
    return QString::fromStdString(path.native());
#endif
}
} // namespace
class FemDialog::Impl {
  public:
    Impl(FemDialog *dialog, std::shared_ptr<application::FemRunner> service)
        : window(dialog), runner(std::move(service)) {}
    application::FemParameters parameters() const {
        return {static_cast<application::FemHole>(hole->currentIndex()),
                static_cast<application::FemMesh>(mesh->currentIndex()), unload->isChecked()};
    }
    void invalidate() {
        if (running) {
            return;
        }
        ++generation;
        result.reset();
        open->setEnabled(false);
        folder->setEnabled(false);
        resultText->setText(QStringLiteral("Параметры изменены. Запустите новый расчёт."));
    }
    void busy(bool value) {
        running = value;
        inputs->setEnabled(!value);
        start->setEnabled(!value && runner != nullptr);
        cancel->setEnabled(value && !cancelling);
        progress->setVisible(value);
    }
    void run() {
        if (running || !runner) {
            return;
        }
        invalidate();
        cancelling = false;
        const auto token = ++generation;
        const auto request = application::FemRequest{parameters(), context.toStdString()};
        submittedContext = context;
        jobContext->setText(QStringLiteral("Контекст задания: %1").arg(submittedContext));
        busy(true);
        status->setText(QStringLiteral("Запуск FEM: учебная пластина. Ожидайте несколько минут…"));
        resultText->clear();
        log->clear();
        QPointer<FemDialog> guard(window);
        runner->start(
            request,
            [guard, token](std::string message) {
                if (guard && guard->impl_->generation == token && guard->impl_->running) {
                    auto &state = *guard->impl_;
                    state.log->moveCursor(QTextCursor::End);
                    state.log->insertPlainText(QString::fromStdString(message));
                }
            },
            [guard, token](application::FemOutcome outcome) {
                if (!guard || guard->impl_->generation != token || !guard->impl_->running) {
                    return;
                }
                auto &state = *guard->impl_;
                const bool cancelled = state.cancelling;
                state.busy(false);
                if (cancelled || outcome.status == application::FemOutcomeStatus::Cancelled) {
                    state.status->setText(
                        (outcome.status == application::FemOutcomeStatus::Failed ||
                         outcome.status == application::FemOutcomeStatus::StopFailed)
                            ? QString::fromStdString(outcome.detail)
                            : QStringLiteral(
                                  "Расчёт отменён. Частичный результат не применяется."));
                } else if (outcome.status == application::FemOutcomeStatus::Completed &&
                           outcome.result) {
                    state.result = std::move(outcome.result);
                    state.status->setText(
                        QStringLiteral("Учебный расчёт завершён. Численные проверки пройдены."));
                    auto summary =
                        QStringLiteral("Сила в конце гибки: %1 Н\nУгол под нагрузкой: %2°")
                            .arg(state.result->formedForceN, 0, 'f', 2)
                            .arg(state.result->formedAngleDeg, 0, 'f', 2);
                    if (state.result->unloadedAngleDeg) {
                        summary += QStringLiteral("\nУгол после разгрузки: %1°")
                                       .arg(*state.result->unloadedAngleDeg, 0, 'f', 2);
                    }
                    state.resultText->setText(summary +
                                              QStringLiteral("\nПлоская пластина = 0°.\n") +
                                              pathText(state.result->directory));
                    state.open->setEnabled(true);
                    state.folder->setEnabled(true);
                } else {
                    state.status->setText(QStringLiteral("Расчёт не завершён:\n") +
                                          QString::fromStdString(outcome.detail));
                }
                if (outcome.status == application::FemOutcomeStatus::StopFailed) {
                    state.start->setEnabled(false);
                    state.closeWhenFinished = false;
                    state.status->setText(
                        QString::fromStdString(outcome.detail) +
                        QStringLiteral("\nОстановка не подтверждена. После ручной очистки "
                                       "перезапустите приложение."));
                }
                state.updateContext();
                if (state.closeWhenFinished) {
                    guard->close();
                }
            });
    }
    void stop() {
        if (!running || cancelling) {
            return;
        }
        cancelling = true;
        cancel->setEnabled(false);
        status->setText(QStringLiteral("Остановка FEM-контейнера…"));
        runner->cancel();
    }
    void updateContext() {
        contextLabel->setText(QStringLiteral("Текущий контекст: %1").arg(context));
        if (!submittedContext.isEmpty()) {
            jobContext->setText(QStringLiteral("Контекст задания: %1%2")
                                    .arg(submittedContext,
                                         context != submittedContext
                                             ? QStringLiteral("\nCAD-контекст изменился. Результат "
                                                              "остаётся отдельным учебным опытом.")
                                             : QString{}));
        }
    }
    FemDialog *window;
    std::shared_ptr<application::FemRunner> runner;
    QGroupBox *inputs = nullptr;
    QComboBox *hole = nullptr, *mesh = nullptr;
    QCheckBox *unload = nullptr;
    QLabel *contextLabel = nullptr, *jobContext = nullptr, *status = nullptr, *resultText = nullptr;
    QPushButton *start = nullptr, *cancel = nullptr, *open = nullptr, *folder = nullptr;
    QProgressBar *progress = nullptr;
    QPlainTextEdit *log = nullptr;
    QString context = QStringLiteral("Без загруженной детали"), submittedContext;
    std::optional<application::FemResult> result;
    std::uint64_t generation = 0;
    bool running = false, cancelling = false, closeWhenFinished = false;
};
FemDialog::FemDialog(std::shared_ptr<application::FemRunner> runner, QWidget *parent)
    : QDialog(parent), impl_(std::make_unique<Impl>(this, std::move(runner))) {
    setObjectName(QStringLiteral("femDialog"));
    setWindowTitle(QStringLiteral("FEM · отверстие в зоне гиба"));
    resize(780, 820);
    auto *layout = new QVBoxLayout(this);
    auto *scroll = new QScrollArea(this);
    scroll->setWidgetResizable(true);
    auto *body = new QWidget(scroll);
    auto *content = new QVBoxLayout(body);
    const auto label = [&](QString name, QString value) {
        auto *widget = new QLabel(std::move(value), body);
        widget->setObjectName(std::move(name));
        widget->setWordWrap(true);
        widget->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
        widget->setTextFormat(Qt::PlainText);
        widget->setTextInteractionFlags(Qt::TextSelectableByMouse);
        content->addWidget(widget);
        return widget;
    };
    label(QStringLiteral("femScope"),
          QStringLiteral("Исследовательский расчёт учебной пластины 60 × 20 × 1 мм. "
                         "Геометрия и материал загруженного STEP сюда не переносятся. "
                         "Результат не подтверждает изготовление CAD-детали."));
    impl_->contextLabel = label(QStringLiteral("femContext"), {});
    impl_->inputs = new QGroupBox(QStringLiteral("Учебная модель"), body);
    auto *form = new QFormLayout(impl_->inputs);
    form->setRowWrapPolicy(QFormLayout::WrapLongRows);
    impl_->hole = new QComboBox(impl_->inputs);
    impl_->hole->setObjectName(QStringLiteral("femHole"));
    impl_->hole->addItems({QStringLiteral("Ø4 мм на линии гиба"),
                           QStringLiteral("Ø4 мм, смещение 6 мм"),
                           QStringLiteral("Без отверстия — контроль")});
    form->addRow(QStringLiteral("Отверстие"), impl_->hole);
    impl_->mesh = new QComboBox(impl_->inputs);
    impl_->mesh->setObjectName(QStringLiteral("femMesh"));
    impl_->mesh->addItems({QStringLiteral("Крупная — качественный просмотр"),
                           QStringLiteral("Средняя"),
                           QStringLiteral("Мелкая — проверка чувствительности")});
    impl_->mesh->setCurrentIndex(1);
    form->addRow(QStringLiteral("Сетка"), impl_->mesh);
    impl_->unload =
        new QCheckBox(QStringLiteral("Рассчитать разгрузку и остаточную форму"), impl_->inputs);
    impl_->unload->setObjectName(QStringLiteral("femUnload"));
    impl_->unload->setChecked(true);
    form->addRow(impl_->unload);
    auto *model = new QLabel(
        QStringLiteral("Ход 3 мм · V24 / R2 · пуансон 85° / R2\n"
                       "Условный изотропный материал: E=210000 МПа, ν=0,3, "
                       "учебное упрочнение. Контакт без трения.\n"
                       "PEEQ — пластическая деформация с усреднением в узлах. "
                       "Трещины не рассчитываются; точность на образцах не установлена."),
        impl_->inputs);
    model->setWordWrap(true);
    model->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
    form->addRow(model);
    content->addWidget(impl_->inputs);
    label(QStringLiteral("femEnvironment"),
          QStringLiteral("Окружение: локальный Docker с Linux-контейнерами и образом "
                         "bending-fem:ubuntu24.04 (CalculiX 2.21 / Gmsh 4.12.1). "
                         "Автоматическая установка и загрузка образа не выполняются."));
    auto *setup = new QPushButton(QStringLiteral("Как подготовить FEM-окружение"), body);
    setup->setObjectName(QStringLiteral("femSetup"));
    content->addWidget(setup);
    auto *instructions = new QPlainTextEdit(body);
    instructions->setObjectName(QStringLiteral("femInstructions"));
    instructions->setReadOnly(true);
    instructions->setPlainText(QStringLiteral(
        "Из корня исходного репозитория:\n"
        "docker build -t bending-fem:ubuntu24.04 experiments/bend-hole\n\n"
        "Из установленного комплекта: Dockerfile и README находятся в\n"
        "share/3d-bending-simulator/fem рядом с bin. Команда из этой папки:\n"
        "docker build -t bending-fem:ubuntu24.04 .\n\n"
        "На Windows нужен запущенный Docker Desktop в режиме Linux containers. "
        "Запуск FEM на Windows пока не проверен. Приложение работает и без Docker."));
    instructions->setMaximumHeight(180);
    instructions->hide();
    content->addWidget(instructions);
    connect(setup, &QPushButton::clicked, this,
            [instructions] { instructions->setVisible(instructions->isHidden()); });
    impl_->status =
        label(QStringLiteral("femStatus"), QStringLiteral("Готово к запуску учебного расчёта."));
    impl_->jobContext = label(QStringLiteral("femJobContext"), {});
    impl_->resultText = label(QStringLiteral("femResult"), {});
    impl_->log = new QPlainTextEdit(body);
    impl_->log->setObjectName(QStringLiteral("femLog"));
    impl_->log->setReadOnly(true);
    impl_->log->setMaximumBlockCount(250);
    impl_->log->setMinimumHeight(100);
    impl_->log->setMaximumHeight(160);
    impl_->log->setAccessibleName(QStringLiteral("Журнал FEM-задания"));
    content->addWidget(impl_->log);
    content->addStretch();
    scroll->setWidget(body);
    layout->addWidget(scroll);
    impl_->progress = new QProgressBar(this);
    impl_->progress->setRange(0, 0);
    impl_->progress->setAccessibleName(QStringLiteral("Выполняется FEM-расчёт"));
    layout->addWidget(impl_->progress);
    auto *commands = new QHBoxLayout;
    impl_->start = new QPushButton(QStringLiteral("Рассчитать учебную пластину"), this);
    impl_->start->setObjectName(QStringLiteral("femStart"));
    impl_->cancel = new QPushButton(QStringLiteral("Отменить расчёт"), this);
    impl_->cancel->setObjectName(QStringLiteral("femCancel"));
    commands->addWidget(impl_->start);
    commands->addWidget(impl_->cancel);
    layout->addLayout(commands);
    auto *outputs = new QHBoxLayout;
    impl_->open = new QPushButton(QStringLiteral("Открыть 3D-анимацию в браузере"), this);
    impl_->open->setObjectName(QStringLiteral("femOpenAnimation"));
    impl_->open->setEnabled(false);
    impl_->folder = new QPushButton(QStringLiteral("Каталог расчёта"), this);
    impl_->folder->setObjectName(QStringLiteral("femOpenFolder"));
    impl_->folder->setEnabled(false);
    outputs->addWidget(impl_->open);
    outputs->addWidget(impl_->folder);
    layout->addLayout(outputs);
    connect(impl_->start, &QPushButton::clicked, this, [this] { impl_->run(); });
    connect(impl_->cancel, &QPushButton::clicked, this, [this] { impl_->stop(); });
    connect(impl_->hole, &QComboBox::currentIndexChanged, this, [this] { impl_->invalidate(); });
    connect(impl_->mesh, &QComboBox::currentIndexChanged, this, [this] { impl_->invalidate(); });
    connect(impl_->unload, &QCheckBox::toggled, this, [this] { impl_->invalidate(); });
    connect(impl_->open, &QPushButton::clicked, this, [this] {
        if (impl_->result &&
            !QDesktopServices::openUrl(QUrl::fromLocalFile(pathText(impl_->result->animation)))) {
            impl_->status->setText(QStringLiteral(
                "Не удалось открыть браузер. Откройте result.html в каталоге расчёта."));
        }
    });
    connect(impl_->folder, &QPushButton::clicked, this, [this] {
        if (impl_->result) {
            QDesktopServices::openUrl(QUrl::fromLocalFile(pathText(impl_->result->directory)));
        }
    });
    impl_->busy(false);
    impl_->updateContext();
}
FemDialog::~FemDialog() {
    if (impl_->running) {
        impl_->runner->cancel();
    }
}
void FemDialog::setCadContext(QString description) {
    impl_->context = std::move(description);
    impl_->updateContext();
}
bool FemDialog::isRunning() const {
    return impl_->running;
}
void FemDialog::closeEvent(QCloseEvent *event) {
    if (impl_->running) {
        impl_->closeWhenFinished = true;
        impl_->stop();
        event->ignore();
    } else {
        impl_->closeWhenFinished = false;
        QDialog::closeEvent(event);
    }
}
void FemDialog::reject() {
    if (impl_->running) {
        close();
    } else {
        QDialog::reject();
    }
}
} // namespace bending::ui
