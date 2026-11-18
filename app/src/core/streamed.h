// Rekordbox keeps a streamed track's metadata (Beatport, Spotify, TIDAL...) encrypted in its library: titles and
// artists read "$A7:v1:<IV>:<ciphertext>", both Base64, AES-128-CBC with PKCS#7 padding. The key is the same in every
// Rekordbox 7.2.19: Rekordbox derives it from two strings built into rekordbox.exe (a JUCE BlowFish keyed with
// "AlphaTheta corporation" decrypts a JUCE-Base64 blob to 32 hex digits), so Wave keeps the derived key.
#pragma once

#include <QByteArray>
#include <QString>

namespace wave {

bool isStreamedText(const QString &text);  // "$A7:v1:..." (or another version of it)
// The plain text of an encrypted value; anything else comes back as it is. "" when it can't be read.
QString revealStreamed(const QString &text);
QByteArray aes128CbcDecrypt(const QByteArray &key, const QByteArray &iv, const QByteArray &data);  // "" if malformed

}  // namespace wave
