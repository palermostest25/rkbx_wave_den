#include "core/bridge.h"

#include "../../../native/mac_bridge/bridge_layout.h"

#include <QDir>
#include <QFileInfo>
#include <QStandardPaths>
#include <QThread>
#include <QElapsedTimer>

#include <atomic>
#include <cstring>

#ifdef _WIN32
#include <windows.h>
#else
#include <ctime>
#include <unistd.h>
#endif

namespace wave::bridge {
namespace {

QString text(const char *raw, size_t size) {
    return QString::fromUtf8(raw, int(strnlen(raw, size)));
}

uint32_t seqOf(const uchar *data) {
    return reinterpret_cast<const std::atomic<uint32_t> *>(data + offsetof(BridgeState, seq))->load(std::memory_order_acquire);
}

}  // namespace

uint64_t fnv(const uchar *data, size_t n) {
    uint64_t h = 0xcbf29ce484222325ULL;
    for (size_t i = 0; i < n; ++i) h = (h ^ data[i]) * 0x100000001b3ULL;
    return h;
}

uint64_t uptimeNs() {
#ifdef _WIN32
    LARGE_INTEGER counter, frequency;
    QueryPerformanceCounter(&counter);
    QueryPerformanceFrequency(&frequency);
    return uint64_t(counter.QuadPart / frequency.QuadPart) * 1000000000ULL +
           uint64_t(counter.QuadPart % frequency.QuadPart) * 1000000000ULL / uint64_t(frequency.QuadPart);
#elif defined(__APPLE__)
    return clock_gettime_nsec_np(CLOCK_UPTIME_RAW);
#else
    timespec ts{};
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return uint64_t(ts.tv_sec) * 1000000000ULL + uint64_t(ts.tv_nsec);
#endif
}

std::optional<Snapshot> parse(const uchar *data, qint64 size) {
    if (!data || size < qint64(sizeof(BridgeState))) return std::nullopt;
    const auto *state = reinterpret_cast<const BridgeState *>(data);
    for (int attempt = 0; attempt < 8; ++attempt) {
        if (std::memcmp(state->magic, kBridgeMagic, sizeof kBridgeMagic)) return std::nullopt;
        const uint32_t seq = seqOf(data);
        if (state->version != kBridgeVersion) {
            Snapshot older;
            older.pid = state->pid;
            older.status = Unsupported;
            older.older = true;
            older.note = QStringLiteral("Rekordbox has an older Wave bridge loaded: press CONNECT to restart it");
            return older;
        }
        if (seq & 1) continue;  // mid-write
        Snapshot s;
        s.pid = state->pid;
        s.status = state->status;
        s.note = text(state->note, sizeof state->note);
        s.ticks = state->ticks;
        s.uptimeNs = state->uptime_ns;
        for (uint32_t i = 0; i < std::min(state->deck_count, kBridgeDecks); ++i) {
            const BridgeDeck &d = state->decks[i];
            Deck deck;
            deck.loaded = d.flags & kDeckLoaded;
            deck.playing = d.flags & kDeckPlaying;
            deck.position = d.position_ms / 1000;
            deck.duration = d.total_ms / 1000.0;
            deck.bpm = d.bpm / 100.0;
            deck.originalBpm = d.original_bpm / 100.0;
            if (d.flags & kDeckLooping) deck.loop = std::make_pair(d.loop_in / 1000.0, d.loop_out / 1000.0);
            if (deck.loaded) {
                deck.path = text(d.path, sizeof d.path);
                deck.keyShift = d.key_shift;
                deck.tempoRange = d.tempo_range;
            }
            deck.master = deck.loaded && state->master == int32_t(i);
            deck.masterTempo = d.flags & kDeckMasterTempo;
            deck.sync = d.flags & kDeckSync;
            deck.slip = d.flags & kDeckSlip;
            deck.keySync = d.flags & kDeckKeySync;
            s.decks.push_back(deck);
        }
        for (const BridgeDevice &device : state->devices)
            if (device.handle) s.devices.emplace_back(device.handle, text(device.root, sizeof device.root));
        Browser b;
        const BridgeBrowser &raw = state->browser;
        b.rows = raw.rows; b.cursor = raw.cursor; b.first = raw.first;
        b.focus = raw.focus == 0 ? QStringLiteral("list") : raw.focus == 1 ? QStringLiteral("tree") : QString();
        b.treePath = text(raw.tree_path, sizeof raw.tree_path);
        for (int i = 0; i < std::min<int>(raw.count, kBrowseRows); ++i)
            b.tracks.emplace_back(raw.rows_[i].id, text(raw.rows_[i].path, sizeof raw.rows_[i].path));
        s.browser = b;
        if (seqOf(data) == seq) return s;  // nothing changed underneath: consistent
    }
    return std::nullopt;
}

bool live(const std::optional<Snapshot> &snapshot, uint32_t pid, uint64_t now) {
    return snapshot && pid && snapshot->pid == pid && snapshot->status == Live && now - snapshot->uptimeNs < 1000000000ULL;
}

QString defaultFolder() {
#ifdef _WIN32
    return QDir(qEnvironmentVariable("LOCALAPPDATA", QDir::homePath() + "/AppData/Local")).filePath("rkbx_wave/bridge");
#else
    return QDir::home().filePath("Library/Application Support/rkbx_wave/bridge");
#endif
}

Client::Client(const QString &path) : file_(path) {}
Client::~Client() {
    if (map_) file_.unmap(map_);
}

bool Client::open(QString *error) {
    if (!file_.open(QIODevice::ReadWrite)) {
        if (error) *error = file_.errorString();
        return false;
    }
#ifndef _WIN32
    // Only ever our own private file (on Windows it sits in the user's own LOCALAPPDATA).
    const auto perms = file_.permissions();
    if (QFileInfo(file_).ownerId() != uint(getuid()) ||
        perms & (QFileDevice::ReadGroup | QFileDevice::WriteGroup | QFileDevice::ReadOther | QFileDevice::WriteOther)) {
        if (error) *error = QStringLiteral("Wave bridge state file is not private");
        return false;
    }
#endif
    if (file_.size() < qint64(sizeof(BridgeState))) {
        // Mapping past a short file would fault on read: an earlier layout is reported instead.
        const QByteArray header = file_.peek(12);
        uint32_t version = 0;
        if (header.size() == 12) std::memcpy(&version, header.constData() + 8, 4);
        older_ = header.startsWith(QByteArray(kBridgeMagic, 8)) && version != kBridgeVersion;
        if (error) *error = older_ ? QStringLiteral("Rekordbox has an older Wave bridge loaded: press CONNECT to restart it")
                                   : QStringLiteral("Wave bridge state file is not complete");
        return false;
    }
    map_ = file_.map(0, sizeof(BridgeState));
    if (!map_ && error) *error = file_.errorString();
    return map_ != nullptr;
}

std::optional<Snapshot> Client::snapshot() const {
    return parse(map_, map_ ? qint64(sizeof(BridgeState)) : 0);
}

std::pair<bool, QString> Client::request(Command kind, int deck, uint32_t value, int64_t device, int timeoutMs) {
    if (!map_) return {false, QStringLiteral("Rekordbox is not connected")};
    if (deck < 0 || deck >= int(kBridgeDecks)) return {false, QStringLiteral("No such deck")};
    auto *state = reinterpret_cast<BridgeState *>(map_);
    BridgeCommand c{};
    c.seq = (state->command.seq + 1) ? state->command.seq + 1 : 1;
    c.kind = kind;
    c.deck = uint32_t(deck);
    c.content_id = value;
    c.issued_ns = uptimeNs();
    c.device = device;
    uchar bytes[32];
    std::memcpy(bytes, &c.seq, 4); std::memcpy(bytes + 4, &c.kind, 4); std::memcpy(bytes + 8, &c.deck, 4);
    std::memcpy(bytes + 12, &c.content_id, 4); std::memcpy(bytes + 16, &c.issued_ns, 8); std::memcpy(bytes + 24, &c.device, 8);
    c.check = fnv(bytes, sizeof bytes);
    std::memcpy(&state->command, &c, sizeof c);
    QElapsedTimer waited;
    waited.start();
    while (waited.elapsed() < timeoutMs) {
        std::atomic_thread_fence(std::memory_order_acquire);
        if (state->reply.seq == c.seq)
            return {state->reply.result == kReplyRequested, text(state->reply.message, sizeof state->reply.message)};
        QThread::msleep(5);
    }
    return {false, QStringLiteral("Rekordbox did not answer; is it still running?")};
}

}  // namespace wave::bridge
