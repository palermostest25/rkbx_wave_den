// Wave updates itself from its GitHub releases: it finds the latest release, downloads this computer's
// file, checks it against the SHA-256 GitHub publishes for it, swaps it in and restarts. Where Wave is
// installed in Program Files, Windows asks once for permission to swap it.
#pragma once

#include <QObject>
#include <QString>

#include <functional>
#include <optional>

class QNetworkAccessManager;

namespace wave {

QString version();                       // this Wave's
QList<int> versionNumbers(const QString &text);  // "v3.0.1" -> 3, 0, 1; empty when it isn't one
QString assetName();                     // this computer's file in a release

struct Update {
    QString version, url, sha256, page;
    qint64 size = 0;
};

// Pure: the newer release for this computer in GitHub's answer, if any.
std::optional<Update> newerIn(const QByteArray &releaseJson, const QString &current, const QString &asset);

class Updater : public QObject {
    Q_OBJECT
public:
    explicit Updater(QObject *parent = nullptr);
    bool canInstall() const;             // a packaged Wave replaces itself; a development build doesn't
    void check();                        // answers with checked()
    void install(const Update &update);  // answers with progress(), then failed() or ready()
    static void cleanUp();               // what an earlier update left beside Wave

signals:
    void checked(std::optional<wave::Update> update, const QString &error);
    void progress(int percent);
    void failed(const QString &why);
    void ready();                        // the new Wave starts once this one quits: quit now

private:
    QString swapIn(const QString &file);
    QNetworkAccessManager *net_;
};

}  // namespace wave

Q_DECLARE_METATYPE(std::optional<wave::Update>)
