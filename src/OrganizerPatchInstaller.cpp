// SPDX-License-Identifier: GPL-3.0-only

#include <array>
#include <memory>

#include <QApplication>
#include <QColor>
#include <QCommandLineOption>
#include <QCommandLineParser>
#include <QCoreApplication>
#include <QCryptographicHash>
#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QFrame>
#include <QHBoxLayout>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLabel>
#include <QLocale>
#include <QMessageBox>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QPainter>
#include <QPointer>
#include <QProcess>
#include <QProgressBar>
#include <QPushButton>
#include <QSaveFile>
#include <QStyle>
#include <QSvgRenderer>
#include <QTextStream>
#include <QThread>
#include <QUrl>
#include <QVBoxLayout>
#include <QWidget>

namespace {
constexpr auto kRepositoryApi =
    "https://api.github.com/repos/silenceremember/prism-family-organizer-patch/releases?per_page=20";

QString formatBytes(qint64 bytes)
{
    if (bytes < 0) {
        return QStringLiteral("—");
    }
    static constexpr std::array<const char*, 4> units{ "B", "KiB", "MiB", "GiB" };
    double value = static_cast<double>(bytes);
    qsizetype unit = 0;
    while (value >= 1024.0 && unit + 1 < static_cast<qsizetype>(units.size())) {
        value /= 1024.0;
        ++unit;
    }
    const auto decimals = unit == 0 ? 0 : (value < 10.0 ? 1 : 0);
    return QStringLiteral("%1 %2").arg(QLocale().toString(value, 'f', decimals), QString::fromLatin1(units.at(unit)));
}

QPixmap renderSvg(const QString& resource, const QSize& size)
{
    QSvgRenderer renderer(resource);
    if (!renderer.isValid()) {
        return QPixmap{};
    }
    QPixmap pixmap(size);
    pixmap.fill(Qt::transparent);
    QPainter painter(&pixmap);
    renderer.render(&painter, QRectF(QPointF{}, QSizeF(size)));
    return pixmap;
}

QJsonObject readObject(const QString& path)
{
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) {
        return {};
    }
    const auto document = QJsonDocument::fromJson(file.readAll());
    return document.isObject() ? document.object() : QJsonObject{};
}

QByteArray sha256File(const QString& path)
{
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) {
        return {};
    }
    QCryptographicHash hash(QCryptographicHash::Sha256);
    if (!hash.addData(&file)) {
        return {};
    }
    return hash.result().toHex();
}

void writeFailure(const QString& target, const QString& message)
{
    QFile log(QDir(QFileInfo(target).absolutePath()).filePath(QStringLiteral("organizer-patch-installer.log")));
    if (log.open(QIODevice::WriteOnly | QIODevice::Append | QIODevice::Text)) {
        QTextStream stream(&log);
        stream << QDateTime::currentDateTimeUtc().toString(Qt::ISODate) << " " << message << "\n";
    }
}

bool saveState(const QString& path, const QString& version, const QByteArray& installedHash, QString* error)
{
    QFile input(path);
    if (!input.open(QIODevice::ReadOnly)) {
        *error = QStringLiteral("Could not open patch state: %1").arg(input.errorString());
        return false;
    }
    const auto document = QJsonDocument::fromJson(input.readAll());
    input.close();
    if (!document.isObject()) {
        *error = QStringLiteral("Patch state is invalid.");
        return false;
    }
    auto state = document.object();
    state.insert(QStringLiteral("version"), version);
    state.insert(QStringLiteral("installedSha256"), QString::fromLatin1(installedHash));
    state.insert(QStringLiteral("updatedAt"), QDateTime::currentDateTimeUtc().toString(Qt::ISODate));

    QSaveFile output(path);
    if (!output.open(QIODevice::WriteOnly) || output.write(QJsonDocument(state).toJson(QJsonDocument::Indented)) < 0 ||
        !output.commit()) {
        *error = QStringLiteral("Could not update patch state: %1").arg(output.errorString());
        return false;
    }
    return true;
}

bool saveInstallState(const QString& path, const QString& target, const QString& family, const QString& version,
                      const QByteArray& installedHash, const QString& original, const QByteArray& originalHash,
                      QString* error)
{
    QJsonObject state;
    state.insert(QStringLiteral("schema"), 1);
    state.insert(QStringLiteral("version"), version);
    state.insert(QStringLiteral("family"), family);
    state.insert(QStringLiteral("executable"), QFileInfo(target).fileName());
    state.insert(QStringLiteral("original"), QFileInfo(path).absoluteDir().relativeFilePath(original));
    state.insert(QStringLiteral("originalSha256"), QString::fromLatin1(originalHash));
    state.insert(QStringLiteral("installedSha256"), QString::fromLatin1(installedHash));
    state.insert(QStringLiteral("updatedAt"), QDateTime::currentDateTimeUtc().toString(Qt::ISODate));

    QSaveFile output(path);
    if (!output.open(QIODevice::WriteOnly) || output.write(QJsonDocument(state).toJson(QJsonDocument::Indented)) < 0 ||
        !output.commit()) {
        *error = QStringLiteral("Could not create patch state: %1").arg(output.errorString());
        return false;
    }
    return true;
}

bool waitForRename(const QString& source, const QString& destination, QString* error)
{
    for (int attempt = 0; attempt < 300; ++attempt) {
        if (QFile::rename(source, destination)) {
            return true;
        }
        QThread::msleep(100);
    }
    *error = QStringLiteral("Timed out waiting for the launcher to close.");
    return false;
}

bool waitForRemove(const QString& path, QString* error)
{
    for (int attempt = 0; attempt < 300; ++attempt) {
        if (!QFileInfo::exists(path) || QFile::remove(path)) {
            return true;
        }
        QThread::msleep(100);
    }
    *error = QStringLiteral("Timed out waiting for the previous launcher process to exit: %1").arg(path);
    return false;
}

bool validStateLocation(const QString& statePath, const QString& target)
{
    const QFileInfo stateInfo(statePath);
    const QFileInfo targetInfo(target);
    const QDir stateDir = stateInfo.absoluteDir();
    return stateInfo.fileName() == QStringLiteral("state.json") && stateDir.dirName() == QStringLiteral(".organizer-patch") &&
           QFileInfo(stateDir.absolutePath()).absoluteDir().absolutePath() == targetInfo.absoluteDir().absolutePath();
}

bool replaceLauncher(const QString& target, const QString& source, const QByteArray& expectedHash, const QString& mode,
                     const QString& statePath, const QString& version, const QString& family, QString* error)
{
    if (!QFileInfo(target).isAbsolute() || !QFileInfo(source).isAbsolute() || target == source) {
        *error = QStringLiteral("Unsafe maintenance paths.");
        return false;
    }
    if (expectedHash.size() != 64 || sha256File(source) != expectedHash) {
        *error = QStringLiteral("Source executable failed SHA-256 verification.");
        return false;
    }
    if (!validStateLocation(statePath, target)) {
        *error = QStringLiteral("Unsafe patch state location.");
        return false;
    }

    QString installOriginal;
    QByteArray installOriginalHash;
    bool installSnapshotCreated = false;
    auto cleanupInstallSnapshot = [&] {
        if (!installSnapshotCreated) {
            return;
        }
        QFile::remove(statePath);
        QFile::remove(installOriginal);
        const auto originalDir = QFileInfo(installOriginal).absoluteDir();
        QDir().rmdir(originalDir.absolutePath());
        QDir().rmdir(QFileInfo(statePath).absoluteDir().absolutePath());
    };
    if (mode == QStringLiteral("install")) {
        if (version.isEmpty() || family.isEmpty()) {
            *error = QStringLiteral("Install mode requires a version and launcher family.");
            return false;
        }
        if (QFileInfo::exists(statePath)) {
            *error = QStringLiteral("Organizer Patch is already installed; use update mode.");
            return false;
        }
        const auto stateDir = QFileInfo(statePath).absoluteDir().absolutePath();
        const auto originalDir = QDir(stateDir).filePath(QStringLiteral("original"));
        installOriginal = QDir(originalDir).filePath(QFileInfo(target).fileName());
        if (QFileInfo::exists(installOriginal) || !QDir().mkpath(originalDir) || !QFile::copy(target, installOriginal)) {
            *error = QStringLiteral("Could not preserve the pristine launcher executable.");
            return false;
        }
        installOriginalHash = sha256File(installOriginal);
        if (installOriginalHash.size() != 64) {
            *error = QStringLiteral("Could not verify the pristine launcher executable.");
            QFile::remove(installOriginal);
            return false;
        }
        installSnapshotCreated = true;
    }

    const auto rollback = target + QStringLiteral(".organizer-rollback");
    if (QFileInfo::exists(rollback) && !QFile::remove(rollback)) {
        *error = QStringLiteral("Could not remove a stale rollback file.");
        return false;
    }
    if (!waitForRename(target, rollback, error)) {
        cleanupInstallSnapshot();
        return false;
    }

    auto restoreRollback = [&] {
        QFile::remove(target);
        QFile::rename(rollback, target);
    };
    if (!QFile::copy(source, target)) {
        *error = QStringLiteral("Could not install the replacement executable.");
        restoreRollback();
        cleanupInstallSnapshot();
        return false;
    }
    QFile::setPermissions(target, QFile::permissions(rollback));
    if (sha256File(target) != expectedHash) {
        *error = QStringLiteral("Installed executable failed SHA-256 verification.");
        restoreRollback();
        cleanupInstallSnapshot();
        return false;
    }

    if (mode == QStringLiteral("update")) {
        if (version.isEmpty() || !saveState(statePath, version, expectedHash, error)) {
            restoreRollback();
            return false;
        }
        QFile::remove(source);
    } else if (mode == QStringLiteral("install")) {
        if (!saveInstallState(statePath, target, family, version, expectedHash, installOriginal, installOriginalHash, error)) {
            restoreRollback();
            cleanupInstallSnapshot();
            return false;
        }
    } else {
        const auto stateDir = QFileInfo(statePath).absoluteDir().absolutePath();
        if (!QDir(stateDir).removeRecursively()) {
            writeFailure(target, QStringLiteral("Original launcher restored, but patch data could not be removed: %1").arg(stateDir));
        }
    }

    QString cleanupError;
    if (!waitForRemove(rollback, &cleanupError)) {
        writeFailure(target, QStringLiteral("Replacement succeeded, but rollback cleanup failed: %1").arg(cleanupError));
    }
    return true;
}

class OrganizerPatchManagerWindow final : public QWidget {
   public:
    OrganizerPatchManagerWindow(QString target, QString statePath, QString family)
        : m_target(std::move(target)), m_statePath(std::move(statePath)), m_family(std::move(family))
    {
        setWindowTitle(QStringLiteral("Organizer Patch Manager"));
        setWindowIcon(QIcon(renderSvg(QStringLiteral(":/organizer-manager/logo-background.svg"), QSize(48, 48))));
        setMinimumSize(560, 350);

        auto* layout = new QVBoxLayout(this);
        layout->setContentsMargins(18, 18, 18, 18);
        layout->setSpacing(12);

        auto* header = new QHBoxLayout;
        auto* logo = new QLabel(this);
        logo->setFixedSize(176, 110);
        logo->setPixmap(renderSvg(QStringLiteral(":/organizer-manager/logo.svg"), QSize(168, 104)));
        logo->setAlignment(Qt::AlignCenter);
        header->addWidget(logo, 0, Qt::AlignTop);

        auto* titleLayout = new QVBoxLayout;
        auto* title = new QLabel(QStringLiteral("Prism Family Organizer Patch"), this);
        auto titleFont = title->font();
        titleFont.setBold(true);
        titleFont.setPointSizeF(titleFont.pointSizeF() + 3.0);
        title->setFont(titleFont);
        titleLayout->addWidget(title);
        auto* subtitle = new QLabel(QStringLiteral("Update or remove the Organizer integration safely."), this);
        subtitle->setWordWrap(true);
        titleLayout->addWidget(subtitle);
        titleLayout->addStretch();
        header->addLayout(titleLayout, 1);
        layout->addLayout(header);

        auto* versionFrame = new QFrame(this);
        versionFrame->setFrameShape(QFrame::StyledPanel);
        auto* versionLayout = new QVBoxLayout(versionFrame);
        const auto state = readObject(m_statePath);
        m_installedVersion = state.value(QStringLiteral("version")).toString();
        auto* current = new QLabel(QStringLiteral("Installed: %1").arg(
                                       m_installedVersion.isEmpty() ? QStringLiteral("Unknown") : m_installedVersion),
                                   versionFrame);
        m_latestLabel = new QLabel(QStringLiteral("Latest: checking…"), versionFrame);
        versionLayout->addWidget(current);
        versionLayout->addWidget(m_latestLabel);
        layout->addWidget(versionFrame);

        m_status = new QLabel(this);
        m_status->setWordWrap(true);
        layout->addWidget(m_status);

        auto* progressLayout = new QHBoxLayout;
        m_progress = new QProgressBar(this);
        m_progress->setRange(0, 1000);
        m_progress->setTextVisible(false);
        m_progress->setVisible(false);
        progressLayout->addWidget(m_progress, 1);
        m_progressText = new QLabel(this);
        m_progressText->setMinimumWidth(170);
        m_progressText->setAlignment(Qt::AlignRight | Qt::AlignVCenter);
        m_progressText->setVisible(false);
        progressLayout->addWidget(m_progressText);
        layout->addLayout(progressLayout);

        auto* buttons = new QHBoxLayout;
        m_checkButton = new QPushButton(style()->standardIcon(QStyle::SP_BrowserReload), QStringLiteral("Check"), this);
        m_updateButton =
            new QPushButton(style()->standardIcon(QStyle::SP_ArrowDown), QStringLiteral("Update"), this);
        m_removeButton = new QPushButton(style()->standardIcon(QStyle::SP_TrashIcon), QStringLiteral("Remove"), this);
        m_closeButton = new QPushButton(style()->standardIcon(QStyle::SP_DialogCloseButton), QStringLiteral("Close"), this);
        m_updateButton->setEnabled(false);
        const auto original = QDir(QFileInfo(m_statePath).absolutePath())
                                  .filePath(state.value(QStringLiteral("original")).toString());
        m_removeButton->setEnabled(!m_installedVersion.isEmpty() && QFileInfo::exists(original));
        buttons->addWidget(m_checkButton);
        buttons->addWidget(m_updateButton);
        buttons->addWidget(m_removeButton);
        buttons->addStretch();
        buttons->addWidget(m_closeButton);
        layout->addLayout(buttons);

        connect(m_checkButton, &QPushButton::clicked, this, [this] { checkReleases(); });
        connect(m_updateButton, &QPushButton::clicked, this, [this] { beginUpdate(); });
        connect(m_removeButton, &QPushButton::clicked, this, [this] { beginRemove(); });
        connect(m_closeButton, &QPushButton::clicked, this, &QWidget::close);
        connect(qApp, &QCoreApplication::aboutToQuit, this, [this] {
            if (m_restartOnExit && QFileInfo::exists(m_target)) {
                QProcess::startDetached(m_target, {}, QFileInfo(m_target).absolutePath());
            }
        });

        checkReleases();
    }

   private:
    struct ReleaseAsset {
        QString version;
        QUrl url;
        QByteArray digest;

        bool valid() const { return !version.isEmpty() && url.isValid() && digest.size() == 64; }
    };

    void setBusy(bool busy, const QString& status)
    {
        m_busy = busy;
        m_checkButton->setEnabled(!busy);
        m_updateButton->setEnabled(!busy && m_available.valid() && m_available.version != m_installedVersion);
        const auto state = readObject(m_statePath);
        const auto original = QDir(QFileInfo(m_statePath).absolutePath())
                                  .filePath(state.value(QStringLiteral("original")).toString());
        m_removeButton->setEnabled(!busy && QFileInfo::exists(original));
        m_status->setText(status);
    }

    void resetProgress()
    {
        m_progress->setVisible(false);
        m_progressText->setVisible(false);
        m_progress->setRange(0, 1000);
        m_progress->setValue(0);
        m_progressText->clear();
    }

    void setProgress(qint64 received, qint64 total)
    {
        m_progress->setVisible(true);
        m_progressText->setVisible(true);
        if (total <= 0) {
            m_progress->setRange(0, 0);
            m_progressText->setText(QStringLiteral("%1 / unknown").arg(formatBytes(received)));
            return;
        }
        m_progress->setRange(0, 1000);
        const auto value = qBound(0, qRound(1000.0 * static_cast<double>(received) / static_cast<double>(total)), 1000);
        m_progress->setValue(value);
        m_progressText->setText(QStringLiteral("%1 / %2 (%3%)")
                                    .arg(formatBytes(received), formatBytes(total))
                                    .arg(qRound(value / 10.0)));
    }

    ReleaseAsset findFamilyAsset(const QByteArray& json, QString* error) const
    {
        QJsonParseError parseError;
        const auto document = QJsonDocument::fromJson(json, &parseError);
        if (parseError.error != QJsonParseError::NoError || !document.isArray()) {
            *error = QStringLiteral("GitHub returned invalid release data.");
            return {};
        }
        const auto expected = QStringLiteral("prism-family-organizer-patch-%1-windows-x64.exe").arg(m_family);
        for (const auto releaseValue : document.array()) {
            const auto release = releaseValue.toObject();
            if (release.value(QStringLiteral("draft")).toBool()) {
                continue;
            }
            auto version = release.value(QStringLiteral("tag_name")).toString();
            if (version.startsWith(QLatin1Char('v'), Qt::CaseInsensitive)) {
                version.remove(0, 1);
            }
            for (const auto assetValue : release.value(QStringLiteral("assets")).toArray()) {
                const auto asset = assetValue.toObject();
                if (asset.value(QStringLiteral("name")).toString() != expected) {
                    continue;
                }
                auto digest = asset.value(QStringLiteral("digest")).toString();
                if (!digest.startsWith(QStringLiteral("sha256:"), Qt::CaseInsensitive)) {
                    continue;
                }
                digest.remove(0, 7);
                ReleaseAsset result{ version, QUrl(asset.value(QStringLiteral("browser_download_url")).toString()),
                                     digest.toLatin1().toLower() };
                if (result.valid()) {
                    return result;
                }
            }
        }
        *error = QStringLiteral("No compatible Windows release was found for %1.").arg(m_family);
        return {};
    }

    void checkReleases()
    {
        if (m_busy) {
            return;
        }
        resetProgress();
        setBusy(true, QStringLiteral("Checking GitHub Releases…"));
        QNetworkRequest request(QUrl(QString::fromLatin1(kRepositoryApi)));
        request.setHeader(QNetworkRequest::UserAgentHeader, QStringLiteral("Prism-Family-Organizer-Patch"));
        request.setRawHeader("Accept", "application/vnd.github+json");
        request.setAttribute(QNetworkRequest::RedirectPolicyAttribute, QNetworkRequest::NoLessSafeRedirectPolicy);
        auto* reply = m_network.get(request);
        m_reply = reply;
        connect(reply, &QNetworkReply::finished, this, [this, reply] {
            const auto body = reply->readAll();
            const auto networkError = reply->error();
            const auto networkMessage = reply->errorString();
            reply->deleteLater();
            m_reply = nullptr;
            if (networkError != QNetworkReply::NoError) {
                setBusy(false, QStringLiteral("Check failed: %1").arg(networkMessage));
                return;
            }
            QString error;
            m_available = findFamilyAsset(body, &error);
            if (!m_available.valid()) {
                setBusy(false, error);
                return;
            }
            m_latestLabel->setText(QStringLiteral("Latest: %1").arg(m_available.version));
            if (m_available.version == m_installedVersion) {
                setBusy(false, QStringLiteral("The installed patch is up to date."));
            } else {
                setBusy(false, QStringLiteral("Version %1 is available.").arg(m_available.version));
            }
        });
    }

    void beginUpdate()
    {
        if (m_busy || !m_available.valid() || m_available.version == m_installedVersion) {
            return;
        }
        if (QMessageBox::question(this, QStringLiteral("Update Organizer Patch"),
                                  QStringLiteral("Download %1, install it, and restart the launcher?")
                                      .arg(m_available.version)) != QMessageBox::Yes) {
            return;
        }
        resetProgress();
        setProgress(0, -1);
        setBusy(true, QStringLiteral("Downloading %1…").arg(m_available.version));
        QNetworkRequest request(m_available.url);
        request.setHeader(QNetworkRequest::UserAgentHeader, QStringLiteral("Prism-Family-Organizer-Patch"));
        request.setAttribute(QNetworkRequest::RedirectPolicyAttribute, QNetworkRequest::NoLessSafeRedirectPolicy);
        auto* reply = m_network.get(request);
        m_reply = reply;
        connect(reply, &QNetworkReply::downloadProgress, this,
                [this](qint64 received, qint64 total) { setProgress(received, total); });
        connect(reply, &QNetworkReply::finished, this, [this, reply] { updateDownloaded(reply); });
    }

    void updateDownloaded(QNetworkReply* reply)
    {
        const auto payload = reply->readAll();
        const auto networkError = reply->error();
        const auto networkMessage = reply->errorString();
        reply->deleteLater();
        m_reply = nullptr;
        if (networkError != QNetworkReply::NoError) {
            resetProgress();
            setBusy(false, QStringLiteral("Download failed: %1").arg(networkMessage));
            return;
        }
        setProgress(payload.size(), payload.size());
        setBusy(true, QStringLiteral("Verifying and installing %1…").arg(m_available.version));
        if (QCryptographicHash::hash(payload, QCryptographicHash::Sha256).toHex() != m_available.digest) {
            resetProgress();
            setBusy(false, QStringLiteral("Downloaded launcher failed SHA-256 verification."));
            return;
        }
        const auto downloadDir = QDir(QFileInfo(m_statePath).absolutePath()).filePath(QStringLiteral("downloads"));
        if (!QDir().mkpath(downloadDir)) {
            setBusy(false, QStringLiteral("Could not create the update directory."));
            return;
        }
        const auto source = QDir(downloadDir).filePath(QStringLiteral("organizer-update-%1.exe").arg(m_available.version));
        QSaveFile output(source);
        if (!output.open(QIODevice::WriteOnly) || output.write(payload) != payload.size() || !output.commit()) {
            setBusy(false, QStringLiteral("Could not save the downloaded launcher."));
            return;
        }
        QString error;
        if (!replaceLauncher(m_target, source, m_available.digest, QStringLiteral("update"), m_statePath,
                             m_available.version, m_family, &error)) {
            setBusy(false, error);
            return;
        }
        restartAndQuit();
    }

    void beginRemove()
    {
        if (m_busy) {
            return;
        }
        const auto state = readObject(m_statePath);
        const auto originalRelative = state.value(QStringLiteral("original")).toString();
        const auto originalHash = state.value(QStringLiteral("originalSha256")).toString().toLatin1().toLower();
        const auto original = QDir(QFileInfo(m_statePath).absolutePath()).filePath(originalRelative);
        if (originalRelative.isEmpty() || originalHash.size() != 64 || sha256File(original) != originalHash) {
            QMessageBox::critical(this, QStringLiteral("Remove Organizer Patch"),
                                  QStringLiteral("The saved original launcher is missing or failed verification."));
            return;
        }
        if (QMessageBox::warning(this, QStringLiteral("Remove Organizer Patch"),
                                 QStringLiteral("Restore the original launcher and remove Organizer Patch?\n\n"
                                                "Instances and group configuration will be kept."),
                                 QMessageBox::Yes | QMessageBox::Cancel, QMessageBox::Cancel) != QMessageBox::Yes) {
            return;
        }
        setBusy(true, QStringLiteral("Restoring the original launcher…"));
        QString error;
        if (!replaceLauncher(m_target, original, originalHash, QStringLiteral("remove"), m_statePath, {}, m_family,
                             &error)) {
            setBusy(false, error);
            return;
        }
        restartAndQuit();
    }

    void restartAndQuit()
    {
        setBusy(true, QStringLiteral("Restarting the launcher…"));
        if (!QProcess::startDetached(m_target, {}, QFileInfo(m_target).absolutePath())) {
            setBusy(false, QStringLiteral("The operation succeeded, but the launcher could not be restarted."));
            return;
        }
        m_restartOnExit = false;
        qApp->quit();
    }

   private:
    QString m_target;
    QString m_statePath;
    QString m_family;
    QString m_installedVersion;
    QLabel* m_latestLabel = nullptr;
    QLabel* m_status = nullptr;
    QProgressBar* m_progress = nullptr;
    QLabel* m_progressText = nullptr;
    QPushButton* m_checkButton = nullptr;
    QPushButton* m_updateButton = nullptr;
    QPushButton* m_removeButton = nullptr;
    QPushButton* m_closeButton = nullptr;
    QNetworkAccessManager m_network;
    QPointer<QNetworkReply> m_reply;
    ReleaseAsset m_available;
    bool m_busy = false;
    bool m_restartOnExit = true;
};
}  // namespace

int main(int argc, char* argv[])
{
    bool managerRequested = false;
    for (int index = 1; index < argc; ++index) {
        if (QString::fromLocal8Bit(argv[index]) == QStringLiteral("--manage")) {
            managerRequested = true;
            break;
        }
    }
    std::unique_ptr<QCoreApplication> app;
    if (managerRequested) {
        app = std::make_unique<QApplication>(argc, argv);
    } else {
        app = std::make_unique<QCoreApplication>(argc, argv);
    }
    QCoreApplication::setApplicationName(QStringLiteral("Organizer Patch Installer"));

    QCommandLineParser parser;
    parser.addHelpOption();
    QCommandLineOption manageOption(QStringLiteral("manage"), QStringLiteral("open the graphical patch manager"));
    QCommandLineOption modeOption(QStringLiteral("mode"), QStringLiteral("install, update, or remove"), QStringLiteral("mode"));
    QCommandLineOption targetOption(QStringLiteral("target"), QStringLiteral("launcher executable"), QStringLiteral("path"));
    QCommandLineOption sourceOption(QStringLiteral("source"), QStringLiteral("replacement executable"), QStringLiteral("path"));
    QCommandLineOption hashOption(QStringLiteral("sha256"), QStringLiteral("expected source SHA-256"), QStringLiteral("digest"));
    QCommandLineOption stateOption(QStringLiteral("state"), QStringLiteral("patch state file"), QStringLiteral("path"));
    QCommandLineOption versionOption(QStringLiteral("version"), QStringLiteral("new patch version"), QStringLiteral("version"));
    QCommandLineOption familyOption(QStringLiteral("family"), QStringLiteral("launcher family for a new install"),
                                    QStringLiteral("family"));
    QCommandLineOption restartOption(QStringLiteral("restart"), QStringLiteral("restart the launcher after maintenance"));
    parser.addOptions({ manageOption, modeOption, targetOption, sourceOption, hashOption, stateOption, versionOption,
                        familyOption, restartOption });
    parser.process(*app);

    if (parser.isSet(manageOption)) {
        const auto target = QFileInfo(parser.value(targetOption)).absoluteFilePath();
        const auto state = QFileInfo(parser.value(stateOption)).absoluteFilePath();
        const auto family = parser.value(familyOption).toLower();
        const auto validFamily = family == QStringLiteral("pineconemc") || family == QStringLiteral("prism") ||
                                 family == QStringLiteral("freesm");
        if (!QFileInfo::exists(target) || !QFileInfo::exists(state) || !validStateLocation(state, target) ||
            !validFamily) {
            QMessageBox::critical(nullptr, QStringLiteral("Organizer Patch Manager"),
                                  QStringLiteral("The launcher target or patch state is invalid."));
            if (QFileInfo::exists(target)) {
                QProcess::startDetached(target, {}, QFileInfo(target).absolutePath());
            }
            return 1;
        }

        QApplication::setStyle(QStringLiteral("Fusion"));
        QPalette palette;
        palette.setColor(QPalette::Window, QColor(QStringLiteral("#0f0f0f")));
        palette.setColor(QPalette::WindowText, QColor(QStringLiteral("#f0f0f0")));
        palette.setColor(QPalette::Base, QColor(QStringLiteral("#0f0f0f")));
        palette.setColor(QPalette::AlternateBase, QColor(QStringLiteral("#141414")));
        palette.setColor(QPalette::Text, QColor(QStringLiteral("#f0f0f0")));
        palette.setColor(QPalette::Button, QColor(QStringLiteral("#141414")));
        palette.setColor(QPalette::ButtonText, QColor(QStringLiteral("#f0f0f0")));
        palette.setColor(QPalette::Highlight, QColor(QStringLiteral("#353535")));
        palette.setColor(QPalette::HighlightedText, QColor(QStringLiteral("#ffffff")));
        palette.setColor(QPalette::Disabled, QPalette::Text, QColor(QStringLiteral("#777777")));
        palette.setColor(QPalette::Disabled, QPalette::ButtonText, QColor(QStringLiteral("#777777")));
        QApplication::setPalette(palette);
        qApp->setStyleSheet(QStringLiteral(
            "QWidget { background: #0f0f0f; color: #f0f0f0; }"
            "QFrame[frameShape=\"6\"] { border: 1px solid #2b2b2b; border-radius: 5px; }"
            "QPushButton { background: #141414; border: 1px solid #2b2b2b; border-radius: 4px; padding: 6px 12px; }"
            "QPushButton:hover { background: #1b1b1b; }"
            "QPushButton:disabled { color: #777777; }"
            "QProgressBar { border: 1px solid #2b2b2b; background: #0f0f0f; min-height: 16px; }"
            "QProgressBar::chunk { background: #777777; }"));

        OrganizerPatchManagerWindow window(target, state, family);
        window.show();
        return app->exec();
    }

    const auto mode = parser.value(modeOption).toLower();
    const auto target = QFileInfo(parser.value(targetOption)).absoluteFilePath();
    const auto source = QFileInfo(parser.value(sourceOption)).absoluteFilePath();
    const auto expectedHash = parser.value(hashOption).toLatin1().toLower();
    const auto state = QFileInfo(parser.value(stateOption)).absoluteFilePath();
    const auto restart = parser.isSet(restartOption);
    QString error;

    if (mode != QStringLiteral("install") && mode != QStringLiteral("update") && mode != QStringLiteral("remove")) {
        error = QStringLiteral("Invalid installer mode.");
    } else if (!replaceLauncher(target, source, expectedHash, mode, state, parser.value(versionOption),
                                parser.value(familyOption), &error)) {
        // replaceLauncher provides the actionable error.
    }

    if (!error.isEmpty()) {
        writeFailure(target, error);
    }
    if (restart && QFileInfo::exists(target)) {
        QProcess::startDetached(target, {}, QFileInfo(target).absolutePath());
    }
    return error.isEmpty() ? 0 : 1;
}
