#pragma once
#include <QObject>
#include <QUrl>
#include <memory>

namespace bending::update {
enum class UpdateState {
    Idle,
    Checking,
    Current,
    NoReleases,
    Available,
    Downloading,
    Downloaded,
    Preparing,
    Ready,
    Error
};
struct ReleaseInfo {
    QString version, notes;
    QUrl page, package, checksums;
    QString packageName, digest;
    qint64 size = 0;
};
struct UpdateOptions {
    QString installedVersion;
    // Dependency injection for local tests; production always uses the fixed GitHub origin.
    QUrl testOrigin;
    int timeoutMs = 30000;
};
class UpdateClient final : public QObject {
    Q_OBJECT
  public:
    explicit UpdateClient(UpdateOptions options, QObject *parent = nullptr);
    ~UpdateClient() override;
    UpdateState state() const;
    QString message() const;
    QString installedVersion() const;
    ReleaseInfo release() const;
    qint64 received() const;
    qint64 total() const;
    bool busy() const;
    QString verifiedZip() const;
    QString portableRoot() const;
    QString installUnavailableReason() const;
    void check(bool includePrereleases);
    void download();
    void cancel();
    void prepare();
    void saveZipTo(const QString &path);
    QString startInstaller(const QString &reopenPath);
  signals:
    void changed();

  private:
    class Impl;
    std::unique_ptr<Impl> impl_;
};
// Portable resources/settings live outside immutable version folders.
QString discoverPortableRoot(const QString &applicationDirectory, const QString &version);
bool acknowledgeUpdate(const QString &root, const QString &version, const QString &token);
} // namespace bending::update
