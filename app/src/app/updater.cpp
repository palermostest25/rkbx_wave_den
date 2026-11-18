#include "app/updater.h"

#include "app/install.h"

#include <QCoreApplication>
#include <QCryptographicHash>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLoggingCategory>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QProcess>
#include <QStandardPaths>
#include <QSysInfo>
#include <QTemporaryDir>
#include <QtConcurrent>

#ifdef Q_OS_WIN
#include <windows.h>
#endif

Q_LOGGING_CATEGORY(lcUpdate, "wave.update")

namespace wave {
namespace {
const char *kLatest = "https://api.github.com/repos/palermostest25/rkbx_wave_den/releases/latest";
const char *kReleases = "https://github.com/palermostest25/rkbx_wave_den/releases/latest";
const QStringList kHosts = {"https://github.com/", "https://objects.githubusercontent.com/", "https://release-assets.githubusercontent.com/"};
const char *kBundleId = "io.github.palermostest25.rekordbox-wave";

bool fromGitHub(const QString &url) {
    for (const QString &h : kHosts) if (url.startsWith(h)) return true;
    return false;
}
}  // namespace

QString version() { return QStringLiteral(WAVE_VERSION); }

QList<int> versionNumbers(const QString &text) {
    QString t = text.trimmed();
    if (t.startsWith('v') || t.startsWith('V')) t.remove(0, 1);
    QList<int> out;
    for (const QString &part : t.split('.')) {
        bool ok = false;
        const int n = part.toInt(&ok);
        if (!ok || part.isEmpty() || !part.at(0).isDigit()) return {};
        out << n;
    }
    return out;
}

QString assetName() {
#ifdef Q_OS_WIN
    return QStringLiteral("Rekordbox-Wave-Windows-x64.exe");
#elif defined(Q_OS_MACOS)
    return QSysInfo::currentCpuArchitecture() == QLatin1String("arm64") ? QStringLiteral("Rekordbox-Wave-macOS-AppleSilicon.dmg")
                                                                         : QStringLiteral("Rekordbox-Wave-macOS-Intel.dmg");
#else
    return {};
#endif
}

std::optional<Update> newerIn(const QByteArray &json, const QString &current, const QString &asset) {
    const QJsonObject release = QJsonDocument::fromJson(json).object();
    const QList<int> newer = versionNumbers(release.value("tag_name").toString()), mine = versionNumbers(current);
    if (release.value("draft").toBool() || release.value("prerelease").toBool() || newer.isEmpty() || mine.isEmpty() || !(mine < newer))
        return std::nullopt;
    for (const QJsonValue &a : release.value("assets").toArray()) {
        const QJsonObject o = a.toObject();
        if (o.value("name").toString() != asset) continue;
        const QString url = o.value("browser_download_url").toString(), digest = o.value("digest").toString();
        // Only GitHub's own files, and only ones it publishes a checksum for.
        if (!fromGitHub(url) || !digest.startsWith("sha256:") || digest.size() != 71) return std::nullopt;
        QStringList parts;
        for (int n : newer) parts << QString::number(n);
        const QString page = release.value("html_url").toString();
        return Update{parts.join('.'), url, digest.mid(7).toLower(), page.isEmpty() ? QString::fromLatin1(kReleases) : page,
                      qint64(o.value("size").toDouble())};
    }
    return std::nullopt;
}

Updater::Updater(QObject *parent) : QObject(parent), net_(new QNetworkAccessManager(this)) {
    qRegisterMetaType<std::optional<wave::Update>>();
}

bool Updater::canInstall() const {
#ifdef WAVE_PACKAGED
    return !assetName().isEmpty();
#else
    return false;
#endif
}

void Updater::check() {
    QNetworkRequest request{QUrl(QString::fromLatin1(kLatest))};
    request.setRawHeader("Accept", "application/vnd.github+json");
    request.setRawHeader("User-Agent", "Rekordbox-Wave-updater");
    request.setTransferTimeout(10000);
    QNetworkReply *reply = net_->get(request);
    connect(reply, &QNetworkReply::finished, this, [this, reply] {
        reply->deleteLater();
        if (reply->error() != QNetworkReply::NoError) {
            emit checked(std::nullopt, reply->errorString());
            return;
        }
        emit checked(newerIn(reply->readAll(), version(), assetName()), QString());
    });
}

void Updater::install(const Update &update) {
    auto *folder = new QTemporaryDir(QDir::temp().filePath("wave-update-XXXXXX"));
    const QString target = folder->filePath(update.url.section('/', -1));
    auto *file = new QFile(target + ".part", this);
    if (!folder->isValid() || !file->open(QIODevice::WriteOnly)) {
        emit failed(QStringLiteral("The update could not be saved"));
        delete folder;
        return;
    }
    auto *digest = new QCryptographicHash(QCryptographicHash::Sha256);
    QNetworkRequest request{QUrl(update.url)};
    request.setRawHeader("User-Agent", "Rekordbox-Wave-updater");
    request.setAttribute(QNetworkRequest::RedirectPolicyAttribute, QNetworkRequest::NoLessSafeRedirectPolicy);
    QNetworkReply *reply = net_->get(request);
    connect(reply, &QNetworkReply::readyRead, this, [reply, file, digest] {
        const QByteArray chunk = reply->readAll();
        digest->addData(chunk);
        file->write(chunk);
    });
    connect(reply, &QNetworkReply::downloadProgress, this, [this, update](qint64 done, qint64) {
        emit progress(int(done * 100 / std::max<qint64>(1, update.size)));
    });
    connect(reply, &QNetworkReply::finished, this, [this, reply, file, digest, folder, target, update] {
        reply->deleteLater();
        const QByteArray rest = reply->readAll();
        digest->addData(rest);
        file->write(rest);
        file->close();
        const QString sha = QString::fromLatin1(digest->result().toHex());
        delete digest;
        QString why;
        if (reply->error() != QNetworkReply::NoError) why = reply->errorString();
        else if (!fromGitHub(reply->url().toString())) why = QStringLiteral("The update was redirected away from GitHub");
        else if (file->size() != update.size || sha != update.sha256) why = QStringLiteral("The downloaded update did not match GitHub's checksum");
        if (why.isEmpty()) {
            QFile::remove(target);
            file->rename(target);
            why = swapIn(target);
        }
        file->deleteLater();
        delete folder;  // the new Wave is in place (or nothing changed): the download goes
        if (why.isEmpty()) emit ready();
        else {
            qCWarning(lcUpdate) << "Update failed:" << why;
            emit failed(why);
        }
    });
}

QString Updater::swapIn(const QString &file) {
#ifdef Q_OS_WIN
    const QString exe = QCoreApplication::applicationFilePath();
    const QString folder = QFileInfo(exe).absolutePath();
    if (!install::writable(folder)) {
        // Installed in Program Files: the new Wave swaps itself in, with Windows' permission.
        const QString why = install::elevated(file, {"--replace", QDir::toNativeSeparators(exe)});
        if (!why.isEmpty()) return why;
    } else {
        const QString old = QDir(folder).filePath(QFileInfo(exe).completeBaseName() + ".old.exe");
        QFile::remove(old);
        if (!QFile::rename(exe, old)) return QStringLiteral("Wave could not move itself aside");  // a running exe can be renamed, not replaced
        if (!QFile::copy(file, exe)) {
            QFile::rename(old, exe);
            return QStringLiteral("The new Wave could not be put in place");
        }
    }
    if (!QProcess::startDetached(exe, {}, folder)) return QStringLiteral("The new Wave did not start");
    return {};
#elif defined(Q_OS_MACOS)
    QDir app(QCoreApplication::applicationDirPath());
    app.cdUp();
    app.cdUp();
    const QString bundle = app.absolutePath();
    const QString parent = QFileInfo(bundle).absolutePath();
    if (!bundle.endsWith(".app") || !QFileInfo(parent).isWritable())
        return QStringLiteral("Wave can't replace itself in %1: download the update from GitHub instead").arg(parent);
    const QString fresh = parent + "/." + QFileInfo(bundle).fileName() + ".new", old = parent + "/." + QFileInfo(bundle).fileName() + ".old";
    QDir(fresh).removeRecursively();
    QTemporaryDir mount;
    if (QProcess::execute("/usr/bin/hdiutil", {"attach", "-nobrowse", "-readonly", "-noautoopen", "-mountpoint", mount.path(), file}) != 0)
        return QStringLiteral("The update could not be opened");
    const QStringList apps = QDir(mount.path()).entryList({"*.app"}, QDir::Dirs);
    const int copied = apps.isEmpty() ? 1 : QProcess::execute("/usr/bin/ditto", {mount.path() + "/" + apps[0], fresh});
    QProcess::execute("/usr/bin/hdiutil", {"detach", mount.path(), "-quiet"});
    if (copied != 0) return QStringLiteral("The update could not be copied");
    QProcess sign;
    sign.start("/usr/bin/codesign", {"-dv", fresh});
    sign.waitForFinished(30000);
    const bool ours = QString::fromUtf8(sign.readAllStandardError()).contains(QStringLiteral("Identifier=%1").arg(kBundleId));
    if (!ours || QProcess::execute("/usr/bin/codesign", {"--verify", "--deep", "--strict", fresh}) != 0) {
        QDir(fresh).removeRecursively();
        return QStringLiteral("The downloaded Wave is not signed as Rekordbox Wave");
    }
    auto q = [](const QString &s) { return "'" + QString(s).replace("'", "'\\''") + "'"; };
    // Waits for this Wave to quit, swaps the bundles (back again if the swap fails), then opens the new one.
    const QString script = QStringLiteral("while kill -0 %1 2>/dev/null; do sleep 0.2; done; rm -rf %2; mv %3 %2 && { mv %4 %3 || mv %2 %3; }; rm -rf %2; open %3")
                               .arg(QCoreApplication::applicationPid()).arg(q(old), q(bundle), q(fresh));
    if (!QProcess::startDetached("/bin/sh", {"-c", script})) return QStringLiteral("The update could not be started");
    return {};
#else
    Q_UNUSED(file);
    return QStringLiteral("Updating is built for Windows and macOS");
#endif
}

void Updater::cleanUp() {
#ifdef Q_OS_WIN
    const QFileInfo exe(QCoreApplication::applicationFilePath());
    QFile::remove(exe.absoluteDir().filePath(exe.completeBaseName() + ".old.exe"));
#elif defined(Q_OS_MACOS)
    QDir app(QCoreApplication::applicationDirPath());
    app.cdUp();
    app.cdUp();
    if (app.absolutePath().endsWith(".app"))
        for (const char *leftover : {".new", ".old"})
            QDir(QFileInfo(app.absolutePath()).absolutePath() + "/." + QFileInfo(app.absolutePath()).fileName() + leftover).removeRecursively();
#endif
}

}  // namespace wave
