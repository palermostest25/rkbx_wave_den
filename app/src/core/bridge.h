// Wave's bridge inside Rekordbox shares the engine's state through one private file; Wave reads it and
// writes requests (load a track, key lock, Rekordbox's own browser window). The layout is
// native/mac_bridge/bridge_layout.h, shared with both bridges, so it is included rather than copied.
#pragma once

#include <QFile>
#include <QString>

#include <array>
#include <cstdint>
#include <optional>
#include <utility>
#include <vector>

namespace wave::bridge {

enum Status : uint32_t { Starting = 0, Live = 1, Unsupported = 2, Failed = 3 };

struct Deck {
    bool loaded = false, playing = false, master = false, masterTempo = false, sync = false, slip = false, keySync = false;
    double position = 0, duration = 0;   // seconds
    double bpm = 0, originalBpm = 0;
    std::optional<std::pair<double, double>> loop;  // seconds, while a loop runs
    std::optional<double> loopIn, loopOut;          // seconds: Rekordbox's loop points as they stand, running or not
    QString path;
    int keyShift = 0;
    int tempoRange = -1;                 // Rekordbox's percentage: 6, 10, 16, 100 (WIDE)
};

struct Browser {
    int rows = -1, cursor = -1, first = 0;
    QString focus;                       // "list", "tree" or ""
    QString treePath;
    std::vector<std::pair<uint32_t, QString>> tracks;  // (id, file) of the rows around the cursor
    std::vector<std::pair<QString, QString>> texts;    // (title, artist) of the same rows, as Rekordbox shows them
};

struct Snapshot {
    uint32_t pid = 0, status = Starting;
    QString note;
    uint64_t ticks = 0, uptimeNs = 0;
    std::vector<Deck> decks;
    std::vector<std::pair<int64_t, QString>> devices;  // (handle, root) of each mounted stick
    std::optional<Browser> browser;
    bool older = false;                  // an earlier Wave's layout: never misread
};

// The shared file as bytes: one consistent snapshot, or none while the bridge is mid-write.
std::optional<Snapshot> parse(const uchar *data, qint64 size);

uint64_t uptimeNs();                     // the bridge's clock
uint64_t fnv(const uchar *data, size_t n);
bool live(const std::optional<Snapshot> &snapshot, uint32_t pid, uint64_t now = uptimeNs());
QString defaultFolder();                 // where the bridge keeps state.bin

enum Command : uint32_t { Load = 1, MasterTempo = 2, LoadDevice = 3, ShowBrowser = 4, LoadRow = 5, Search = 6, Rekordbox = 7 };

class Client {
public:
    explicit Client(const QString &path);
    ~Client();
    bool open(QString *error = nullptr);
    std::optional<Snapshot> snapshot() const;
    bool older() const { return older_; }  // open failed on an earlier Wave's layout
    // Asks the bridge; (accepted, Rekordbox's answer). Waits up to ``timeoutMs`` for the answer.
    std::pair<bool, QString> request(Command kind, int deck, uint32_t value, int64_t device = 0, int timeoutMs = 2500);
    // Rekordbox's search box: the query, as if typed there and Return pressed.
    std::pair<bool, QString> search(const QString &query);
    void listFrom(int first);  // the rows of Rekordbox's list Wave reads from now on: from ``first``, or -1 its cursor's

private:
    QFile file_;
    uchar *map_ = nullptr;
    bool older_ = false;
};

// The controller as Rekordbox hears it (Windows): the bridge passes on every message Rekordbox's MIDI input
// receives, and keeps the controls Wave names from Rekordbox while Wave's heartbeat is fresh.
class Tap {
public:
    explicit Tap(const QString &path);
    ~Tap();
    std::vector<std::array<int, 3>> take();  // messages since the last call: (status, data1, data2)
    bool hooked() const { return hooked_; }  // a live bridge hears Rekordbox's controller input
    // Keeps these (status, data1) from Rekordbox, and beats: call at least twice a second. {} keeps nothing.
    void keep(const std::vector<std::pair<int, int>> &controls);
    uint32_t kept() const;                   // messages kept from Rekordbox so far

private:
    bool ensure();
    QFile file_;
    uchar *map_ = nullptr;
    uint32_t pid_ = 0, next_ = 0;
    uint64_t retryAt_ = 0;
    bool hooked_ = false;
    std::vector<uint16_t> keeping_;
};

}  // namespace wave::bridge
