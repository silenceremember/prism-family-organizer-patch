// SPDX-License-Identifier: GPL-3.0-only

#pragma once

#include <QPointer>
#include <QUrl>
#include <QWidget>

#include "ui/pages/BasePage.h"

class QLabel;
class QPushButton;
class QNetworkReply;

class OrganizerPatchPage final : public QWidget, public BasePage {
    Q_OBJECT

   public:
    explicit OrganizerPatchPage(QWidget* parent = nullptr);

    QString displayName() const override { return tr("Organizer Patch"); }
    QIcon icon() const override { return QIcon::fromTheme("update"); }
    QString id() const override { return QStringLiteral("organizer-patch"); }

   private:
    struct ReleaseAsset {
        QString version;
        QUrl downloadUrl;
        QByteArray sha256;

        bool isValid() const { return !version.isEmpty() && downloadUrl.isValid() && sha256.size() == 64; }
    };

    enum class Action { Check, Update, Latest };

    void beginCheck();
    void checkFinished(QNetworkReply* reply);
    void beginUpdate();
    void updateFinished(QNetworkReply* reply);
    void beginRemove();
    void setAction(Action action);
    void setBusy(bool busy, const QString& status = {});
    bool launchMaintenance(const QStringList& arguments, QString* error);

    QString familyId() const;
    QString patchRoot() const;
    QString statePath() const;
    QString installedVersion() const;
    ReleaseAsset newestRelease(const QByteArray& json, QString* error) const;

   private:
    QPushButton* m_actionButton = nullptr;
    QLabel* m_versionValue = nullptr;
    QPushButton* m_removeButton = nullptr;
    QLabel* m_status = nullptr;
    QPointer<QNetworkReply> m_reply;
    ReleaseAsset m_available;
    Action m_action = Action::Check;
    bool m_canRemove = false;
};

