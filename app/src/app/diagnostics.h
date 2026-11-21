#pragma once
// Wave's diagnostics as a GitHub issue: no token, no account in Wave. The issue page opens in the browser filled in
// with what fits in its address (the latest of the log); a longer copy goes on the clipboard. The repository is
// public, so the Windows user name, computer name and home folder are blanked out first.

#include <QString>
#include <QStringList>
#include <QUrl>

namespace wave::diagnostics {

struct Report {
    QString title;
    QString body;   // what the issue page is filled in with (fits a URL)
    QString full;   // the longer copy, for the clipboard (fits an issue: GitHub takes 65536 characters)
};

// Names that identify the person or the computer, replaced in everything sent.
QString redact(QString text, const QStringList &secrets, const QString &home);

// The report from the log's text (oldest first), a header (versions) and the settings (JSON), within the limits.
Report build(const QString &log, const QString &header, const QString &settings, int urlBudget = 6500, int fullBudget = 60000);

QUrl issueUrl(const QString &repo, const Report &report);

}  // namespace wave::diagnostics
