#include "app/reader.h"

#include "core/clock.h"
#include "core/library.h"
#include "host/rekordbox.h"

#include <QDir>
#include <QFileInfo>

#include <chrono>
#include <thread>

namespace wave {
namespace {

bool sameDecks(const std::vector<bridge::Deck> &a, const std::vector<bridge::Deck> &b) {
    if (a.size() != b.size()) return false;
    for (size_t i = 0; i < a.size(); ++i) {
        const auto &x = a[i], &y = b[i];
        if (x.loaded != y.loaded || x.playing != y.playing || x.position != y.position || x.bpm != y.bpm ||
            x.originalBpm != y.originalBpm || x.loop != y.loop || x.path != y.path || x.master != y.master ||
            x.masterTempo != y.masterTempo || x.sync != y.sync || x.keyShift != y.keyShift || x.tempoRange != y.tempoRange)
            return false;
    }
    return true;
}

bool sameBrowser(const std::optional<bridge::Browser> &a, const std::optional<bridge::Browser> &b) {
    if (a.has_value() != b.has_value()) return false;
    if (!a) return true;
    return a->rows == b->rows && a->cursor == b->cursor && a->first == b->first && a->focus == b->focus &&
           a->treePath == b->treePath && a->tracks == b->tracks;
}

}  // namespace

QString streamingService(const QString &path) {
    // How Rekordbox names a streamed file: a service's scheme (spotify:track:…) or its web address.
    static const std::pair<const char *, const char *> services[] = {
        {"spotify", "Spotify"}, {"beatport", "Beatport"}, {"beatsource", "Beatsource"}, {"tidal", "TIDAL"},
        {"soundcloud", "SoundCloud"}, {"apple", "Apple Music"}};
    const QString p = path.toLower();
    for (const auto &[key, name] : services)
        if (p.startsWith(QLatin1String(key)) || p.contains(QLatin1String(key) + QLatin1Char(':')) || p.contains(QLatin1String(key) + QLatin1String(".com")))
            return QString::fromLatin1(name);
    return {};
}

Reader::Reader(QObject *parent) : QObject(parent) { qRegisterMetaType<std::shared_ptr<const ReaderFrame>>(); }

Reader::~Reader() { stop(); }

void Reader::start() {
    if (thread_) return;
    stop_ = false;
    thread_ = QThread::create([this] { run(); });
    thread_->setObjectName("rekordbox-bridge");
    thread_->start(QThread::HighPriority);
}

void Reader::stop() {
    if (!thread_) return;
    stop_ = true;
    thread_->wait(3000);
    delete thread_;
    thread_ = nullptr;
}

void Reader::run() {
    using namespace std::chrono;
    Libraries libraries;
    std::unique_ptr<bridge::Client> client;
    double openedAt = 0, nextPid = 0, lastEmit = 0;
    uint32_t pid = 0;
    uint64_t lastTick = ~0ULL;
    bool bridged = false, stale = false, checked = false;
    int misses = 0;
    std::map<int, QString> paths;
    ReaderFrame last;
    const QString stateFile = QDir(bridge::defaultFolder()).filePath("state.bin");
    auto wait = [&](double seconds) {
        const auto until = steady_clock::now() + duration<double>(seconds);
        while (!stop_ && steady_clock::now() < until) std::this_thread::sleep_for(std::min(duration<double>(.02), duration<double>(until - steady_clock::now())));
    };
    // Sends a frame when anything the UI shows changed, and at least twice a second for the status.
    auto emitFrame = [&](ReaderFrame f) {
        f.pid = pid;
        f.checked = checked;
        const double t = now();
        f.decksChanged = !sameDecks(f.decks, last.decks);
        const bool changed = f.decksChanged || !f.tracks.empty() || f.state != last.state || f.status != last.status ||
                             f.connected != last.connected || f.bridged != last.bridged || f.pid != last.pid ||
                             f.devices != last.devices || !sameBrowser(f.browser, last.browser) || f.stale != last.stale;
        if (!changed && t - lastEmit < .5) return;
        lastEmit = t;
        last = f;
        emit frame(std::make_shared<const ReaderFrame>(std::move(f)));
    };
    while (!stop_) {
        const double started = now();
        ReaderFrame f;
        if (started >= nextPid) {
            nextPid = started + 1;
            pid = host::rekordboxPid();
            checked = true;
        }
        // Not bridged: follow the state file afresh each second, as a newly started Rekordbox replaces it.
        if (client && !bridged && started - openedAt > 1) client.reset();
        bool older = false;
        if (!client && QFileInfo::exists(stateFile)) {
            auto c = std::make_unique<bridge::Client>(stateFile);
            openedAt = started;
            if (c->open()) client = std::move(c);
            else older = c->older();
        }
        std::optional<bridge::Snapshot> snapshot = client ? client->snapshot() : std::nullopt;
        if (!snapshot && older) {
            snapshot = bridge::Snapshot{};
            snapshot->pid = pid;
            snapshot->status = bridge::Unsupported;
            snapshot->older = true;
            snapshot->note = QStringLiteral("Rekordbox has an older Wave bridge loaded: press CONNECT to restart it");
        }
        if (!snapshot && bridged && ++misses < 100) {  // caught Rekordbox mid-write: read again, never disconnect
            wait(.002);
            continue;
        }
        if (snapshot) misses = 0;  // a bridge that stays unreadable (it stopped mid-write) is gone, not busy
        if (bridged && snapshot && pid && snapshot->pid == pid && snapshot->status == bridge::Live && !bridge::live(snapshot, pid)) {
            // Rekordbox's main thread is busy (a heavy load, a dialog): its bridge resumes with it, so hold
            // every deck as it is rather than start over.
            f = last;
            f.tracks.clear();
            f.state = host::State::Busy;
            f.status = kBusy;
            emitFrame(f);
            wait(.05);
            continue;
        }
        if (!bridge::live(snapshot, pid)) {
            if (bridged) {  // forget the decks
                for (auto &[deck, path] : paths) f.tracks[deck] = TrackInfo{};
                paths.clear();
            }
            bridged = stale = false;
            lastTick = ~0ULL;
            if (!pid) f.state = host::State::Absent;
            else if (snapshot && snapshot->pid == pid)
                f.state = snapshot->status == bridge::Unsupported ? (snapshot->older ? host::State::Older : host::State::Unsupported)
                        : snapshot->status == bridge::Failed     ? host::State::Failed
                        : snapshot->status == bridge::Live       ? host::State::Busy
                                                                 : host::State::Starting;
            else f.state = host::State::None;
            if (pid && snapshot && snapshot->pid == pid && (snapshot->status == bridge::Unsupported || snapshot->status == bridge::Failed))
                f.status = snapshot->note;
            else if (pid && snapshot && snapshot->pid == pid)
                f.status = snapshot->status == bridge::Live ? kBusy : kStarting;  // live and stopped: stuck, not starting
            else if (pid)
                f.status = kRelaunch;
            else
                f.status = QStringLiteral("Waiting for Rekordbox · press CONNECT to start it with Wave");
            emitFrame(f);
            wait(.25);
            continue;
        }
        if (!bridged) {
            bridged = true;
            stale = host::staleBridge(pid);
        }
        f.connected = f.bridged = true;
        f.stale = stale;
        f.state = host::State::Live;
        f.status = stale ? QStringLiteral("REKORDBOX · direct · press CONNECT to restart Rekordbox and finish updating Wave")
                         : QStringLiteral("REKORDBOX · direct");
        if (snapshot->ticks == lastTick) {
            emitFrame(last.bridged ? [&] { ReaderFrame same = last; same.tracks.clear(); same.status = f.status; same.stale = stale; return same; }() : f);
            wait(std::max(0.0, 1 / 120.0 - (now() - started)));
            continue;
        }
        lastTick = snapshot->ticks;
        f.decks = snapshot->decks;
        f.devices = snapshot->devices;
        f.browser = snapshot->browser;
        // Rekordbox's tick, not this read, is when the values were true.
        f.when = now() - std::max<int64_t>(0, int64_t(bridge::uptimeNs() - snapshot->uptimeNs)) / 1e9;
        for (int idx = 0; idx < int(f.decks.size()); ++idx) {
            const QString path = f.decks[idx].path;
            if (paths.count(idx) && paths[idx] == path) continue;
            if (!paths.count(idx) && path.isEmpty()) { paths[idx] = path; continue; }
            paths[idx] = path;
            TrackInfo info;
            info.path = path;
            if (!path.isEmpty()) {
                const QString service = streamingService(path);
                if (auto track = libraries.resolvePath(path)) {
                    info.analysisFolder = track->analysisFolder;
                    info.title = track->title;
                    info.artist = track->artist;
                    info.key = track->key;
                    info.artwork = track->artwork;
                    // Rekordbox keeps a streamed track's title encrypted: say where it streams from instead.
                    if (info.title.startsWith(QLatin1String("$A")) && info.title.contains(QLatin1String(":v"))) {
                        info.title = service.isEmpty() ? QStringLiteral("Streamed track") : QStringLiteral("%1 track").arg(service);
                        if (info.artist.startsWith(QLatin1String("$A"))) info.artist.clear();
                    }
                } else if (!service.isEmpty()) {
                    info.title = QStringLiteral("%1 track").arg(service);
                    info.error = QStringLiteral("Streaming from %1: the waveform shows once Rekordbox has analysed it").arg(service);
                } else {
                    info.error = QStringLiteral("%1 is not in the Rekordbox library or a mounted USB").arg(QFileInfo(path).fileName());
                }
            }
            f.tracks[idx] = info;
        }
        emitFrame(f);
        wait(std::max(0.0, 1 / 120.0 - (now() - started)));
    }
    libraries.close();
}

}  // namespace wave
