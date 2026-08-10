// SPDX-License-Identifier: GPL-3.0-only

#include "OrganizerPatchPage.h"

#include <QCoreApplication>
#include <QCryptographicHash>
#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLabel>
#include <QMessageBox>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QProcess>
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
    auto* box = new QGroupBox(tr("Prism Family Organizer Patch"), this);
    auto* boxLayout = new QVBoxLayout(box);
    auto* topRow = new QHBoxLayout;

    m_actionButton = new QPushButton(tr("Check"), box);
    m_actionButton->setMinimumWidth(90);
    topRow->addWidget(m_actionButton);
    topRow->addSpacing(12);
    topRow->addWidget(new QLabel(tr("Version:"), box));
    m_versionValue = new QLabel(installedVersion(), box);
    m_versionValue->setTextInteractionFlags(Qt::TextSelectableByMouse);
    topRow->addWidget(m_versionValue);
    topRow->addStretch();
    boxLayout->addLayout(topRow);

    m_status = new QLabel(box);
    m_status->setWordWrap(true);
    m_status->setVisible(false);
    boxLayout->addWidget(m_status);

    m_removeButton = new QPushButton(tr("Remove"), box);
    m_removeButton->setMinimumWidth(90);
    m_removeButton->setSizePolicy(QSizePolicy::Fixed, QSizePolicy::Fixed);
    boxLayout->addWidget(m_removeButton, 0, Qt::AlignLeft);

    pageLayout->addWidget(box);
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
            m_actionButton->setEnabled(true);
            break;
        case Action::Update:
            m_actionButton->setText(tr("Update"));
            m_actionButton->setEnabled(true);
            break;
        case Action::Latest:
            m_actionButton->setText(tr("Latest"));
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

void OrganizerPatchPage::beginCheck()
{
    if (m_reply) {
        return;
    }

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
    setBusy(true, tr("Downloading %1…").arg(m_available.version));
    QNetworkRequest request(m_available.downloadUrl);
    request.setHeader(QNetworkRequest::UserAgentHeader, BuildConfig.USER_AGENT);
    request.setAttribute(QNetworkRequest::RedirectPolicyAttribute, QNetworkRequest::NoLessSafeRedirectPolicy);
    auto* reply = APPLICATION->network()->get(request);
    m_reply = reply;
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
        setAction(Action::Update);
        setBusy(false, tr("Download failed: %1").arg(errorText));
        return;
    }
    const auto actualHash = QCryptographicHash::hash(payload, QCryptographicHash::Sha256).toHex();
    if (actualHash != m_available.sha256) {
        setAction(Action::Update);
        setBusy(false, tr("Downloaded file failed SHA-256 verification."));
        return;
    }

    const auto downloads = QDir(patchRoot()).filePath(QStringLiteral("downloads"));
    const auto payloadPath = QDir(downloads).filePath(QStringLiteral("organizer-update-%1.exe").arg(m_available.version));
    QSaveFile output(payloadPath);
    if (!output.open(QIODevice::WriteOnly) || output.write(payload) != payload.size() || !output.commit()) {
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
    if (!launchMaintenance(arguments, &error)) {
        setAction(Action::Update);
        setBusy(false, error);
        return;
    }
    QCoreApplication::quit();
}

bool OrganizerPatchPage::launchMaintenance(const QStringList& arguments, QString* error)
{
    const auto installedHelper = QDir(patchRoot()).filePath(QStringLiteral("organizer-patch-maintenance.exe"));
    if (!QFileInfo::exists(installedHelper)) {
        *error = tr("The Organizer Patch maintenance helper is missing.");
        return false;
    }

    const auto tempDir = QDir(QDir::tempPath())
                             .filePath(QStringLiteral("prism-family-organizer-patch/%1").arg(QUuid::createUuid().toString(QUuid::Id128)));
    if (!QDir().mkpath(tempDir)) {
        *error = tr("Could not create a temporary maintenance directory.");
        return false;
    }
    const auto temporaryHelper = QDir(tempDir).filePath(QStringLiteral("organizer-patch-maintenance.exe"));
    if (!QFile::copy(installedHelper, temporaryHelper)) {
        *error = tr("Could not prepare the maintenance helper.");
        return false;
    }
    const auto installedQtCore = QDir(QCoreApplication::applicationDirPath()).filePath(QStringLiteral("Qt6Core.dll"));
    const auto temporaryQtCore = QDir(tempDir).filePath(QStringLiteral("Qt6Core.dll"));
    if (!QFileInfo::exists(installedQtCore) || !QFile::copy(installedQtCore, temporaryQtCore)) {
        *error = tr("Could not prepare the maintenance runtime.");
        return false;
    }
    if (!QProcess::startDetached(temporaryHelper, arguments, tempDir)) {
        *error = tr("Could not start the maintenance helper.");
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
    if (!launchMaintenance(arguments, &error)) {
        QMessageBox::critical(this, tr("Remove Organizer Patch"), error);
        return;
    }
    QCoreApplication::quit();
}

