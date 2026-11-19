#include "core/bridge.h"

#include "../../../native/mac_bridge/bridge_layout.h"

#include <QDir>
#include <QFileInfo>
#include <QStandardPaths>
#include <QThread>
#include <QElapsedTimer>

#include <algorithm>
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
        for (int i = 0; i < std::min<int>(raw.count, kBrowseRows); ++i) {
            b.tracks.emplace_back(raw.rows_[i].id, text(raw.rows_[i].path, sizeof raw.rows_[i].path));
            const BrowseText &t = state->texts[i];
            b.texts.emplace_back(text(t.title, sizeof t.title), text(t.artist, sizeof t.artist));
        }
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

std::pair<bool, QString> Client::search(const QString &query) {
    if (!map_) return {false, QStringLiteral("Rekordbox is not connected")};
    auto *state = reinterpret_cast<BridgeState *>(map_);
    QByteArray utf8 = query.trimmed().toUtf8().left(int(sizeof state->search.query) - 1);
    while (!utf8.isEmpty() && (uchar(utf8.back()) & 0xC0) == 0x80) utf8.chop(1);  // never half a character
    std::memset(state->search.query, 0, sizeof state->search.query);
    std::memcpy(state->search.query, utf8.constData(), size_t(utf8.size()));
    uint32_t check = 0x811c9dc5u;
    for (const char c : utf8) check = (check ^ uchar(c)) * 0x01000193u;
    return request(Search, 0, check);
}

void Client::listFrom(int first) {
    if (map_) reinterpret_cast<std::atomic<int32_t> *>(&reinterpret_cast<BridgeState *>(map_)->search.first)->store(first);
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

Tap::Tap(const QString &path) : file_(path) {}

Tap::~Tap() {
    if (map_) {
        auto *m = &reinterpret_cast<BridgeState *>(map_)->midi;
        reinterpret_cast<std::atomic<uint32_t> *>(&m->keep_count)->store(0, std::memory_order_release);
        reinterpret_cast<std::atomic<uint64_t> *>(&m->wave_ns)->store(0, std::memory_order_release);
        file_.unmap(map_);
    }
}

bool Tap::ensure() {
    if (map_) {
        const auto *state = reinterpret_cast<const BridgeState *>(map_);
        if (!std::memcmp(state->magic, kBridgeMagic, sizeof kBridgeMagic) && state->version == kBridgeVersion) return true;
        file_.unmap(map_);  // another layout: look again later
        map_ = nullptr;
        file_.close();
    }
    const uint64_t now = uptimeNs();
    if (now < retryAt_) return false;
    retryAt_ = now + 1000000000ULL;
    if (!file_.isOpen() && !file_.open(QIODevice::ReadWrite)) return false;
    if (file_.size() < qint64(sizeof(BridgeState))) { file_.close(); return false; }
    map_ = file_.map(0, sizeof(BridgeState));
    if (!map_) { file_.close(); return false; }
    const auto *state = reinterpret_cast<const BridgeState *>(map_);
    if (std::memcmp(state->magic, kBridgeMagic, sizeof kBridgeMagic) || state->version != kBridgeVersion) {
        file_.unmap(map_);
        map_ = nullptr;
        file_.close();
        return false;
    }
    next_ = 0;
    return true;
}

std::vector<std::array<int, 3>> Tap::take() {
    std::vector<std::array<int, 3>> out;
    hooked_ = false;
    if (!ensure()) return out;
    auto *state = reinterpret_cast<BridgeState *>(map_);
    const uint64_t beat = reinterpret_cast<std::atomic<uint64_t> *>(&state->uptime_ns)->load(std::memory_order_acquire);
    const uint64_t now = uptimeNs();
    const BridgeMidi &m = state->midi;
    // Rekordbox's heartbeat can be late by a second or more while it is minimised (Windows slows its timers): only a
    // long silence means the bridge is gone.
    hooked_ = reinterpret_cast<const std::atomic<uint32_t> *>(&m.hooked)->load(std::memory_order_acquire) == 1 &&
              state->status == kStatusLive && beat && now >= beat && now - beat < 5000000000ULL;
    if (state->pid != pid_) { pid_ = state->pid; next_ = 0; }  // another Rekordbox: its own count
    if (!hooked_) return out;
    const uint32_t written = reinterpret_cast<const std::atomic<uint32_t> *>(&m.written)->load(std::memory_order_acquire);
    if (!next_ || written + 1 < next_) { next_ = written + 1; return out; }  // from now on, not what came before
    if (written >= next_ + kMidiEvents) next_ = written + 1 - kMidiEvents;  // fell behind: the oldest are gone
    for (uint32_t i = 0; i < kMidiEvents && next_ <= written; ++i) {
        const BridgeMidiEvent &e = m.events[(next_ - 1) % kMidiEvents];
        const auto *seq = reinterpret_cast<const std::atomic<uint32_t> *>(&e.seq);
        if (seq->load(std::memory_order_acquire) != next_) break;  // still being written
        const std::array<int, 3> message{e.status, e.data1, e.data2};
        std::atomic_thread_fence(std::memory_order_acquire);
        if (seq->load(std::memory_order_acquire) != next_) break;  // overwritten while read
        out.push_back(message);
        ++next_;
    }
    return out;
}

void Tap::keep(const std::vector<std::pair<int, int>> &controls) {
    if (!map_) return;
    auto &m = reinterpret_cast<BridgeState *>(map_)->midi;
    std::vector<uint16_t> keys;
    for (const auto &[status, data1] : controls) {
        const int s = (status & 0xF0) == 0x80 ? 0x90 | (status & 0x0F) : status;
        const auto key = uint16_t(s << 8 | (data1 & 0x7F));
        if (std::find(keys.begin(), keys.end(), key) == keys.end() && keys.size() < kMidiKeeps) keys.push_back(key);
    }
    auto *count = reinterpret_cast<std::atomic<uint32_t> *>(&m.keep_count);
    if (keys != keeping_) {
        count->store(0, std::memory_order_release);  // nothing half-written is ever kept
        for (size_t i = 0; i < keys.size(); ++i) m.keep[i] = keys[i];
        count->store(uint32_t(keys.size()), std::memory_order_release);
        keeping_ = keys;
    }
    reinterpret_cast<std::atomic<uint64_t> *>(&m.wave_ns)->store(keys.empty() ? 0 : uptimeNs(), std::memory_order_release);
}

uint32_t Tap::kept() const {
    return map_ ? reinterpret_cast<const std::atomic<uint32_t> *>(&reinterpret_cast<const BridgeState *>(map_)->midi.kept)->load() : 0;
}

}  // namespace wave::bridge
