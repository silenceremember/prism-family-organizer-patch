// SPDX-License-Identifier: GPL-3.0-only

#pragma once

#include <QPointer>
#include <QUrl>
#include <QWidget>

#include "ui/pages/BasePage.h"

class QLabel;
class QPushButton;
class QNetworkReply;
class QProgressBar;
class QEvent;

class OrganizerPatchPage final : public QWidget, public BasePage {
    Q_OBJECT

   public:
    explicit OrganizerPatchPage(QWidget* parent = nullptr);

    QString displayName() const override { return tr("Organizer Patch"); }
    QIcon icon() const override;
    QString id() const override { return QStringLiteral("organizer-patch"); }

   protected:
    void changeEvent(QEvent* event) override;

   private:
    struct ReleaseAsset {
        QString version;
        QUrl downloadUrl;
        QByteArray sha256;

        bool isValid() const { return !version.isEmpty() && downloadUrl.isValid() && sha256.size() == 64; }
    };

    void beginCheck(bool manageAfterCheck = false);
    void checkFinished(QNetworkReply* reply);
    void beginManage();
    void downloadManager();
    void managerDownloadFinished(QNetworkReply* reply);
    void setBusy(bool busy, const QString& status = {});
    void setDownloadProgress(qint64 received, qint64 total);
    void resetDownloadProgress();
    void updateBranding();
    bool launchManager(const QString& managerPath, QString* error);

    QString familyId() const;
    QString patchRoot() const;
    QString statePath() const;
    QString installedVersion() const;
    ReleaseAsset newestRelease(const QByteArray& json, const QString& assetName, QString* error) const;

   private:
    QPushButton* m_checkButton = nullptr;
    QPushButton* m_manageButton = nullptr;
    QLabel* m_brandLogo = nullptr;
    QLabel* m_versionValue = nullptr;
    QLabel* m_status = nullptr;
    QProgressBar* m_progressBar = nullptr;
    QLabel* m_progressAmount = nullptr;
    QPointer<QNetworkReply> m_reply;
    ReleaseAsset m_available;
    ReleaseAsset m_managerAsset;
    bool m_manageAfterCheck = false;
};
