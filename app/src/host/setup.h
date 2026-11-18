// Installing the Rekordbox Wave supports, when it is missing. Rekordbox is AlphaTheta's: Wave never ships
// it. It gets AlphaTheta's own installer - found on rekordbox.com, or a file the DJ downloaded - checks that
// it is signed by AlphaTheta (or Pioneer DJ) and runs it. On Windows each version installs in a folder of
// its own, so the supported one sits beside a newer one, and Wave starts the one it supports.
#pragma once

#include <QObject>
#include <QString>

#include <functional>

class QNetworkAccessManager;

namespace wave::host {

extern const char *const kDownloadPage;
QString installerName();                          // Install_rekordbox_x64_7_2_19.zip / Install_rekordbox_7_2_19.pkg.zip
QString downloadFromPage(const QString &html);    // AlphaTheta's link to it on rekordbox.com, or ""
bool trustedSigner(const QString &subject);

class Setup : public QObject {
    Q_OBJECT
public:
    explicit Setup(QObject *parent = nullptr);
    // From rekordbox.com, or from ``archive`` (a file the DJ chose). Answers with status(), progress(), done().
    void run(const QString &archive = {});
    bool busy() const { return busy_; }

signals:
    void status(const QString &text);
    void progress(double fraction);               // -1 while it can't tell
    void done(bool installed, const QString &message);

private:
    void find();
    void download(const QString &url, const QString &sha256);
    void installFrom(const QString &archive);
    void finish(bool ok, const QString &message);
    QNetworkAccessManager *net_;
    QString folder_;
    bool busy_ = false;
};

}  // namespace wave::host
