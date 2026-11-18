// Wave's bridge inside Rekordbox shares the engine's state through one private file; Wave reads it and
// writes requests (load a track, key lock, Rekordbox's own browser window). The layout is
// native/mac_bridge/bridge_layout.h, shared with both bridges, so it is included rather than copied.
#pragma once

#include <QFile>
#include <QString>

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
    QString path;
    int keyShift = 0;
    int tempoRange = -1;                 // Rekordbox's percentage: 6, 10, 16, 100 (WIDE)
};

struct Browser {
    int rows = -1, cursor = -1, first = 0;
    QString focus;                       // "list", "tree" or ""
    QString treePath;
    std::vector<std::pair<uint32_t, QString>> tracks;  // (id, file) of the rows around the cursor
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

enum Command : uint32_t { Load = 1, MasterTempo = 2, LoadDevice = 3, ShowBrowser = 4 };

class Client {
public:
    explicit Client(const QString &path);
    ~Client();
    bool open(QString *error = nullptr);
    std::optional<Snapshot> snapshot() const;
    bool older() const { return older_; }  // open failed on an earlier Wave's layout
    // Asks the bridge; (accepted, Rekordbox's answer). Waits up to ``timeoutMs`` for the answer.
    std::pair<bool, QString> request(Command kind, int deck, uint32_t value, int64_t device = 0, int timeoutMs = 2500);

private:
    QFile file_;
    uchar *map_ = nullptr;
    bool older_ = false;
};

}  // namespace wave::bridge
