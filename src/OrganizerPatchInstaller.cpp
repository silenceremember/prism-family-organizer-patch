Exit code: 0
Wall time: 0.2 seconds
Output:
// SPDX-License-Identifier: GPL-3.0-only

#include <QCommandLineOption>
#include <QCommandLineParser>
#include <QCoreApplication>
#include <QCryptographicHash>
#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QJsonObject>
#include <QProcess>
#include <QSaveFile>
#include <QTextStream>
#include <QThread>

namespace {
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
}  // namespace

int main(int argc, char* argv[])
{
    QCoreApplication app(argc, argv);
    QCoreApplication::setApplicationName(QStringLiteral("Organizer Patch Installer"));

    QCommandLineParser parser;
    parser.addHelpOption();
    QCommandLineOption modeOption(QStringLiteral("mode"), QStringLiteral("install, update, or remove"), QStringLiteral("mode"));
    QCommandLineOption targetOption(QStringLiteral("target"), QStringLiteral("launcher executable"), QStringLiteral("path"));
    QCommandLineOption sourceOption(QStringLiteral("source"), QStringLiteral("replacement executable"), QStringLiteral("path"));
    QCommandLineOption hashOption(QStringLiteral("sha256"), QStringLiteral("expected source SHA-256"), QStringLiteral("digest"));
    QCommandLineOption stateOption(QStringLiteral("state"), QStringLiteral("patch state file"), QStringLiteral("path"));
    QCommandLineOption versionOption(QStringLiteral("version"), QStringLiteral("new patch version"), QStringLiteral("version"));
    QCommandLineOption familyOption(QStringLiteral("family"), QStringLiteral("launcher family for a new install"),
                                    QStringLiteral("family"));
    QCommandLineOption restartOption(QStringLiteral("restart"), QStringLiteral("restart the launcher after maintenance"));
    parser.addOptions(
        { modeOption, targetOption, sourceOption, hashOption, stateOption, versionOption, familyOption, restartOption });
    parser.process(app);

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

