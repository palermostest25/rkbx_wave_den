// The core, checked against the Python app it replaces: the same files read to the same values, the
// same tiles drawn to the same pixels (references made by tests/make_fixtures.py), and the playback
// timeline's own tests, ported case by case.
#include "core/anlz.h"
#include "core/bridge.h"
#include "core/timeline.h"
#include "core/waveform.h"

#include "../../native/mac_bridge/bridge_layout.h"

#include <QFile>
#include <QImage>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QTest>

#include <cmath>
#include <cstring>

using namespace wave;

class CoreTests : public QObject {
    Q_OBJECT

    static QJsonObject expected() {
        QFile file(QStringLiteral(WAVE_TEST_DATA "/expected.json"));
        file.open(QIODevice::ReadOnly);
        return QJsonDocument::fromJson(file.readAll()).object();
    }

    static Observation at(double position, double time, std::optional<bool> playing = {}, std::optional<double> rate = {}) {
        Observation o;
        o.position = position;
        o.observedAt = time;
        o.playing = playing;
        o.rate = rate;
        return o;
    }

private slots:
    // --- Rekordbox's files --------------------------------------------------------------------
    void analysisMatchesThePythonApp() {
        const QJsonObject want = expected();
        QString error;
        const Analysis a = loadFolder(QStringLiteral(WAVE_TEST_DATA "/track"), &error);
        QVERIFY2(!a.empty(), qPrintable(error));
        QCOMPARE(a.columns(), want["columns"].toInt());
        QCOMPARE(a.duration, want["duration"].toDouble());
        QVERIFY(a.songPath.endsWith(QLatin1String("Test Track.mp3")));
        for (const char *band : {"low", "mid", "high"}) {
            const auto &mine = band == QLatin1String("low") ? a.low : band == QLatin1String("mid") ? a.mid : a.high;
            const QJsonArray theirs = want[band].toArray();
            for (int i = 0; i < theirs.size(); ++i) QCOMPARE(mine[i], float(theirs[i].toDouble()));
        }
        QCOMPARE(a.originalBpm, want["bpm"].toDouble());
        const QJsonArray grid = want["grid"].toArray();
        QCOMPARE(int(a.grid.size()), grid.size());
        for (int i = 0; i < grid.size(); ++i) {
            QCOMPARE(a.grid[i].timeMs, grid[i][0].toDouble());
            QCOMPARE(a.grid[i].number, grid[i][1].toInt());
            QCOMPARE(a.grid[i].bpm, grid[i][2].toDouble());
        }
        const QJsonArray cues = want["cues"].toArray();
        QCOMPARE(int(a.cues.size()), cues.size());
        for (int i = 0; i < cues.size(); ++i) {
            const QJsonArray c = cues[i].toArray(), rgb = c[1].toArray();
            QCOMPARE(a.cues[i].timeSec, c[0].toDouble());
            QCOMPARE(a.cues[i].color, QColor(rgb[0].toInt(), rgb[1].toInt(), rgb[2].toInt()));
            QCOMPARE(a.cues[i].memory, c[2].toBool());
            QCOMPARE(a.cues[i].number, c[3].toInt());
            QCOMPARE(a.cues[i].endSec.has_value(), !c[4].isNull());
            if (a.cues[i].endSec) QCOMPARE(*a.cues[i].endSec, c[4].toDouble());
        }
    }

    void damagedFilesGiveNothingRatherThanCrash() {
        QVERIFY(readTags(QByteArray("PMAI\0\0\0\x1c", 8)).empty());
        QByteArray file("PMAI");
        file += QByteArray::fromHex("0000001c7fffffff") + QByteArray(16, 0) + QByteArray("PWV7\x00\x00\x00\x18\xff\xff\xff\xff", 12);
        QVERIFY(readTags(file).empty());  // a tag claiming more than the file holds
        QVERIFY(loadFolder(QStringLiteral("/no/such/folder")).empty());
    }

    // --- Waveform pictures -------------------------------------------------------------------
    void tilesMatchThePythonRendererPixelForPixel() {
        const Analysis a = loadFolder(QStringLiteral(WAVE_TEST_DATA "/track"));
        const Palette rekordbox;  // Rekordbox's colours, the default
        const Levels levels{1.1860465116279069f, 1.1860465116279069f, 1.3255813953488373f, 1};
        for (auto [tile, reduction] : {std::pair{0, 1}, {2, 1}, {1, 4}}) {
            const QImage want(QStringLiteral(WAVE_TEST_DATA "/tile-%1-%2.png").arg(tile).arg(reduction));
            const QImage mine = renderTile(a, tile, rekordbox, levels, reduction);
            QCOMPARE(mine.size(), want.size());
            int differ = 0;
            for (int y = 0; y < want.height(); ++y)
                for (int x = 0; x < want.width(); ++x)
                    differ += (mine.pixel(x, y) & 0xffffff) != (want.pixel(x, y) & 0xffffff);
            QVERIFY2(differ == 0, qPrintable(QStringLiteral("tile %1/%2: %3 pixels differ").arg(tile).arg(reduction).arg(differ)));
        }
    }

    void tileCacheIsBoundedAndKeyedByTrack() {
        auto a = std::make_shared<const Analysis>(loadFolder(QStringLiteral(WAVE_TEST_DATA "/track")));
        TileCache cache(2);
        cache.get(a, 0, {}, {});
        cache.get(a, 1, {}, {});
        cache.get(a, 0, {}, {});  // now the most recent
        cache.get(a, 2, {}, {});
        QCOMPARE(cache.size(), 2);
        QVERIFY(cache.cached(a.get(), 0, {}, {}));
        QVERIFY(!cache.cached(a.get(), 1, {}, {}));  // the oldest went
        QVERIFY(!cache.cached(a.get(), 0, {}, Levels{2, 1, 1, 1}));  // other levels, another picture
    }

    // --- Playback timeline (tests/test_live_state.py) ----------------------------------------
    void unloadedResetAndPause() {
        PlaybackTimeline t;
        QVERIFY(!t.position(0));
        QVERIFY(t.stale(0));
        t.observe(at(5, 1, true, 1), 1);
        QVERIFY(qFuzzyCompare(*t.position(1.02), 5.02));
        t.observe(at(5.02, 1.02, false, 1), 1.02);
        QCOMPARE(*t.position(1.2), 5.02);
        t.reset();
        QVERIFY(!t.position(2));
    }

    void velocityInterpolationAndStaleCap() {
        PlaybackTimeline t;
        for (int i = 0; i < 5; ++i) t.observe(at(10 + i * .02, 1 + i * .02), 1 + i * .02);
        QVERIFY(std::abs(*t.position(1.10) - 10.10) < 1e-9);
        QVERIFY(std::abs(*t.position(9) - 10.16) < 1e-9);
        QVERIFY(t.stale(9));
    }

    void unchangedObservationsStopAfterABoundedHold() {
        PlaybackTimeline t;
        t.observe(at(5, 0), 0);
        t.observe(at(5.02, .02), .02);
        t.observe(at(5.02, .04), .04);
        QVERIFY(std::abs(*t.position(.06) - 5.06) < 1e-9);
        t.observe(at(5.02, .12), .12);
        QCOMPARE(*t.position(.20), 5.02);
    }

    void pollingFasterThanTheSourceDoesNotStutter() {
        PlaybackTimeline t;
        double now = 0;
        for (int tick = 0; tick < 30; ++tick) {
            now = tick * .02;
            t.observe(at((tick / 2) * .04, now), now);
            if (tick >= 2) {
                QVERIFY(std::abs(*t.position(now) - now) < 1e-7);
                QVERIFY(std::abs(*t.position(now + .01) - (now + .01)) < 1e-7);
            }
        }
        QVERIFY(!t.observe(at(1, now - .001), now));
    }

    void aCueJumpIsNotAHugeVelocity() {
        PlaybackTimeline t;
        t.observe(at(5, 0), 0);
        t.observe(at(5.02, .02), .02);
        t.observe(at(80, .04), .04);
        QCOMPARE(*t.position(.10), 80.0);
        t.observe(at(80.02, .06), .06);
        QVERIFY(std::abs(*t.position(.08) - 80.04) < 1e-7);
    }

    void reverseJogFollowsWithoutOvershooting() {
        PlaybackTimeline t;
        t.observe(at(5, 0), 0);
        t.observe(at(4.98, .02), .02);
        QVERIFY(*t.position(.03) > 4.98 && *t.position(.03) < 5);
        QVERIFY(std::abs(*t.position(.2) - 4.98) < 1e-9);
    }

    void platterScratchNeverRunsAhead() {
        PlaybackTimeline t;
        t.timing({.3, .6, .1, .6});
        t.rateHint = 1.0;
        for (int tick = 0; tick < 240; ++tick) {
            const double now = tick / 60.0, truth = 30 + .35 * std::sin(3 * M_PI * now);
            t.observe(at(std::floor(truth * 10) / 10, now), now);
            if (tick > 30) QVERIFY(std::abs(*t.position(now) - truth) < .2);
        }
    }

    // --- The bridge's shared file ------------------------------------------------------------
    void bridgeStateIsReadConsistentlyOrNotAtAll() {
        std::vector<uchar> raw(sizeof(BridgeState));
        auto *s = reinterpret_cast<BridgeState *>(raw.data());
        std::memcpy(s->magic, kBridgeMagic, sizeof kBridgeMagic);
        s->version = kBridgeVersion;
        s->seq = 2; s->pid = 4242; s->status = kStatusLive; s->deck_count = 2; s->master = 1; s->ticks = 7; s->uptime_ns = 123;
        s->decks[0].flags = kDeckLoaded | kDeckPlaying | kDeckLooping | kDeckSync;
        s->decks[0].position_ms = 4721; s->decks[0].total_ms = 300000; s->decks[0].bpm = 12800; s->decks[0].original_bpm = 12500;
        s->decks[0].loop_in = 4430; s->decks[0].loop_out = 6234; s->decks[0].key_shift = 2;
        std::strcpy(s->decks[0].path, "/a.m4a");
        s->decks[1].flags = kDeckLoaded | kDeckMasterTempo;
        s->devices[1].handle = 77;
        std::strcpy(s->devices[1].root, "/Volumes/GIG");
        auto snap = bridge::parse(raw.data(), qint64(raw.size()));
        QVERIFY(snap);
        QCOMPARE(snap->pid, 4242u);
        QCOMPARE(int(snap->decks.size()), 2);
        QVERIFY(std::abs(snap->decks[0].position - 4.721) < 1e-12);
        QCOMPARE(snap->decks[0].bpm, 128.0);
        QCOMPARE(snap->decks[0].loop->first, 4.43);
        QVERIFY(snap->decks[0].playing && snap->decks[0].sync && !snap->decks[0].master);
        QVERIFY(snap->decks[1].master && snap->decks[1].masterTempo && !snap->decks[1].loop);
        QCOMPARE(snap->devices.size(), size_t(1));
        QCOMPARE(snap->devices[0].second, QStringLiteral("/Volumes/GIG"));
        QVERIFY(bridge::live(snap, 4242, 123 + 10));
        QVERIFY(!bridge::live(snap, 99, 123 + 10));
        QVERIFY(!bridge::live(snap, 4242, 123 + 2000000000ULL));
        s->seq = 3;  // the bridge is writing
        QVERIFY(!bridge::parse(raw.data(), qint64(raw.size())));
        s->seq = 2;
        s->version = kBridgeVersion - 1;  // an earlier Wave's bridge
        QVERIFY(bridge::parse(raw.data(), qint64(raw.size()))->older);
    }
};

QTEST_GUILESS_MAIN(CoreTests)
#include "core_tests.moc"
