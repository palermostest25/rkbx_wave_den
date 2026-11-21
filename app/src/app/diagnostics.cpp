#include "app/diagnostics.h"

#include <functional>

namespace wave::diagnostics {

namespace {
constexpr int kLineMax = 400;  // Rekordbox's window lists run long: the start of each line is what matters

QString fence(const QString &text, const QString &kind = {}) { return QStringLiteral("```%1\n%2\n```").arg(kind, text); }

QString compose(const QString &header, const QString &settings, const QString &log, bool withSettings) {
    QString out = header + QStringLiteral("\n\n");
    if (withSettings && !settings.isEmpty())
        out += QStringLiteral("<details><summary>Settings</summary>\n\n") + fence(settings, QStringLiteral("json")) + QStringLiteral("\n</details>\n\n");
    return out + QStringLiteral("Log (latest last):\n\n") + fence(log);
}

int encodedLength(const QString &text) { return int(QUrl::toPercentEncoding(text).size()); }

// The newest lines of the log, oldest first, as many as `fits` allows.
QString tail(const QStringList &lines, const std::function<bool(const QString &)> &fits) {
    QStringList kept;
    for (int i = int(lines.size()) - 1; i >= 0; --i) {
        QString line = lines[i];
        if (line.size() > kLineMax) line = line.left(kLineMax) + QStringLiteral("…");
        kept.prepend(line);
        if (!fits(kept.join('\n'))) { kept.removeFirst(); break; }
    }
    return kept.join('\n');
}
}  // namespace

QString redact(QString text, const QStringList &secrets, const QString &home) {
    if (home.size() > 3) {
        text.replace(home, QStringLiteral("~"), Qt::CaseInsensitive);
        QString back = home;
        text.replace(back.replace('/', '\\'), QStringLiteral("~"), Qt::CaseInsensitive);
    }
    for (const QString &s : secrets)
        if (s.trimmed().size() >= 3) text.replace(s, QStringLiteral("<hidden>"), Qt::CaseInsensitive);
    return text;
}

Report build(const QString &log, const QString &header, const QString &settings, int urlBudget, int fullBudget) {
    Report r;
    r.title = header.section('\n', 0, 0).remove(QStringLiteral("**")).left(120);
    QStringList lines = log.split('\n');
    while (!lines.isEmpty() && lines.last().trimmed().isEmpty()) lines.removeLast();
    // The page: the header and the newest lines that fit in its address (the settings only on the clipboard).
    r.body = compose(header, settings, tail(lines, [&](const QString &t) { return encodedLength(compose(header, settings, t, false)) <= urlBudget; }), false);
    r.body += QStringLiteral("\n\n_A longer log is on the clipboard: paste it below to add it._");
    // The clipboard: the settings and far more of the log.
    r.full = compose(header, settings, tail(lines, [&](const QString &t) { return compose(header, settings, t, true).size() <= fullBudget; }), true);
    return r;
}

QUrl issueUrl(const QString &repo, const Report &report) {
    // Encoded by hand, once: every character of the text survives as typed.
    const QByteArray query = "title=" + QUrl::toPercentEncoding(report.title) + "&body=" + QUrl::toPercentEncoding(report.body) +
                             "&labels=diagnostics";
    return QUrl::fromEncoded(repo.toUtf8() + "/issues/new?" + query, QUrl::StrictMode);
}

}  // namespace wave::diagnostics
