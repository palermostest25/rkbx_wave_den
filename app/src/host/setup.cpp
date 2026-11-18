#include "host/setup.h"

#include "host/rekordbox.h"

#include <QCryptographicHash>
#include <QDateTime>
#include <QDir>
#include <QDirIterator>
#include <QFile>
#include <QFutureWatcher>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLoggingCategory>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QProcess>
#include <QProcessEnvironment>
#include <QRegularExpression>
#include <QStandardPaths>
#include <QtConcurrent>

#ifdef Q_OS_WIN
#include <windows.h>
#include <shellapi.h>
#endif

Q_LOGGING_CATEGORY(lcSetup, "wave.setup")

namespace wave::host {

const char *const kDownloadPage = "https://rekordbox.com/en/download/";

namespace {
// Where a maintainer can point Wave at AlphaTheta's exact download for the supported version, without a release.
const char *kSources = "https://raw.githubusercontent.com/palermostest25/rkbx_wave_den/main/docs/rekordbox-sources.json";
// AlphaTheta's own installer, hosted on this project's pre-release (the mirror-rekordbox CI job): first choice,
// as it stays put when rekordbox.com moves on. Checked by its SHA-256 and AlphaTheta's signature like any other.
const char *kMirror = "https://github.com/palermostest25/rkbx_wave_den/releases/download/rekordbox-7.2.19/Install_rekordbox_x64_7_2_19.exe";
const char *kMirrorSha256 = "290bfaa2bbab6d4b650d9e8982b5a8eeaaa86ad168eb8dc3f3d506c11310f862";
const QStringList kOfficial = {"https://rekordbox.com/", "https://cdn.rekordbox.com/", "https://www.rekordbox.com/",
                               "https://github.com/palermostest25/rkbx_wave_den/releases/download/"};
// Where GitHub serves release files from, after its redirect.
const QStringList kGitHubFiles = {"https://objects.githubusercontent.com/", "https://release-assets.githubusercontent.com/"};

bool official(const QString &url) {
    for (const QString &h : kOfficial) if (url.startsWith(h)) return true;
    return false;
}

bool servedOfficially(const QString &url) {
    if (official(url)) return true;
    for (const QString &h : kGitHubFiles) if (url.startsWith(h)) return true;
    return false;
}

QString versionUnderscored() { return supportedText().replace('.', '_'); }

QNetworkRequest request(const QString &url) {
    QNetworkRequest r{QUrl(url)};
    r.setRawHeader("User-Agent", "Rekordbox-Wave-setup");
    r.setAttribute(QNetworkRequest::RedirectPolicyAttribute, QNetworkRequest::NoLessSafeRedirectPolicy);
    return r;
}

// Who signed ``path``, if the signature is valid; "" otherwise.
QString signer(const QString &path) {
    QProcess p;
#ifdef Q_OS_WIN
    QProcessEnvironment env = QProcessEnvironment::systemEnvironment();
    env.insert("WAVE_FILE", QDir::toNativeSeparators(path));
    p.setProcessEnvironment(env);
    p.setCreateProcessArgumentsModifier([](QProcess::CreateProcessArguments *a) { a->flags |= CREATE_NO_WINDOW; });
    p.start("powershell", {"-NoProfile", "-NonInteractive", "-Command",
                           "$s = Get-AuthenticodeSignature -LiteralPath $env:WAVE_FILE; if ($s.Status -eq 'Valid') { $s.SignerCertificate.Subject }"});
    p.waitForFinished(60000);
    return QString::fromUtf8(p.readAllStandardOutput()).trimmed();
#elif defined(Q_OS_MACOS)
    p.start("/usr/sbin/pkgutil", {"--check-signature", path});
    p.waitForFinished(60000);
    const QString out = QString::fromUtf8(p.readAllStandardOutput());
    return p.exitCode() == 0 && out.contains("Status: signed") ? out : QString();
#else
    Q_UNUSED(path);
    return {};
#endif
}

// The installer inside AlphaTheta's zip (or the file itself, when it is one).
QString installerIn(const QString &archive, const QString &folder, QString *error) {
    const QString lower = archive.toLower();
    if (lower.endsWith(".exe") || lower.endsWith(".pkg")) return archive;
    const QString out = QDir(folder).filePath("unpacked");
    QDir().mkpath(out);
#ifdef Q_OS_WIN
    // Windows 10 and later have bsdtar, which reads zips.
    const int code = QProcess::execute(qEnvironmentVariable("SystemRoot", "C:\\Windows") + "\\System32\\tar.exe",
                                       {"-xf", QDir::toNativeSeparators(archive), "-C", QDir::toNativeSeparators(out)});
#else
    const int code = QProcess::execute("/usr/bin/ditto", {"-x", "-k", archive, out});
#endif
    if (code != 0) { *error = QStringLiteral("That file could not be opened"); return {}; }
    QString best;
    for (QDirIterator it(out, {"*.exe", "*.pkg", "*.EXE", "*.PKG"}, QDir::Files | QDir::Dirs | QDir::NoDotAndDotDot, QDirIterator::Subdirectories);
         it.hasNext();) {
        const QString found = it.next();
        if (found.contains("__MACOSX")) continue;
        if (best.isEmpty() || found.size() < best.size()) best = found;
    }
    if (best.isEmpty()) *error = QStringLiteral("No Rekordbox installer in that file");
    return best;
}

// Runs AlphaTheta's installer after checking its signature; blocks until it finishes.
QString runInstaller(const QString &installer) {
    const QString subject = signer(installer);
    if (!trustedSigner(subject)) return QStringLiteral("That installer is not signed by AlphaTheta or Pioneer DJ: Wave will not run it");
    qCInfo(lcSetup) << "Running the Rekordbox installer signed by" << subject.section('\n', 0, 0);
#ifdef Q_OS_WIN
    const std::wstring file = QDir::toNativeSeparators(installer).toStdWString();
    const std::wstring dir = QDir::toNativeSeparators(QFileInfo(installer).absolutePath()).toStdWString();
    SHELLEXECUTEINFOW info{};
    info.cbSize = sizeof info;
    info.fMask = SEE_MASK_NOCLOSEPROCESS;
    info.lpVerb = L"runas";  // Rekordbox installs for every user: Windows asks for permission
    info.lpFile = file.c_str();
    info.lpDirectory = dir.c_str();
    info.nShow = SW_SHOWNORMAL;
    if (!ShellExecuteExW(&info) || !info.hProcess) return QStringLiteral("The Rekordbox installer did not start (permission refused?)");
    WaitForSingleObject(info.hProcess, INFINITE);
    CloseHandle(info.hProcess);
    return {};
#elif defined(Q_OS_MACOS)
    return QProcess::execute("/usr/bin/open", {"-W", installer}) == 0 ? QString() : QStringLiteral("The Rekordbox installer did not run");
#else
    return QStringLiteral("Rekordbox installs on Windows and macOS");
#endif
}
}  // namespace

QString installerName() {
#ifdef Q_OS_WIN
    return QStringLiteral("Install_rekordbox_x64_%1.zip").arg(versionUnderscored());
#else
    return QStringLiteral("Install_rekordbox_%1.pkg.zip").arg(versionUnderscored());
#endif
}

QString downloadFromPage(const QString &html) {
    QString stem = installerName();
    stem.remove(QRegularExpression("(\\.pkg)?\\.zip$"));
    const QRegularExpression link("https://(?:cdn\\.)?rekordbox\\.com/[^\"'\\s<>]*" + QRegularExpression::escape(stem) + "[^\"'\\s<>]*\\.zip");
    const auto m = link.match(html);
    return m.hasMatch() ? m.captured().replace("\\/", "/") : QString();
}

bool trustedSigner(const QString &subject) {
    const QString s = subject.toLower();
    return s.contains("alphatheta") || s.contains("pioneer dj") || s.contains("pioneer corporation");
}

Setup::Setup(QObject *parent) : QObject(parent), net_(new QNetworkAccessManager(this)) {}

void Setup::finish(bool ok, const QString &message) {
    busy_ = false;
    if (!folder_.isEmpty()) QDir(folder_).removeRecursively();
    folder_.clear();
    emit done(ok, message);
}

void Setup::run(const QString &archive) {
    if (busy_) return;
    busy_ = true;
    triedMirror_ = triedSite_ = false;
    folder_ = QDir::temp().filePath(QStringLiteral("wave-rekordbox-%1").arg(QDateTime::currentMSecsSinceEpoch()));
    QDir().mkpath(folder_);
    if (!archive.isEmpty()) installFrom(archive);
    else find();
}

void Setup::find() {
#ifdef Q_OS_WIN
    if (!triedMirror_) {
        triedMirror_ = true;
        download(QString::fromLatin1(kMirror), QString::fromLatin1(kMirrorSha256));
        return;
    }
#endif
    emit status(QStringLiteral("Looking for Rekordbox %1 on rekordbox.com…").arg(supportedText()));
    emit progress(-1);
    QNetworkReply *reply = net_->get(request(kSources));
    connect(reply, &QNetworkReply::finished, this, [this, reply] {
        reply->deleteLater();
        const QJsonObject entry = QJsonDocument::fromJson(reply->readAll()).object().value(
#ifdef Q_OS_WIN
            "windows"
#else
            "mac"
#endif
        ).toObject();
        if (reply->error() == QNetworkReply::NoError && entry.value("version").toString() == supportedText() && official(entry.value("url").toString())) {
            download(entry.value("url").toString(), entry.value("sha256").toString().toLower());
            return;
        }
        QNetworkReply *page = net_->get(request(kDownloadPage));
        connect(page, &QNetworkReply::finished, this, [this, page] {
            page->deleteLater();
            const QString url = downloadFromPage(QString::fromUtf8(page->readAll()));
            if (url.isEmpty())
                finish(false, QStringLiteral("rekordbox.com no longer offers %1 directly. Download it from the rekordbox.com archive, "
                                             "then choose the file with USE A DOWNLOADED INSTALLER.").arg(supportedText()));
            else download(url, {});
        });
    });
}

void Setup::download(const QString &url, const QString &sha256) {
    if (!official(url)) { finish(false, QStringLiteral("Wave only downloads Rekordbox from rekordbox.com or Wave's own release")); return; }
    emit status(QStringLiteral("Downloading Rekordbox %1…").arg(supportedText()));
    const QString target = QDir(folder_).filePath(url.section('/', -1).section('?', 0, 0));
    auto *file = new QFile(target, this);
    if (!file->open(QIODevice::WriteOnly)) { finish(false, QStringLiteral("The download could not be saved")); return; }
    auto *digest = new QCryptographicHash(QCryptographicHash::Sha256);
    QNetworkReply *reply = net_->get(request(url));
    connect(reply, &QNetworkReply::readyRead, this, [reply, file, digest] { const QByteArray b = reply->readAll(); digest->addData(b); file->write(b); });
    connect(reply, &QNetworkReply::downloadProgress, this, [this](qint64 done, qint64 total) { emit progress(total > 0 ? double(done) / total : -1); });
    const bool fromMirror = url == QString::fromLatin1(kMirror);
    connect(reply, &QNetworkReply::finished, this, [this, reply, file, digest, sha256, target, fromMirror] {
        reply->deleteLater();
        const QByteArray b = reply->readAll();
        digest->addData(b);
        file->write(b);
        file->close();
        file->deleteLater();
        const QString got = QString::fromLatin1(digest->result().toHex());
        delete digest;
        if (reply->error() != QNetworkReply::NoError) {
            if (fromMirror && !triedSite_) { triedSite_ = true; find(); return; }  // the hosted copy is unreachable: rekordbox.com
            finish(false, reply->errorString());
        }
        else if (!servedOfficially(reply->url().toString())) finish(false, QStringLiteral("The download was redirected away from rekordbox.com"));
        else if (!sha256.isEmpty() && got != sha256) finish(false, QStringLiteral("The Rekordbox download did not match its checksum"));
        else installFrom(target);
    });
}

void Setup::installFrom(const QString &archive) {
    emit status(QStringLiteral("Checking the installer…"));
    emit progress(-1);
    const QString folder = folder_;
    auto *watcher = new QFutureWatcher<QString>(this);
    connect(watcher, &QFutureWatcherBase::finished, this, [this, watcher] {
        watcher->deleteLater();
        const QString why = watcher->result();
        if (!why.isEmpty()) { finish(false, why); return; }
#ifdef Q_OS_WIN
        if (!supportedInstall()) { finish(false, QStringLiteral("Rekordbox %1 was not installed").arg(supportedText())); return; }
#endif
        finish(true, QStringLiteral("Rekordbox %1 is installed").arg(supportedText()));
    });
    watcher->setFuture(QtConcurrent::run([this, archive, folder]() -> QString {
        QString error;
        const QString installer = installerIn(archive, folder, &error);
        if (installer.isEmpty()) return error;
        QMetaObject::invokeMethod(this, [this] { emit status(QStringLiteral("Installing Rekordbox %1: answer its installer…").arg(supportedText())); });
        return runInstaller(installer);
    }));
}

}  // namespace wave::host
