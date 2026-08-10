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
#include <QPainter>
#include <QPalette>
#include <QProcess>
#include <QProcessEnvironment>
#include <QProgressBar>
#include <QPushButton>
#include <QSaveFile>
#include <QStyle>
#include <QSvgRenderer>
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
    auto* repositoryButton = new QPushButton(style()->standardIcon(QStyle::SP_DriveNetIcon), tr("GitHub"), card);
    auto* releasesButton = new QPushButton(style()->standardIcon(QStyle::SP_ArrowDown), tr("Releases"), card);
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
    m_checkButton = new QPushButton(style()->standardIcon(QStyle::SP_BrowserReload), tr("Check"), card);
    m_manageButton = new QPushButton(style()->standardIcon(QStyle::SP_FileDialogDetailedView), tr("Manage"), card);
    for (auto* button : { m_checkButton, m_manageButton }) {
        button->setMinimumWidth(110);
        topRow->addWidget(button);
    }
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

    cardLayout->addLayout(content, 1);
    pageLayout->addWidget(card);
    pageLayout->addStretch();

    connect(m_checkButton, &QPushButton::clicked, this, [this] { beginCheck(); });
    connect(m_manageButton, &QPushButton::clicked, this, &OrganizerPatchPage::beginManage);
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

QIcon OrganizerPatchPage::icon() const
{
    return QIcon(renderSvg(QStringLiteral(":/organizer/logo-background.svg"), QSize(48, 48)));
}

void OrganizerPatchPage::updateBranding()
{
    if (!m_brandLogo) {
        return;
    }
    const auto resource = palette().color(QPalette::Window).lightness() < 128
                              ? QStringLiteral(":/organizer/logo.svg")
                              : QStringLiteral(":/organizer/logo-black.svg");
    m_brandLogo->setPixmap(renderSvg(resource, QSize(108, 68)));
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

void OrganizerPatchPage::setBusy(bool busy, const QString& status)
{
    m_checkButton->setEnabled(!busy);
    m_manageButton->setEnabled(!busy);
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

void OrganizerPatchPage::beginCheck(bool manageAfterCheck)
{
    if (m_reply) {
        return;
    }

    m_manageAfterCheck = manageAfterCheck;
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

OrganizerPatchPage::ReleaseAsset OrganizerPatchPage::newestRelease(const QByteArray& json, const QString& assetName,
                                                                   QString* error) const
{
    QJsonParseError parseError;
    const auto document = QJsonDocument::fromJson(json, &parseError);
    if (parseError.error != QJsonParseError::NoError || !document.isArray()) {
        *error = tr("GitHub returned invalid release data.");
        return {};
    }

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
            if (asset.value(QStringLiteral("name")).toString() != assetName) {
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
        *error = tr("GitHub Releases does not contain %1.").arg(assetName);
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
        setBusy(false, tr("Check failed: %1").arg(errorText));
        m_manageAfterCheck = false;
        return;
    }

    QString error;
    const auto familyAssetName = QStringLiteral("prism-family-organizer-patch-%1-windows-x64.exe").arg(familyId());
    m_available = newestRelease(body, familyAssetName, &error);
    if (!m_available.isValid()) {
        setBusy(false, error);
        m_manageAfterCheck = false;
        return;
    }
    QString managerError;
    m_managerAsset = newestRelease(body, QStringLiteral("prism-family-organizer-patch-installer-windows-x64.exe"),
                                   &managerError);

    if (Version(m_available.version) > Version(installedVersion())) {
        setBusy(false, tr("Version %1 is available.").arg(m_available.version));
    } else {
        setBusy(false, tr("The installed patch is up to date."));
    }

    if (m_manageAfterCheck) {
        m_manageAfterCheck = false;
        if (!m_managerAsset.isValid()) {
            setBusy(false, managerError);
            return;
        }
        downloadManager();
    }
}

void OrganizerPatchPage::beginManage()
{
    if (m_reply) {
        return;
    }
    if (!m_managerAsset.isValid()) {
        beginCheck(true);
        return;
    }
    downloadManager();
}

void OrganizerPatchPage::downloadManager()
{
    if (!m_managerAsset.isValid() || m_reply) {
        return;
    }
    resetDownloadProgress();
    setBusy(true, tr("Downloading Organizer Patch Manager…"));
    setDownloadProgress(0, -1);
    QNetworkRequest request(m_managerAsset.downloadUrl);
    request.setHeader(QNetworkRequest::UserAgentHeader, BuildConfig.USER_AGENT);
    request.setAttribute(QNetworkRequest::RedirectPolicyAttribute, QNetworkRequest::NoLessSafeRedirectPolicy);
    auto* reply = APPLICATION->network()->get(request);
    m_reply = reply;
    connect(reply, &QNetworkReply::downloadProgress, this, &OrganizerPatchPage::setDownloadProgress);
    connect(reply, &QNetworkReply::finished, this, [this, reply] { managerDownloadFinished(reply); });
}

void OrganizerPatchPage::managerDownloadFinished(QNetworkReply* reply)
{
    const auto payload = reply->readAll();
    const auto networkError = reply->error();
    const auto errorText = reply->errorString();
    reply->deleteLater();
    m_reply = nullptr;

    if (networkError != QNetworkReply::NoError) {
        resetDownloadProgress();
        setBusy(false, tr("Manager download failed: %1").arg(errorText));
        return;
    }
    setDownloadProgress(payload.size(), payload.size());
    setBusy(true, tr("Verifying Organizer Patch Manager…"));
    if (QCryptographicHash::hash(payload, QCryptographicHash::Sha256).toHex() != m_managerAsset.sha256) {
        resetDownloadProgress();
        setBusy(false, tr("Organizer Patch Manager failed SHA-256 verification."));
        return;
    }

    const auto tempDir = QDir(QDir::tempPath())
                             .filePath(QStringLiteral("prism-family-organizer-patch/%1")
                                           .arg(QUuid::createUuid().toString(QUuid::Id128)));
    if (!QDir().mkpath(tempDir)) {
        resetDownloadProgress();
        setBusy(false, tr("Could not create a temporary manager directory."));
        return;
    }
    const auto managerPath = QDir(tempDir).filePath(QStringLiteral("organizer-patch-manager.exe"));
    QSaveFile output(managerPath);
    if (!output.open(QIODevice::WriteOnly) || output.write(payload) != payload.size() || !output.commit()) {
        resetDownloadProgress();
        setBusy(false, tr("Could not save Organizer Patch Manager."));
        return;
    }

    QString error;
    if (!launchManager(managerPath, &error)) {
        resetDownloadProgress();
        setBusy(false, error);
        return;
    }
    setBusy(true, tr("Organizer Patch Manager is opening…"));
    QCoreApplication::quit();
}

bool OrganizerPatchPage::launchManager(const QString& managerPath, QString* error)
{
    const auto applicationDir = QCoreApplication::applicationDirPath();
    auto environment = QProcessEnvironment::systemEnvironment();
    environment.insert(QStringLiteral("PATH"),
                       applicationDir + QDir::listSeparator() + environment.value(QStringLiteral("PATH")));
    environment.insert(QStringLiteral("QT_PLUGIN_PATH"), applicationDir);

    QProcess manager;
    manager.setProgram(managerPath);
    manager.setArguments({ QStringLiteral("--manage"), QStringLiteral("--target"),
                           QCoreApplication::applicationFilePath(), QStringLiteral("--state"), statePath(),
                           QStringLiteral("--family"), familyId() });
    manager.setWorkingDirectory(QFileInfo(managerPath).absolutePath());
    manager.setProcessEnvironment(environment);
    if (!manager.startDetached()) {
        *error = tr("Could not start Organizer Patch Manager.");
        return false;
    }
    return true;
}
