#include "UpdateDialog.h"
#include "bending/application/ReleaseVersion.h"
#include "bending/update/UpdateClient.h"
#include <QCheckBox>
#include <QDesktopServices>
#include <QFileDialog>
#include <QHBoxLayout>
#include <QLabel>
#include <QProgressBar>
#include <QPushButton>
#include <QTextBrowser>
#include <QTimer>
#include <QVBoxLayout>

namespace bending::ui {
using update::UpdateState;
class UpdateDialog::Impl {
  public:
    update::UpdateClient &client;
    std::function<void()> apply;
    QCheckBox *previews;
    QLabel *status, *detail;
    QTextBrowser *notes;
    QProgressBar *progress;
    QPushButton *primary, *save, *page, *close;
    enum class Intent { None, Install, Save };
    Intent intent = Intent::None;
    QString savePath, exitMessage, shownNotes;
    bool filterChanged = false, waiting = false;
    Impl(update::UpdateClient &c, std::function<void()> a) : client(c), apply(std::move(a)) {}
    bool downloadable() const {
        return !client.release().package.isEmpty() && !client.release().checksums.isEmpty();
    }
    void refresh() {
        const auto state = client.state();
        const bool busy = client.busy() || waiting;
        status->setText(exitMessage.isEmpty() ? client.message() : exitMessage);
        if (state == UpdateState::Error)
            intent = Intent::None;
        if (shownNotes != client.release().notes) {
            shownNotes = client.release().notes;
            notes->setPlainText(shownNotes);
        }
        notes->setVisible(!client.release().notes.isEmpty() && !filterChanged);
        const bool found = !client.release().version.isEmpty() && !filterChanged;
        detail->setText(found ? client.installUnavailableReason()
                              : QStringLiteral("Проверка запускается только по вашей команде."));
        if (found && downloadable() && client.installUnavailableReason().isEmpty())
            detail->setText(QStringLiteral(
                "Пакет будет проверен до перезапуска. Предыдущая версия сохранится для отката."));
        progress->setVisible(busy);
        if (state == UpdateState::Downloading && client.total() > 0 && client.received() > 0) {
            progress->setRange(0, 1000);
            progress->setValue(static_cast<int>(client.received() * 1000 / client.total()));
            progress->setFormat(QStringLiteral("%1 / %2 МБ")
                                    .arg(client.received() / 1048576., 0, 'f', 1)
                                    .arg(client.total() / 1048576., 0, 'f', 1));
        } else {
            progress->setRange(0, 0);
            progress->setFormat({});
        }
        previews->setEnabled(!busy);
        primary->setEnabled(!busy);
        page->setEnabled(!busy);
#ifdef _WIN32
        page->setVisible(found && downloadable());
        save->setVisible(found && downloadable() && client.installUnavailableReason().isEmpty());
        save->setEnabled(!busy);
#else
        page->hide();
        save->hide();
#endif
        close->setText(busy ? QStringLiteral("Отменить") : QStringLiteral("Закрыть"));
        if (busy)
            primary->setText(waiting ? QStringLiteral("Готовим перезапуск…")
                                     : QStringLiteral("Подождите…"));
        else if (!found)
            primary->setText(QStringLiteral("Проверить ещё раз"));
#ifdef _WIN32
        else if (downloadable())
            primary->setText(client.installUnavailableReason().isEmpty()
                                 ? QStringLiteral("Обновить и перезапустить")
                                 : QStringLiteral("Скачать ZIP…"));
#endif
        else
            primary->setText(QStringLiteral("Открыть выпуск"));
        if (state == UpdateState::Downloaded && intent == Intent::Install)
            client.prepare();
        else if (state == UpdateState::Downloaded && intent == Intent::Save) {
            intent = Intent::None;
            client.saveZipTo(savePath);
        } else if (state == UpdateState::Ready && intent == Intent::Install) {
            intent = Intent::None;
            apply();
        }
    }
};
UpdateDialog::UpdateDialog(update::UpdateClient &client, std::function<void()> apply,
                           QWidget *parent)
    : QDialog(parent), impl_(std::make_unique<Impl>(client, std::move(apply))) {
    setObjectName(QStringLiteral("updateDialog"));
    setWindowTitle(QStringLiteral("Обновление программы"));
    setAttribute(Qt::WA_DeleteOnClose);
    setWindowModality(Qt::NonModal);
    resize(560, 410);
    auto *layout = new QVBoxLayout(this);
    layout->setContentsMargins(22, 20, 22, 18);
    layout->setSpacing(12);
    impl_->status = new QLabel(this);
    impl_->status->setObjectName(QStringLiteral("updateStatus"));
    impl_->status->setTextFormat(Qt::PlainText);
    impl_->status->setWordWrap(true);
    auto font = impl_->status->font();
    font.setPointSize(font.pointSize() + 3);
    font.setBold(true);
    impl_->status->setFont(font);
    layout->addWidget(impl_->status);
    layout->addWidget(
        new QLabel(QStringLiteral("Установлена версия %1").arg(client.installedVersion()), this));
    impl_->detail = new QLabel(this);
    impl_->detail->setWordWrap(true);
    impl_->detail->setTextFormat(Qt::PlainText);
    layout->addWidget(impl_->detail);
    impl_->progress = new QProgressBar(this);
    impl_->progress->setAccessibleName(QStringLiteral("Загрузка обновления"));
    layout->addWidget(impl_->progress);
    impl_->notes = new QTextBrowser(this);
    impl_->notes->setObjectName(QStringLiteral("updateNotes"));
    impl_->notes->setAccessibleName(QStringLiteral("Изменения в новой версии"));
    impl_->notes->setOpenExternalLinks(false);
    layout->addWidget(impl_->notes, 1);
    impl_->previews =
        new QCheckBox(QStringLiteral("Включать предварительные версии (alpha, beta, rc)"), this);
    impl_->previews->setObjectName(QStringLiteral("includePrereleases"));
    const auto version = application::parseReleaseVersion(client.installedVersion().toStdString());
    impl_->previews->setChecked(version && version->stage != application::ReleaseStage::Stable);
    layout->addWidget(impl_->previews);
    auto *links = new QHBoxLayout;
    impl_->page = new QPushButton(QStringLiteral("Страница выпуска"), this);
    impl_->save = new QPushButton(QStringLiteral("Скачать ZIP отдельно…"), this);
    links->addWidget(impl_->page);
    links->addWidget(impl_->save);
    links->addStretch();
    layout->addLayout(links);
    auto *actions = new QHBoxLayout;
    actions->addStretch();
    impl_->close = new QPushButton(QStringLiteral("Закрыть"), this);
    impl_->primary = new QPushButton(QStringLiteral("Проверить"), this);
    impl_->primary->setObjectName(QStringLiteral("updatePrimary"));
    impl_->primary->setDefault(true);
    impl_->primary->setMinimumHeight(36);
    actions->addWidget(impl_->close);
    actions->addWidget(impl_->primary);
    layout->addLayout(actions);
    connect(&client, &update::UpdateClient::changed, this, [this] { impl_->refresh(); });
    connect(impl_->previews, &QCheckBox::toggled, this, [this] {
        impl_->filterChanged = true;
        impl_->refresh();
    });
    const auto saveZip = [this] {
        const auto path = QFileDialog::getSaveFileName(
            this, QStringLiteral("Сохранить portable ZIP"), impl_->client.release().packageName,
            QStringLiteral("ZIP (*.zip)"));
        if (path.isEmpty())
            return;
        impl_->savePath = path;
        impl_->intent = Impl::Intent::Save;
        impl_->exitMessage.clear();
        if (!impl_->client.verifiedZip().isEmpty()) {
            impl_->intent = Impl::Intent::None;
            impl_->client.saveZipTo(path);
        } else
            impl_->client.download();
    };
    connect(impl_->save, &QPushButton::clicked, this, saveZip);
    connect(impl_->page, &QPushButton::clicked, this,
            [this] { QDesktopServices::openUrl(impl_->client.release().page); });
    connect(impl_->primary, &QPushButton::clicked, this, [this, saveZip] {
        impl_->exitMessage.clear();
        if (impl_->filterChanged || impl_->client.release().version.isEmpty()) {
            impl_->filterChanged = false;
            impl_->client.check(impl_->previews->isChecked());
            return;
        }
#ifdef _WIN32
        if (impl_->downloadable()) {
            if (!impl_->client.installUnavailableReason().isEmpty()) {
                saveZip();
                return;
            }
            impl_->intent = Impl::Intent::Install;
            if (impl_->client.state() == UpdateState::Ready) {
                impl_->intent = Impl::Intent::None;
                impl_->apply();
            } else if (!impl_->client.verifiedZip().isEmpty())
                impl_->client.prepare();
            else
                impl_->client.download();
            return;
        }
#else
        (void)saveZip;
#endif
        QDesktopServices::openUrl(impl_->client.release().page);
    });
    connect(impl_->close, &QPushButton::clicked, this, [this] {
        impl_->intent = Impl::Intent::None;
        if (impl_->waiting) {
            reject();
            return;
        }
        if (impl_->client.busy())
            impl_->client.cancel();
        else
            reject();
    });
    impl_->refresh();
    QTimer::singleShot(0, this, [this] {
        if (!impl_->client.busy())
            impl_->client.check(impl_->previews->isChecked());
    });
}
void UpdateDialog::exitStatus(const QString &text, bool waiting) {
    impl_->exitMessage = text;
    impl_->waiting = waiting;
    impl_->refresh();
}
void UpdateDialog::done(int result) {
    impl_->intent = Impl::Intent::None;
    if (impl_->client.busy())
        impl_->client.cancel();
    QDialog::done(result);
}
UpdateDialog::~UpdateDialog() = default;
} // namespace bending::ui
