#include "app/session.h"

#include "core/clock.h"
#include "host/rekordbox.h"

#include <QDir>
#include <QDateTime>
#include <QFileInfo>
#include <QLoggingCategory>
#include <QTimer>
#include <set>
#include <QtConcurrent>

#include <cmath>

Q_LOGGING_CATEGORY(lcSession, "wave.session")

namespace wave {
namespace {

bool samePath(QString a, QString b) {
    a = QDir::fromNativeSeparators(a).toCaseFolded();
    b = QDir::fromNativeSeparators(b).toCaseFolded();
    return !a.isEmpty() && !b.isEmpty() && (a == b || a.endsWith(b) || b.endsWith(a));
}

// A demo track: a few minutes of shaped bands with a grid and cues, for --demo and screenshots.
std::shared_ptr<Analysis> demoAnalysis(int deck) {
    auto a = std::make_shared<Analysis>();
    const double bpm = 120 + deck * 4, seconds = 240;
    const int columns = int(seconds * 150);
    a->low.resize(columns); a->mid.resize(columns); a->high.resize(columns);
    for (int i = 0; i < columns; ++i) {
        const double t = i / 150.0, beat = std::fmod(t * bpm / 60, 1.0);
        const double section = 0.55 + 0.45 * std::sin(t / 23 + deck);
        const double kick = std::exp(-beat * 9) * section;
        a->low[i] = float(std::clamp(0.15 + 0.8 * kick, 0.0, 1.0));
        a->mid[i] = float(std::clamp(0.2 + 0.35 * section * (0.6 + 0.4 * std::sin(t * 7.3 + deck)), 0.0, 1.0));
        a->high[i] = float(std::clamp(0.1 + 0.25 * std::exp(-std::fmod(beat + .5, 1.0) * 14) + 0.05 * std::sin(t * 31), 0.0, 1.0));
    }
    a->duration = seconds;
    a->originalBpm = bpm;
    for (int n = 0; n * 60 / bpm < seconds; ++n) a->grid.push_back({n * 60000 / bpm, n % 4 + 1, bpm});
    for (int k = 0; k < 4; ++k) {
        Cue c;
        c.timeSec = 16 + k * 48 * 60 / bpm;
        c.number = k + 1;
        c.color = cueColour(false, c.number);
        if (k == 2) c.endSec = c.timeSec + 8 * 60 / bpm;
        a->cues.push_back(c);
    }
    Cue memory;
    memory.timeSec = 8;
    memory.memory = true;
    memory.color = cueColour(true, 0);
    a->cues.push_back(memory);
    return a;
}

}  // namespace

Session::Session(const QString &path, bool demo, QObject *parent)
    : QObject(parent), settingsPath(path), demo_(demo), cueLibraries_(std::make_shared<Libraries>()) {
    QString error;
    if (!settings.load(settingsPath, &error)) settingsError = error;
    analysisPool_.setMaxThreadCount(1);
    requestPool_.setMaxThreadCount(1);
    started_ = now();
    if (demo_) {
        static const char *titles[][3] = {{"Night Drive", "Lumen", "8A"}, {"Second Sun", "Okara", "11B"},
                                          {"Glass Hours", "Vela Ray", "5A"}, {"Undertow", "Mira Kade", "2B"}};
        for (int i = 0; i < 4; ++i) {
            DeckView &d = decks_[size_t(i)];
            auto a = demoAnalysis(i);
            d.cues = a->cues;
            d.originalBpm = d.currentBpm = a->originalBpm;
            d.analysis = a;
            d.generation = nextGeneration_++;
            d.meta.title = titles[i][0];
            d.meta.artist = titles[i][1];
            d.meta.key = titles[i][2];
            bridge::Deck s;
            s.loaded = true;
            s.playing = i != 3;
            s.master = i == 0;
            s.sync = i == 1;
            s.masterTempo = i != 2;
            s.tempoRange = 10;
            d.status = s;
        }
        setStatus(QStringLiteral("DEMO · synthetic analysis"));
        return;
    }
#if defined(Q_OS_WIN) || defined(Q_OS_MACOS)
    reader_ = new Reader(this);
    connect(reader_, &Reader::frame, this, &Session::receive, Qt::QueuedConnection);
    host_ = new host::Host([this] { return settings.flag("hide_rekordbox"); }, this);
    reader_->start();
    setStatus(QStringLiteral("Waiting for Rekordbox"));
#else
    setStatus(QStringLiteral("Rekordbox runs on Windows and macOS: Wave connects to it there"));
#endif
    cueTimer_.setInterval(250);
    connect(&cueTimer_, &QTimer::timeout, this, &Session::pollCues);
    cueTimer_.start();
    hostTimer_.setInterval(500);
    connect(&hostTimer_, &QTimer::timeout, this, [this] { pollHost(); pollLoading(); });
    hostTimer_.start();
}

Session::~Session() {
    if (reader_) reader_->stop();
    analysisPool_.clear();
    analysisPool_.waitForDone(2000);
    requestPool_.waitForDone(3000);
}

bool Session::save() {
    QString error;
    if (!settings.save(settingsPath, &error)) {
        qCWarning(lcSession) << "Settings could not be saved:" << error;
        return false;
    }
    return true;
}

void Session::setStatus(const QString &s) {
    if (s == status_) return;
    status_ = s;
    emit statusChanged(s);
}

bool Session::playing(int idx) const {
    const DeckView &d = decks_[size_t(idx)];
    if (demo_) return d.status && d.status->playing;
    return d.timeline.playing.value_or(false);
}

std::optional<double> Session::position(int idx) const {
    const DeckView &d = decks_[size_t(idx)];
    if (demo_) return d.status && d.status->playing ? 40 + std::fmod(now() - started_ + idx * 17, 100.0) : 63.0;
    const double t = now();
    return d.timeline.stale(t) ? d.timeline.samplePosition : d.timeline.position(t);
}

void Session::markLoopIn(int idx) {
    if (idx < 0 || idx >= 4) return;
    DeckView &d = decks_[size_t(idx)];
    if (!d.analysis || d.timeline.activeLoop()) return;  // LOOP IN on a running loop adjusts it: Rekordbox draws that
    const auto at = position(idx);
    if (!at) return;
    // Where the playhead is now; Rekordbox's own in point (quantized or not, as it placed it) replaces it as soon as the
    // bridge reports it (receive).
    d.loopInPressed = at;
    d.loopInFromRekordbox = false;
    d.timeline.pendingIn = at;
    qCInfo(lcSession) << "LOOP IN on deck" << idx + 1 << "at" << *at << "s; Rekordbox's loop points now"
                      << (d.status && d.status->loopIn ? *d.status->loopIn : -1.0) << (d.status && d.status->loopOut ? *d.status->loopOut : -1.0);
    adoptLoopIn(idx);
    emit changed();
}

void Session::adoptLoopIn(int idx) {
    DeckView &d = decks_[size_t(idx)];
    if (!d.loopInPressed || !d.status || d.status->loop) return;
    const auto in = d.status->loopIn, out = d.status->loopOut;
    // Rekordbox's in point for this press: near where it was pressed (quantize moves it a beat at most), and no
    // finished loop behind it (an old loop's points stand until a new one replaces them).
    if (!in || std::abs(*in - *d.loopInPressed) > 2.0 || (out && *out > *in)) return;
    if (!d.loopInFromRekordbox || d.timeline.pendingIn != in)
        qCInfo(lcSession) << "Loop in on deck" << idx + 1 << "at" << *in << "s from Rekordbox (pressed at" << *d.loopInPressed << "s)";
    d.timeline.pendingIn = in;
    d.loopInFromRekordbox = true;
}

void Session::clearLoopIn(int idx) {
    if (idx < 0 || idx >= 4) return;
    DeckView &d = decks_[size_t(idx)];
    d.loopInPressed.reset();
    d.loopInFromRekordbox = false;
    if (!d.timeline.pendingIn) return;
    d.timeline.pendingIn.reset();
    emit changed();
}

std::optional<double> Session::previewPosition(int idx) const {
    const auto it = previews_.find(idx);
    if (it == previews_.end()) return std::nullopt;
    const auto [seconds, until] = it->second;
    if (!until) return seconds;
    const double t = now();
    const auto live = position(idx);
    if (!live || t >= *until + kPreviewReturn) return std::nullopt;
    if (t <= *until) return seconds;
    double blend = (t - *until) / kPreviewReturn;
    blend = blend * blend * (3 - 2 * blend);  // a smooth glide home
    return seconds + (*live - seconds) * blend;
}

bool Session::animating() const {
    if (!previews_.empty()) return true;
    for (int i = 0; i < 4; ++i)
        if (decks_[size_t(i)].analysis && playing(i)) return true;
    return false;
}

double Session::zoomBeats() const { return knobBeats_.value_or(settings.number("zoom_beats")); }

double Session::knobZoom(int value, bool reverse) {
    // The sampler knob as a zoom: its middle is the zoom set in Settings, its ends 1 bar and 32 bars.
    const double base = std::clamp(settings.number("zoom_beats"), kKnobNear, kKnobFar);
    double x = std::clamp((value - 64) / (value > 64 ? 63.0 : 64.0), -1.0, 1.0);  // 0 and 127 both reach the ends
    if (!reverse) x = -x;  // clockwise zooms in: fewer beats, a bigger waveform
    const double end = x > 0 ? kKnobFar : kKnobNear;
    knobBeats_ = base * std::pow(end / base, std::abs(x));  // even steps in octaves, each side
    emit changed();
    return *knobBeats_;
}

void Session::previewHold(int idx, double seconds) {
    previews_[idx] = {seconds, std::nullopt};
    if (seekable(idx)) seekTo(idx, seconds);  // a paused deck goes there as the finger moves: CUE then plays from it
    emit changed();
}

void Session::previewRelease(int idx) {
    auto it = previews_.find(idx);
    if (it == previews_.end()) return;
    // A paused deck has gone (is going) where the finger left it: held there while Rekordbox's playhead arrives. A
    // playing deck, or one in a loop, was only looked at: the view glides back to it.
    const auto sk = seeking_.find(idx);
    const bool went = sk != seeking_.end();
    it->second.second = now() + (went ? 1.0 : kPreviewHold);
    if (went && !sk->second.asked && !sk->second.next) seeking_.erase(sk);  // nothing still on its way
}

bool Session::seekable(int idx) const {
    if (demo_ || !bridged() || idx < 0 || idx >= 4) return false;
    const DeckView &d = decks_[size_t(idx)];
    return d.status && d.status->loaded && !d.status->playing && !d.status->loop && d.analysis;
}

void Session::seekTo(int idx, double seconds) {
    Seeking &s = seeking_[idx];
    if (s.asked) { s.next = seconds; return; }  // one at a time: the latest place goes when this one is answered
    s.asked = true;
    s.next.reset();
    const DeckView &d = decks_[size_t(idx)];
    const auto ms = uint32_t(std::lround(std::clamp(seconds, 0.0, d.analysis->duration) * 1000));
    request([idx, ms](bridge::Client &c) { return c.request(bridge::Seek, uint32_t(idx), ms, 0); },
            [this, idx, seconds](bool ok, const QString &message) {
                Seeking &s = seeking_[idx];
                s.asked = false;
                if (!ok) qCInfo(lcSession) << "Deck" << idx + 1 << "to" << seconds << "s:" << message;
                if (s.next && ok && seekable(idx)) { seekTo(idx, *s.next); return; }
                const auto p = previews_.find(idx);
                const bool held = p != previews_.end() && !p->second.second;
                if (!held) {  // let go: done
                    seeking_.erase(idx);
                    if (ok)
                        QTimer::singleShot(600, this, [this, idx, seconds, message] {
                            const auto at = position(idx);
                            qCInfo(lcSession) << "Deck" << idx + 1 << "moved to" << seconds << "s (" << message << "); Rekordbox's playhead now"
                                              << (at ? *at : -1.0) << (playing(idx) ? "playing" : "paused");
                        });
                }
                if (!ok && p != previews_.end() && p->second.second) p->second.second = now();  // didn't take: back to the deck
            });
}

void Session::receive(std::shared_ptr<const ReaderFrame> f) {
    frame_ = f;
    std::set<int> reopened;  // a deck whose timeline starts over: told where Rekordbox's playhead is now, paused or not
    for (const auto &[idx, info] : f->tracks) {
        if (idx < 0 || idx >= 4) continue;
        DeckView &d = decks_[size_t(idx)];
        d.meta = info;
        if (info.path.isEmpty()) { clearDeck(idx); continue; }
        d.error = info.error;
        if (!info.analysisFolder.isEmpty() && info.analysisFolder != d.requested) {
            openTrack(idx, info.analysisFolder);
            d.timeline.reset();
            reopened.insert(idx);
        } else if (info.analysisFolder.isEmpty()) {
            d.analysis.reset();
            d.requested.clear();
            d.cues.clear();
            d.timeline.reset();
        }
    }
    if (f->bridged) {
        for (int idx = 0; idx < int(f->decks.size()) && idx < 4; ++idx) {
            DeckView &d = decks_[size_t(idx)];
            const bridge::Deck &live = f->decks[size_t(idx)];
            d.status = live;
            if (!live.loaded) continue;
            if (live.bpm > 0) d.currentBpm = live.bpm;
            const double original = d.originalBpm > 0 ? d.originalBpm : live.originalBpm;
            d.timeline.rateHint = original > 0 && d.currentBpm > 0 ? std::optional<double>(d.currentBpm / original) : std::nullopt;
            d.timeline.beatSeconds = original > 0 ? std::optional<double>(60 / original) : std::nullopt;
            // Also when the track's analysis came later than the track (Rekordbox analysing it after loading it): a
            // paused deck doesn't change, and without this it read "No track loaded" until something moved it.
            if (f->decksChanged || reopened.count(idx)) {
                d.timeline.timing({.08, .25, 0.0, .3});
                Observation o;
                o.position = live.position;
                o.observedAt = std::min(f->when, now());
                o.playing = live.playing;
                o.loopActive = live.loop.has_value();
                if (live.loop) { o.loopStart = live.loop->first; o.loopEnd = live.loop->second; }
                d.timeline.observe(o, now());
            }
            if (live.loop) {  // the loop is made: Rekordbox's own loop is drawn
                d.timeline.pendingIn.reset();
                d.loopInPressed.reset();
            } else {
                adoptLoopIn(idx);
            }
        }
    } else {
        for (DeckView &d : decks_) d.status.reset();
    }
    // The status line: what Wave is doing for Rekordbox, else what the reader sees.
    if (host_ && !host_->doing.isEmpty()) setStatus(host_->doing + QStringLiteral("…"));
    else if (!f->connected && host_ && !host_->error.isEmpty() && (f->state == host::State::Absent || f->state == host::State::None))
        setStatus(host_->error);
    else setStatus(f->status);
    emit frameArrived(f);
    emit changed();
}

void Session::clearDeck(int idx) {
    DeckView &d = decks_[size_t(idx)];
    d = DeckView{};
    previews_.erase(idx);
    emit analysisChanged();
}

void Session::openTrack(int idx, const QString &folder) {
    DeckView &d = decks_[size_t(idx)];
    d.requested = folder;
    d.analysis.reset();
    d.cues.clear();
    d.originalBpm = 0;
    d.generation = nextGeneration_++;
    d.error = QStringLiteral("Loading track analysis…");
    emit analysisChanged();
    auto libraries = cueLibraries_;
    auto *watcher = new QFutureWatcher<std::pair<std::shared_ptr<const Analysis>, QString>>(this);
    connect(watcher, &QFutureWatcherBase::finished, this, [this, watcher, idx, folder] {
        watcher->deleteLater();
        const auto [analysis, error] = watcher->result();
        DeckView &d = decks_[size_t(idx)];
        if (d.requested != folder) return;  // another track came meanwhile
        if (!analysis) { d.error = QStringLiteral("Analysis unavailable: %1").arg(error); emit changed(); return; }
        d.analysis = analysis;
        d.cues = analysis->cues;
        d.originalBpm = analysis->originalBpm;
        d.error.clear();
        emit analysisChanged();
        emit changed();
    });
    watcher->setFuture(QtConcurrent::run(&analysisPool_, [folder, libraries]() -> std::pair<std::shared_ptr<const Analysis>, QString> {
        QString error;
        auto a = std::make_shared<Analysis>(loadFolder(folder, &error));
        if (a->empty()) return {nullptr, error.isEmpty() ? QStringLiteral("No detailed waveform in this folder") : error};
        // A collection track's cues are in master.db, where Rekordbox writes edits at once.
        if (inCollection(folder)) a->cues = libraries->cues(folder);
        return {a, QString()};
    }));
}

void Session::pollCues() {
    // Re-read the collection tracks' cues a few times a second, so edits made anywhere appear.
    if (cuePolling_ || demo_) return;
    std::map<int, QString> folders;
    for (int i = 0; i < 4; ++i)
        if (decks_[size_t(i)].analysis && inCollection(decks_[size_t(i)].requested)) folders[i] = decks_[size_t(i)].requested;
    if (folders.empty()) return;
    // Only when Rekordbox wrote to its library since the last look (or another track came): a stat, not a query.
    if (masterPath_.isEmpty() || now() > masterCheckedAt_ + 30) { masterPath_ = masterDatabase(); masterCheckedAt_ = now(); }
    const QFileInfo db(masterPath_), wal(masterPath_ + "-wal");
    const QString signature = QStringLiteral("%1|%2|%3").arg(db.lastModified().toMSecsSinceEpoch())
                                  .arg(wal.lastModified().toMSecsSinceEpoch()).arg(wal.size());
    QStringList all;
    for (const auto &[idx, folder] : folders) all << QString::number(idx) + folder;
    if (signature == cueSignature_ && all == cueFolders_) return;
    cueSignature_ = signature;
    cueFolders_ = all;
    cuePolling_ = true;
    auto libraries = cueLibraries_;
    auto *watcher = new QFutureWatcher<std::map<int, std::vector<Cue>>>(this);
    connect(watcher, &QFutureWatcherBase::finished, this, [this, watcher, folders] {
        watcher->deleteLater();
        cuePolling_ = false;
        bool changedAny = false;
        for (const auto &[idx, cues] : watcher->result()) {
            DeckView &d = decks_[size_t(idx)];
            if (d.requested != folders.at(idx) || !d.analysis) continue;
            auto same = [](const std::vector<Cue> &a, const std::vector<Cue> &b) {
                if (a.size() != b.size()) return false;
                for (size_t i = 0; i < a.size(); ++i)
                    if (a[i].timeSec != b[i].timeSec || a[i].number != b[i].number || a[i].memory != b[i].memory || a[i].endSec != b[i].endSec)
                        return false;
                return true;
            };
            if (!same(d.cues, cues)) { d.cues = cues; changedAny = true; }
        }
        if (changedAny) emit changed();
    });
    watcher->setFuture(QtConcurrent::run(&analysisPool_, [folders, libraries] {
        std::map<int, std::vector<Cue>> out;
        for (const auto &[idx, folder] : folders) out[idx] = libraries->cues(folder);
        return out;
    }));
}

host::Seen Session::seen() {
    host::Seen s;
    if (!frame_) return s;
    s.pid = frame_->pid;
    s.state = frame_->state;
    s.stale = frame_->stale;
    s.known = frame_->connected;
    for (int i = 0; i < 4; ++i) {
        if (playing(i)) s.playing = true;
        if (!decks_[size_t(i)].meta.path.isEmpty()) s.loaded = true;
    }
    if (s.pid) {
        auto it = ages_.find(s.pid);
        if (it == ages_.end()) {
            ages_ = {{s.pid, {host::processAge(s.pid), now()}}};
            bridgeModules_.reset();
            it = ages_.find(s.pid);
        }
        if (it->second.first) s.age = *it->second.first + now() - it->second.second;
#ifdef Q_OS_WIN
        if (s.state == host::State::None && !bridgeModules_) bridgeModules_ = !host::bridgeModules(s.pid).isEmpty();
        s.bridgeLoaded = bridgeModules_.value_or(false);
#endif
    }
    return s;
}

void Session::pollHost() {
    if (!host_ || !frame_ || !frame_->checked) return;  // not before the reader has looked for Rekordbox once
    host_->policy.autoStart = settings.flag("start_rekordbox");
    host_->policy.autoConnect = settings.flag("auto_connect");
    host_->policy.keepRunning = settings.flag("keep_rekordbox_running");
    host_->poll(seen(), now());
    if (!host_->doing.isEmpty()) setStatus(host_->doing + QStringLiteral("…"));
}

QString Session::loadUnavailable() const {
    if (demo_) return QStringLiteral("Demo decks don't load tracks");
    if (!reader_) return QStringLiteral("Touch loading needs Rekordbox on Windows or macOS");
    if (!bridged()) return QString::fromLatin1(Reader::kRelaunch);
    return {};
}

void Session::request(std::function<std::pair<bool, QString>(bridge::Client &)> ask, std::function<void(bool, QString)> answered) {
    auto *watcher = new QFutureWatcher<std::pair<bool, QString>>(this);
    connect(watcher, &QFutureWatcherBase::finished, this, [watcher, answered] {
        watcher->deleteLater();
        const auto [ok, message] = watcher->result();
        answered(ok, message);
    });
    watcher->setFuture(QtConcurrent::run(&requestPool_, [ask]() -> std::pair<bool, QString> {
        bridge::Client client(QDir(bridge::defaultFolder()).filePath("state.bin"));
        QString error;
        if (!client.open(&error)) return {false, error};
        return ask(client);
    }));
}

int64_t deviceHandle(const QString &root, const std::vector<std::pair<int64_t, QString>> &devices) {
    // Rekordbox names a stick by its drive ("E:", "E:\\"…): matched by what it names, not how it's written.
    const QString wanted = rootKey(root);
    for (const auto &[h, mounted] : devices)
        if (h && rootKey(mounted) == wanted) return h;
    // One stick in Windows and one in Rekordbox are the same stick, however Rekordbox writes its root.
    if (devices.size() == 1 && devices.front().first && stickLibraries().size() == 1 &&
        rootKey(QFileInfo(stickLibraries().front()).dir().absoluteFilePath("../..")) == wanted)
        return devices.front().first;
    return 0;
}

bool Session::loadTrack(int idx, const CatalogTrack &track, const QString &source) {
    QString why = loadUnavailable();
    if (why.isEmpty() && loading_) why = QStringLiteral("Still loading %1").arg(loading_->title);
    if (why.isEmpty() && playing(idx)) why = QStringLiteral("Deck %1 is playing: stop it before loading over it").arg(idx + 1);
    bool numeric = false;
    const uint32_t id = track.id.toUInt(&numeric);
    if (why.isEmpty() && !numeric) why = QStringLiteral("Only Rekordbox tracks can be loaded from here");
    int64_t handle = 0;
    QString expected = track.path;
    if (why.isEmpty() && !source.isEmpty()) {
        // A stick's export stores paths from its own root; the engine reports the mounted path.
        const QString root = QDir(QFileInfo(source).absolutePath() + "/../..").absolutePath();
        expected = QDir(root).filePath(QString(track.path).remove(0, track.path.startsWith('/') ? 1 : 0));
        const auto devices = frame_ ? frame_->devices : std::vector<std::pair<int64_t, QString>>{};
        handle = deviceHandle(root, devices);
        if (!handle) {
            why = devices.empty() ? QStringLiteral("Rekordbox hasn't opened a USB stick yet: wait a moment after plugging it in")
                                  : QStringLiteral("Rekordbox has not mounted that USB stick yet");
            QStringList seen;
            for (const auto &[h, mounted] : devices) seen << QStringLiteral("%1=%2").arg(h).arg(mounted);
            qCWarning(lcSession) << "No Rekordbox device for" << root << "; Rekordbox has" << seen;
        }
    }
    if (!why.isEmpty()) {
        emit loadStatus(why);
        return false;
    }
    loading_ = Loading{idx, track.title, expected, now() + 8};
    waveLoads_[idx] = {expected, now() + 15};
    emit loadStatus(QStringLiteral("Loading %1 on deck %2…").arg(track.title).arg(idx + 1));
    request([idx, id, handle](bridge::Client &c) { return c.request(handle ? bridge::LoadDevice : bridge::Load, idx, id, handle); },
            [this](bool ok, const QString &message) {
                if (!loading_) return;
                loading_->answered = true;
                loading_->accepted = ok;
                if (!ok) {
                    emit loadStatus(QStringLiteral("Deck %1: %2").arg(loading_->deck + 1).arg(message));
                    loading_.reset();
                }
            });
    return true;
}

void Session::pollLoading() {
    if (!loading_) return;
    // Done only when Rekordbox's engine reports this very file on this deck.
    const DeckView &d = decks_[size_t(loading_->deck)];
    if (loading_->accepted && d.status && samePath(d.status->path, loading_->path) &&
        QDir::fromNativeSeparators(d.status->path).compare(QDir::fromNativeSeparators(loading_->path), Qt::CaseInsensitive) == 0) {
        emit loadStatus(QStringLiteral("Loaded %1 on deck %2").arg(loading_->title).arg(loading_->deck + 1));
        loading_.reset();
        return;
    }
    if (now() > loading_->deadline) {
        emit loadStatus(QStringLiteral("Rekordbox did not load %1 on deck %2").arg(loading_->title).arg(loading_->deck + 1));
        loading_.reset();
    }
}

bool Session::loadedByWave(int idx, const QString &path) const {
    const auto it = waveLoads_.find(idx);
    if (it == waveLoads_.end() || now() > it->second.second || path.isEmpty()) return loading_ && loading_->deck == idx;
    return samePath(path, it->second.first);
}

void Session::toggleMasterTempo(int idx) {
    const DeckView &d = decks_[size_t(idx)];
    if (!d.status || !bridged()) {
        setStatus(loadUnavailable().isEmpty() ? QStringLiteral("Master tempo needs the Rekordbox bridge") : loadUnavailable());
        return;
    }
    const bool on = !d.status->masterTempo;
    request([idx, on](bridge::Client &c) { return c.request(bridge::MasterTempo, idx, on ? 1 : 0); }, [](bool, const QString &) {});
}

void Session::showRekordboxBrowser(bool on, int x, int y, std::function<void(bool, QString)> answered) {
    if (!bridged()) {
        if (answered) answered(false, QStringLiteral("Rekordbox is not connected"));
        return;
    }
    const int64_t packed = on ? int64_t((uint64_t(uint32_t(y)) << 32) | uint32_t(x)) : 0;
    request([on, packed](bridge::Client &c) { return c.request(bridge::ShowBrowser, 0, on ? 1 : 0, packed); },
            [answered](bool ok, const QString &message) { if (answered) answered(ok, message); });
}

void Session::openRekordboxPreferences(std::function<void(bool, QString)> answered) {
    if (!bridged()) {
        if (answered) answered(false, QStringLiteral("Rekordbox is not connected"));
        return;
    }
    request([](bridge::Client &c) { return c.request(bridge::Rekordbox, 0, 0x200A, 0); },
            [answered](bool ok, const QString &message) { if (answered) answered(ok, message); });
}

void Session::searchRekordbox(const QString &query, std::function<void(bool, QString)> answered) {
    if (!bridged()) {
        if (answered) answered(false, QStringLiteral("Rekordbox is not connected"));
        return;
    }
    request([query](bridge::Client &c) { return c.search(query); },
            [answered](bool ok, const QString &message) { if (answered) answered(ok, message); });
}

void Session::readRekordboxList(int first) {
    if (!bridged()) return;
    request([first](bridge::Client &c) { c.listFrom(first); return std::make_pair(true, QString()); }, [](bool, const QString &) {});
}

bool Session::loadRow(int idx, int row, uint32_t id, const QString &title, const QString &path) {
    QString why = loadUnavailable();
    if (why.isEmpty() && loading_) why = QStringLiteral("Still loading %1").arg(loading_->title);
    if (why.isEmpty() && playing(idx)) why = QStringLiteral("Deck %1 is playing: stop it before loading over it").arg(idx + 1);
    if (!why.isEmpty()) {
        emit loadStatus(why);
        return false;
    }
    loading_ = Loading{idx, title, path, now() + 8};
    waveLoads_[idx] = {path, now() + 15};
    emit loadStatus(QStringLiteral("Loading %1 on deck %2…").arg(title).arg(idx + 1));
    request([idx, row, id](bridge::Client &c) { return c.request(bridge::LoadRow, idx, id, row); },
            [this](bool ok, const QString &message) {
                if (!loading_) return;
                loading_->answered = true;
                loading_->accepted = ok;
                if (!ok) {
                    emit loadStatus(QStringLiteral("Deck %1: %2").arg(loading_->deck + 1).arg(message));
                    loading_.reset();
                }
            });
    return true;
}

void Session::connectRekordbox() {
    if (host_) host_->connectNow();
}

}  // namespace wave
