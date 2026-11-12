#pragma once

#include <QFileDialog>
#include <QFileInfo>
#include <QMessageBox>
#include <functional>
#include <utility>

namespace bending::ui {
inline void closeExportFileDialogs(QWidget *owner, const QString &name) {
    for (auto *dialog : owner->findChildren<QDialog *>(name, Qt::FindDirectChildrenOnly))
        if (dialog->isVisible())
            dialog->reject();
}

inline void chooseExportFile(QWidget *owner, const QString &name, const QString &title,
                             const QString &suggestedPath, const QString &filter,
                             const QString &suffix, std::function<void(const QString &)> save) {
    const auto dialogs = owner->findChildren<QDialog *>(name, Qt::FindDirectChildrenOnly);
    for (auto *dialog : dialogs) {
        if (dialog->isVisible()) {
            dialog->raise();
            dialog->activateWindow();
            return;
        }
    }

    const QFileInfo suggested(suggestedPath);
    auto *picker = new QFileDialog(owner);
    picker->setObjectName(name);
    picker->setAttribute(Qt::WA_DeleteOnClose);
    picker->setWindowModality(Qt::WindowModal);
    picker->setWindowTitle(title);
    picker->setDirectory(suggested.absolutePath());
    picker->setNameFilter(filter);
    picker->setAcceptMode(QFileDialog::AcceptSave);
    picker->setFileMode(QFileDialog::AnyFile);
    picker->setDefaultSuffix(suffix);
    picker->selectFile(suggested.fileName());
    QObject::connect(
        picker, &QFileDialog::fileSelected, owner,
        [owner, name, suffix, save = std::move(save)](const QString &path) {
            if (path.isEmpty())
                return;
            const auto extension = QLatin1Char('.') + suffix;
            const auto destination =
                path.endsWith(extension, Qt::CaseInsensitive) ? path : path + extension;
            // The file picker confirms the selected path. Only an appended
            // extension can change the destination after that confirmation.
            if (destination == path || !QFileInfo::exists(destination)) {
                save(destination);
                return;
            }
            auto *confirm =
                new QMessageBox(QMessageBox::Question, QStringLiteral("Заменить файл?"),
                                QStringLiteral("Файл «%1» уже существует. Заменить его?")
                                    .arg(QFileInfo(destination).fileName()),
                                QMessageBox::Yes | QMessageBox::No, owner);
            confirm->setObjectName(name);
            confirm->setTextFormat(Qt::PlainText);
            confirm->setDefaultButton(QMessageBox::No);
            confirm->setAttribute(Qt::WA_DeleteOnClose);
            QObject::connect(confirm, &QDialog::finished, owner, [save, destination](int result) {
                if (result == QMessageBox::Yes)
                    save(destination);
            });
            confirm->open();
        });
    picker->open();
}
} // namespace bending::ui
