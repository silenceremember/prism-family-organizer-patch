Exit code: 0
Wall time: 0.2 seconds
Output:
// SPDX-License-Identifier: GPL-3.0-only

#include "OrganizerPatchPage.h"

#include <array>

#include <QCoreApplication>
#include <QCryptographicHash>
#include <QDateTime>
#include <QDesktopServices>
#include <QDir>
#include <QEvent>
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
#include <QPalette>
#include <QProcess>
#include <QProgressBar>
#include <QPushButton>
#include <QSaveFile>
#include <QUuid>
#include <QVBoxLayout>

#include "Application.h"
#include "BuildConfig.h"
#include "Version.h"

namespace {
constexpr auto kFallbackVersion = "0.1.0-test.0";
constexpr auto kRepositoryApi =
    "https://api.github.com/repos/silenceremember/prism-family-organizer-patch/releases?per_page=20";
constexpr auto kRepositoryUrl = "https://github.com/silenceremember/prism-family-organizer-patch";
constexpr auto kReleasesUrl = "https://github.com/silenceremember/prism-family-organizer-patch/releases";

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

QJsonObject readObject(const QString& path)
{
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) {
        return {};
    }
    const auto document = QJsonDocument::fromJson(file.readAll());
    return document.isObject() ? document.object() : QJsonObject{};
}
}  // namespace

OrganizerPatchPage::OrganizerPatchPage(QWidget* parent) : QWidget(parent)
{
    auto* pageLayout = new QVBoxLayout(this);
    pageLayout->setContentsMargins(6, 6, 6, 6);

    auto* card = new QFrame(this);
    card->setFrameShape(QFrame::StyledPanel);
    auto* cardLayout = new QHBoxLayout(card);
    cardLayout->setContentsMargins(16, 16, 16, 16);
    cardLayout->setSpacing(14);

    m_brandLogo = new QLabel(card);
    m_brandLogo->setFixedSize(116, 72);
    m_brandLogo->setAlignment(Qt::AlignCenter);
    m_brandLogo->setToolTip(tr("Prism Family Organizer Patch"));
    cardLayout->addWidget(m_brandLogo, 0, Qt::AlignTop);

    auto* content = new QVBoxLayout;
    content->setSpacing(8);
    auto* title = new QLabel(tr("Prism Family Organizer Patch"), card);
    auto titleFont = title->font();
    titleFont.setBold(true);
    titleFont.setPointSizeF(titleFont.pointSizeF() + 2.0);
    title->setFont(titleFont);
    content->addWidget(title);

    auto* description = new QLabel(
        tr("Organizer tables and typed groups for PineconeMC, Prism Launcher, and Freesm Launcher."), card);
    description->setWordWrap(true);
    content->addWidget(description);

    auto* links = new QHBoxLayout;
    links->setSpacing(6);
    auto* repositoryButton = new QPushButton(QIcon::fromTheme(QStringLiteral("externaltools")), tr("GitHub"), card);
    auto* releasesButton = new QPushButton(QIcon::fromTheme(QStringLiteral("checkupdate")), tr("Releases"), card);
    for (auto* button : { repositoryButton, releasesButton }) {
        button->setFlat(true);
        button->setSizePolicy(QSizePolicy::Fixed, QSizePolicy::Fixed);
        button->setCursor(Qt::PointingHandCursor);
    }
    repositoryButton->setToolTip(tr("Open the Organizer Patch repository"));
    releasesButton->setToolTip(tr("Open Organizer Patch releases"));
    links->addWidget(repositoryButton);
    links->addWidget(releasesButton);
    links->addStretch();
    content->addLayout(links);

    auto* topRow = new QHBoxLayout;
    m_actionButton = new QPushButton(tr("Check"), card);
    m_actionButton->setIcon(QIcon::fromTheme(QStringLiteral("refresh")));
    m_actionButton->setMinimumWidth(110);
    topRow->addWidget(m_actionButton);
    topRow->addSpacing(8);
    auto* versionLabel = new QLabel(tr("Version:"), card);
    auto versionFont = versionLabel->font();
    versionFont.setBold(true);
    versionLabel->setFont(versionFont);
    topRow->addWidget(versionLabel);
    m_versionValue = new QLabel(installedVersion(), card);
    m_versionValue->setTextInteractionFlags(Qt::TextSelectableByMouse);
    topRow->addWidget(m_versionValue);
    topRow->addStretch();
    content->addLayout(topRow);

    m_status = new QLabel(card);
    m_status->setWordWrap(true);
    m_status->setVisible(false);
    content->addWidget(m_status);

    auto* progressRow = new QHBoxLayout;
    progressRow->setSpacing(10);
    m_progressBar = new QProgressBar(card);
    m_progressBar->setRange(0, 1000);
    m_progressBar->setTextVisible(false);
    m_progressBar->setMinimumHeight(18);
    m_progressBar->setStyleSheet(QStringLiteral("QProgressBar { margin: 0px; }"));
    m_progressBar->setVisible(false);
    progressRow->addWidget(m_progressBar, 1);
    m_progressAmount = new QLabel(card);
    m_progressAmount->setAlignment(Qt::AlignRight | Qt::AlignVCenter);
    m_progressAmount->setMinimumWidth(170);
    m_progressAmount->setVisible(false);
    progressRow->addWidget(m_progressAmount);
    content->addLayout(progressRow);

    m_removeButton = new QPushButton(QIcon::fromTheme(QStringLiteral("delete")), tr("Remove"), card);
    m_removeButton->setMinimumWidth(110);
    m_removeButton->setSizePolicy(QSizePolicy::Fixed, QSizePolicy::Fixed);
    m_removeButton->setToolTip(tr("Restore the pristine launcher executable"));
    content->addWidget(m_removeButton, 0, Qt::AlignLeft);

    cardLayout->addLayout(content, 1);
    pageLayout->addWidget(card);
    pageLayout->addStretch();

    const auto state = readObject(statePath());
    const auto original = QDir(patchRoot()).filePath(state.value(QStringLiteral("original")).toString());
    m_canRemove = !state.isEmpty() && QFileInfo::exists(original);
    m_removeButton->setEnabled(m_canRemove);

    connect(m_actionButton, &QPushButton::clicked, this, [this] {
        if (m_action == Action::Update) {
            beginUpdate();
        } else if (m_action == Action::Check) {
            beginCheck();
        }
    });
    connect(m_removeButton, &QPushButton::clicked, this, &OrganizerPatchPage::beginRemove);
    connect(repositoryButton, &QPushButton::clicked, this,
            [] { QDesktopServices::openUrl(QUrl(QString::fromLatin1(kRepositoryUrl))); });
    connect(releasesButton, &QPushButton::clicked, this,
            [] { QDesktopServices::openUrl(QUrl(QString::fromLatin1(kReleasesUrl))); });
    updateBranding();
}

void OrganizerPatchPage::changeEvent(QEvent* event)
{
    QWidget::changeEvent(event);
    if (event->type() == QEvent::PaletteChange || event->type() == QEvent::ApplicationPaletteChange ||
        event->type() == QEvent::StyleChange) {
        updateBranding();
    }
}

void OrganizerPatchPage::updateBranding()
{
    if (!m_brandLogo) {
        return;
    }
    const auto resource = palette().color(QPalette::Window).lightness() < 128
                              ? QStringLiteral(":/organizer/logo.svg")
                              : QStringLiteral(":/organizer/logo-black.svg");
    m_brandLogo->setPixmap(QIcon(resource).pixmap(QSize(108, 68)));
}

QString OrganizerPatchPage::familyId() const
{
    const auto binary = BuildConfig.LAUNCHER_APP_BINARY_NAME.toLower();
    if (binary.contains(QStringLiteral("freesm"))) {
        return QStringLiteral("freesm");
    }
    if (binary.contains(QStringLiteral("prism")) && !binary.contains(QStringLiteral("ely"))) {
        return QStringLiteral("prism");
    }
    return QStringLiteral("pineconemc");
}

QString OrganizerPatchPage::patchRoot() const
{
    return QDir(QCoreApplication::applicationDirPath()).filePath(QStringLiteral(".organizer-patch"));
}

QString OrganizerPatchPage::statePath() const
{
    return QDir(patchRoot()).filePath(QStringLiteral("state.json"));
}

QString OrganizerPatchPage::installedVersion() const
{
    const auto version = readObject(statePath()).value(QStringLiteral("version")).toString();
    return version.isEmpty() ? QString::fromLatin1(kFallbackVersion) : version;
}

void OrganizerPatchPage::setAction(Action action)
{
    m_action = action;
    switch (action) {
        case Action::Check:
            m_actionButton->setText(tr("Check"));
            m_actionButton->setIcon(QIcon::fromTheme(QStringLiteral("refresh")));
            m_actionButton->setEnabled(true);
            break;
        case Action::Update:
            m_actionButton->setText(tr("Update"));
            m_actionButton->setIcon(QIcon::fromTheme(QStringLiteral("checkupdate")));
            m_actionButton->setEnabled(true);
            break;
        case Action::Latest:
            m_actionButton->setText(tr("Latest"));
            m_actionButton->setIcon(QIcon::fromTheme(QStringLiteral("checkupdate")));
            m_actionButton->setEnabled(false);
            break;
    }
}

void OrganizerPatchPage::setBusy(bool busy, const QString& status)
{
    m_actionButton->setEnabled(!busy && m_action != Action::Latest);
    m_removeButton->setEnabled(!busy && m_canRemove);
    m_status->setText(status);
    m_status->setVisible(!status.isEmpty());
}

void OrganizerPatchPage::resetDownloadProgress()
{
    m_progressBar->setVisible(false);
    m_progressAmount->setVisible(false);
    m_progressBar->setRange(0, 1000);
    m_progressBar->setValue(0);
    m_progressAmount->clear();
}

void OrganizerPatchPage::setDownloadProgress(qint64 received, qint64 total)
{
    m_progressBar->setVisible(true);
    m_progressAmount->setVisible(true);
    if (total <= 0) {
        m_progressBar->setRange(0, 0);
        m_progressAmount->setText(tr("%1 / unknown").arg(formatBytes(received)));
        return;
    }
    m_progressBar->setRange(0, 1000);
    const auto progress = qBound(0, qRound(1000.0 * static_cast<double>(received) / static_cast<double>(total)), 1000);
    m_progressBar->setValue(progress);
    m_progressAmount->setText(
        tr("%1 / %2 (%3%)").arg(formatBytes(received), formatBytes(total)).arg(qRound(progress / 10.0)));
}

void OrganizerPatchPage::beginCheck()
{
    if (m_reply) {
        return;
    }

    resetDownloadProgress();
    setBusy(true, tr("Checking GitHub Releases…"));
    QNetworkRequest request(QUrl(QString::fromLatin1(kRepositoryApi)));
    request.setHeader(QNetworkRequest::UserAgentHeader, BuildConfig.USER_AGENT);
    request.setRawHeader("Accept", "application/vnd.github+json");
    request.setAttribute(QNetworkRequest::RedirectPolicyAttribute, QNetworkRequest::NoLessSafeRedirectPolicy);
    auto* reply = APPLICATION->network()->get(request);
    m_reply = reply;
    connect(reply, &QNetworkReply::finished, this, [this, reply] { checkFinished(reply); });
}

OrganizerPatchPage::ReleaseAsset OrganizerPatchPage::newestRelease(const QByteArray& json, QString* error) const
{
    QJsonParseError parseError;
    const auto document = QJsonDocument::fromJson(json, &parseError);
    if (parseError.error != QJsonParseError::NoError || !document.isArray()) {
        *error = tr("GitHub returned invalid release data.");
        return {};
    }

    const auto expectedName = QStringLiteral("prism-family-organizer-patch-%1-windows-x64.exe").arg(familyId());
    ReleaseAsset newest;
    for (const auto releaseValue : document.array()) {
        const auto release = releaseValue.toObject();
        if (release.value(QStringLiteral("draft")).toBool()) {
            continue;
        }
        auto tag = release.value(QStringLiteral("tag_name")).toString();
        if (tag.startsWith(QLatin1Char('v'), Qt::CaseInsensitive)) {
            tag.remove(0, 1);
        }
        if (tag.isEmpty()) {
            continue;
        }
        for (const auto assetValue : release.value(QStringLiteral("assets")).toArray()) {
            const auto asset = assetValue.toObject();
            if (asset.value(QStringLiteral("name")).toString() != expectedName) {
                continue;
            }
            auto digest = asset.value(QStringLiteral("digest")).toString();
            if (!digest.startsWith(QStringLiteral("sha256:"), Qt::CaseInsensitive)) {
                continue;
            }
            digest.remove(0, 7);
            ReleaseAsset candidate{ tag, QUrl(asset.value(QStringLiteral("browser_download_url")).toString()),
                                    digest.toLatin1().toLower() };
            if (candidate.isValid() && (!newest.isValid() || Version(candidate.version) > Version(newest.version))) {
                newest = std::move(candidate);
            }
        }
    }
    if (!newest.isValid()) {
        *error = tr("No compatible Windows release was found for %1.").arg(familyId());
    }
    return newest;
}

void OrganizerPatchPage::checkFinished(QNetworkReply* reply)
{
    const auto body = reply->readAll();
    const auto networkError = reply->error();
    const auto errorText = reply->errorString();
    reply->deleteLater();
    m_reply = nullptr;

    if (networkError != QNetworkReply::NoError) {
        setAction(Action::Check);
        setBusy(false, tr("Check failed: %1").arg(errorText));
        return;
    }

    QString error;
    m_available = newestRelease(body, &error);
    if (!m_available.isValid()) {
        setAction(Action::Check);
        setBusy(false, error);
        return;
    }

    if (Version(m_available.version) > Version(installedVersion())) {
        setAction(Action::Update);
        setBusy(false, tr("Version %1 is available.").arg(m_available.version));
    } else {
        setAction(Action::Latest);
        setBusy(false, tr("The installed patch is up to date."));
    }
}

void OrganizerPatchPage::beginUpdate()
{
    if (!m_available.isValid() || m_reply) {
        return;
    }
    if (QMessageBox::question(this, tr("Update Organizer Patch"),
                              tr("Download version %1 and restart the launcher?").arg(m_available.version)) !=
        QMessageBox::Yes) {
        return;
    }

    QDir().mkpath(QDir(patchRoot()).filePath(QStringLiteral("downloads")));
    setBusy(true, tr("Downloading %1… The launcher will restart automatically.").arg(m_available.version));
    setDownloadProgress(0, -1);
    QNetworkRequest request(m_available.downloadUrl);
    request.setHeader(QNetworkRequest::UserAgentHeader, BuildConfig.USER_AGENT);
    request.setAttribute(QNetworkRequest::RedirectPolicyAttribute, QNetworkRequest::NoLessSafeRedirectPolicy);
    auto* reply = APPLICATION->network()->get(request);
    m_reply = reply;
    connect(reply, &QNetworkReply::downloadProgress, this, &OrganizerPatchPage::setDownloadProgress);
    connect(reply, &QNetworkReply::finished, this, [this, reply] { updateFinished(reply); });
}

void OrganizerPatchPage::updateFinished(QNetworkReply* reply)
{
    const auto payload = reply->readAll();
    const auto networkError = reply->error();
    const auto errorText = reply->errorString();
    reply->deleteLater();
    m_reply = nullptr;

    if (networkError != QNetworkReply::NoError) {
        resetDownloadProgress();
        setAction(Action::Update);
        setBusy(false, tr("Download failed: %1").arg(errorText));
        return;
    }
    setDownloadProgress(payload.size(), payload.size());
    setBusy(true, tr("Verifying the downloaded update…"));
    const auto actualHash = QCryptographicHash::hash(payload, QCryptographicHash::Sha256).toHex();
    if (actualHash != m_available.sha256) {
        resetDownloadProgress();
        setAction(Action::Update);
        setBusy(false, tr("Downloaded file failed SHA-256 verification."));
        return;
    }

    const auto downloads = QDir(patchRoot()).filePath(QStringLiteral("downloads"));
    const auto payloadPath = QDir(downloads).filePath(QStringLiteral("organizer-update-%1.exe").arg(m_available.version));
    QSaveFile output(payloadPath);
    if (!output.open(QIODevice::WriteOnly) || output.write(payload) != payload.size() || !output.commit()) {
        resetDownloadProgress();
        setAction(Action::Update);
        setBusy(false, tr("Could not save the downloaded update."));
        return;
    }

    QString error;
    const QStringList arguments{ QStringLiteral("--mode"),
                                 QStringLiteral("update"),
                                 QStringLiteral("--target"),
                                 QCoreApplication::applicationFilePath(),
                                 QStringLiteral("--source"),
                                 payloadPath,
                                 QStringLiteral("--sha256"),
                                 QString::fromLatin1(m_available.sha256),
                                 QStringLiteral("--state"),
                                 statePath(),
                                 QStringLiteral("--version"),
                                 m_available.version,
                                 QStringLiteral("--restart") };
    if (!launchInstaller(arguments, &error)) {
        resetDownloadProgress();
        setAction(Action::Update);
        setBusy(false, error);
        return;
    }
    setBusy(true, tr("Update verified. Restarting the launcher…"));
    QCoreApplication::quit();
}
bool OrganizerPatchPage::launchInstaller(const QStringList& arguments, QString* error)
{
    QFile embeddedInstaller(QStringLiteral(":/organizer/organizer-patch-installer.exe"));
    if (!embeddedInstaller.open(QIODevice::ReadOnly)) {
        *error = tr("The embedded Organizer Patch Installer is unavailable.");
        return false;
    }

    const auto tempDir = QDir(QDir::tempPath())
                             .filePath(QStringLiteral("prism-family-organizer-patch/%1").arg(QUuid::createUuid().toString(QUuid::Id128)));
    if (!QDir().mkpath(tempDir)) {
        *error = tr("Could not create a temporary installer directory.");
        return false;
    }
    const auto temporaryInstaller = QDir(tempDir).filePath(QStringLiteral("organizer-patch-installer.exe"));
    QSaveFile installerOutput(temporaryInstaller);
    const auto installerPayload = embeddedInstaller.readAll();
    if (installerPayload.isEmpty() || !installerOutput.open(QIODevice::WriteOnly) ||
        installerOutput.write(installerPayload) != installerPayload.size() || !installerOutput.commit()) {
        *error = tr("Could not extract the embedded Organizer Patch Installer.");
        return false;
    }
    const auto installedQtCore = QDir(QCoreApplication::applicationDirPath()).filePath(QStringLiteral("Qt6Core.dll"));
    const auto temporaryQtCore = QDir(tempDir).filePath(QStringLiteral("Qt6Core.dll"));
    if (!QFileInfo::exists(installedQtCore) || !QFile::copy(installedQtCore, temporaryQtCore)) {
        *error = tr("Could not prepare the Organizer Patch Installer runtime.");
        return false;
    }
    if (!QProcess::startDetached(temporaryInstaller, arguments, tempDir)) {
        *error = tr("Could not start Organizer Patch Installer.");
        return false;
    }
    return true;
}

void OrganizerPatchPage::beginRemove()
{
    if (m_reply) {
        return;
    }
    const auto state = readObject(statePath());
    const auto originalRelative = state.value(QStringLiteral("original")).toString();
    const auto originalHash = state.value(QStringLiteral("originalSha256")).toString().toLatin1().toLower();
    const auto original = QDir(patchRoot()).filePath(originalRelative);
    if (originalRelative.isEmpty() || originalHash.size() != 64 || sha256File(original) != originalHash) {
        QMessageBox::critical(this, tr("Remove Organizer Patch"),
                              tr("The saved original launcher is missing or failed SHA-256 verification."));
        return;
    }
    if (QMessageBox::warning(this, tr("Remove Organizer Patch"),
                             tr("Restore the original launcher and remove Organizer Patch? The launcher will restart. "
                                "Your instances and group configuration will not be deleted."),
                             QMessageBox::Yes | QMessageBox::Cancel, QMessageBox::Cancel) != QMessageBox::Yes) {
        return;
    }

    QString error;
    const QStringList arguments{ QStringLiteral("--mode"),
                                 QStringLiteral("remove"),
                                 QStringLiteral("--target"),
                                 QCoreApplication::applicationFilePath(),
                                 QStringLiteral("--source"),
                                 original,
                                 QStringLiteral("--sha256"),
                                 QString::fromLatin1(originalHash),
                                 QStringLiteral("--state"),
                                 statePath(),
                                 QStringLiteral("--restart") };
    if (!launchInstaller(arguments, &error)) {
        QMessageBox::critical(this, tr("Remove Organizer Patch"), error);
        return;
    }
    QCoreApplication::quit();
}
