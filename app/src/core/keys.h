// Musical keys as the DJ wants them read: as Rekordbox stored them, Alphanumeric (Camelot-style, 8A), or Classic (Am).
#pragma once

#include <QString>

namespace wave {

// "rekordbox" (as stored), "alphanumeric" or "classic". Set once from Settings; read wherever a key is shown.
void setKeyNotation(const QString &notation);
QString keyNotation();

// The key in the chosen notation; text it can't read as a key comes back as it is.
QString keyText(const QString &key);
QString keyText(const QString &key, const QString &notation);

}  // namespace wave
