#include "core/keys.h"

#include <QMutex>
#include <QRegularExpression>

namespace wave {
namespace {

QMutex lock;
QString chosen = QStringLiteral("rekordbox");

// The wheel: number n, minor (A) and major (B), as Rekordbox spells them in Classic.
const char *const kMinor[12] = {"Abm", "Ebm", "Bbm", "Fm", "Cm", "Gm", "Dm", "Am", "Em", "Bm", "F#m", "Dbm"};
const char *const kMajor[12] = {"B", "F#", "Db", "Ab", "Eb", "Bb", "F", "C", "G", "D", "A", "E"};

int semitone(QChar letter, QChar accidental) {
    static const int base[7] = {9, 11, 0, 2, 4, 5, 7};  // A B C D E F G
    int s = base[letter.toUpper().unicode() - 'A'];
    if (accidental == '#' || accidental == QChar(0x266F)) ++s;
    if (accidental == 'b' || accidental == QChar(0x266D)) --s;
    return (s + 12) % 12;
}

// Camelot number (1..12) of a key from its root semitone: minor keys from A minor = 8, majors from C major = 8.
int wheel(int root, bool minor) {
    const int fifths = (((root - (minor ? 9 : 0)) * 7) % 12 + 12) % 12;  // fifths above A minor / C major (both 8)
    return (fifths + 7) % 12 + 1;
}

}  // namespace

void setKeyNotation(const QString &notation) {
    QMutexLocker l(&lock);
    chosen = notation.isEmpty() ? QStringLiteral("rekordbox") : notation;
}

QString keyNotation() {
    QMutexLocker l(&lock);
    return chosen;
}

QString keyText(const QString &key) { return keyText(key, keyNotation()); }

QString keyText(const QString &key, const QString &notation) {
    if (notation != QLatin1String("alphanumeric") && notation != QLatin1String("classic")) return key;
    const QString k = key.trimmed();
    int number = 0;
    bool minor = false;
    static const QRegularExpression camelot(QStringLiteral("^(1[0-2]|[1-9])\\s*([ABab])$"));
    static const QRegularExpression musical(QStringLiteral("^([A-Ga-g])([#b\\x{266F}\\x{266D}]?)\\s*(m|min|minor|maj|major|M)?$"));
    if (const auto m = camelot.match(k); m.hasMatch()) {
        number = m.captured(1).toInt();
        minor = m.captured(2).toUpper() == QLatin1String("A");
    } else if (const auto m = musical.match(k); m.hasMatch()) {
        const QString quality = m.captured(3);
        minor = quality == QLatin1String("m") || quality.startsWith(QLatin1String("min"));
        const QString acc = m.captured(2);
        number = wheel(semitone(m.captured(1).at(0), acc.isEmpty() ? QChar() : acc.at(0)), minor);
    } else {
        return key;
    }
    if (notation == QLatin1String("alphanumeric")) return QStringLiteral("%1%2").arg(number).arg(minor ? 'A' : 'B');
    return QString::fromLatin1(minor ? kMinor[number - 1] : kMajor[number - 1]);
}

}  // namespace wave
