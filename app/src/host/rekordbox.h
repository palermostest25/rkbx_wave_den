// Rekordbox on this computer: which versions are installed, the running one, and starting, connecting,
// showing and quitting it. Wave never changes Rekordbox on disk: its bridge is an OpenSSL provider that
// Rekordbox loads itself when started with OPENSSL_CONF pointing at Wave's config, or (Windows) is loaded
// into a running Rekordbox as Windows loads any library.
#pragma once

#include <QList>
#include <QString>
#include <QStringList>

#include <cstdint>
#include <optional>
#include <utility>
#include <vector>

namespace wave::host {

constexpr int kSupported[3] = {7, 2, 19};  // the version Wave's bridge was checked against
QString supportedText();                   // "7.2.19"

struct Install {
    QList<int> version;
    QString executable;
    QString text() const;
    bool supported() const;
};

QList<int> versionOf(const QString &text);
std::vector<Install> installed(const QString &root = {});  // every Rekordbox found, oldest first
std::optional<Install> supportedInstall();
QString summary();                                         // one line for Settings

// Closes Rekordbox's update manager (Windows: "Upmgr rekordbox.exe") wherever it runs: Wave keeps Rekordbox at the
// version its bridge supports, so updates are never offered. Returns how many were closed.
int stopUpdater();

// The running Rekordbox, 0 for none.
uint32_t rekordboxPid();
std::optional<double> processAge(uint32_t pid);            // seconds since it started
QString processExecutable(uint32_t pid);
QStringList bridgeModules(uint32_t pid);                   // Windows: Wave bridges loaded in it

// The bridge: its library, embedded in Wave, put where Rekordbox may read it, named by its hash (a
// running Rekordbox maps it, so it is never overwritten), and the OpenSSL config that loads it.
QString bridgeName();                                      // wave-bridge-<hash>.dll / .dylib
QString installBridge(QString *error = nullptr);           // the config's path, "" on failure
bool staleBridge(uint32_t pid);                            // it runs a bridge from another Wave build

// Starting and stopping. Each blocks briefly; call off the UI thread. They return "" or what went wrong.
QString launch(bool hidden, const QString &executable = {});
QString restart(bool hidden);                               // asks Rekordbox to quit, then starts it with the bridge
QString attach(uint32_t pid, QString *message);             // Windows: connect the bridge to a running Rekordbox
void askToQuit(uint32_t pid);                               // as its own close button: it asks if a deck plays

// Windows: Rekordbox's windows to the front (for its own screens) and back out of sight.
QString show(bool maximize = false);
void hide();

}  // namespace wave::host
