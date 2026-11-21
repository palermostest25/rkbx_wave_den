// The core, checked against the Python app it replaces: the same files read to the same values, the
// same tiles drawn to the same pixels (references made by tests/make_fixtures.py), and the playback
// timeline's own tests, ported case by case.
#include "core/anlz.h"
#include "core/bridge.h"
#include "core/keys.h"
#include "core/library.h"
#include "core/streamed.h"
#include "core/settings.h"
#include "core/timeline.h"
#include "core/waveform.h"

#include "../../native/mac_bridge/bridge_layout.h"

#include <QFile>
#include <QImage>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QTemporaryDir>
#include <QTest>

#include <sqlite3.h>

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
    void keysReadInEitherNotation() {
        const char *const pairs[][2] = {{"Am", "8A"}, {"C", "8B"}, {"Em", "9A"}, {"Dm", "7A"}, {"F#m", "11A"}, {"Abm", "1A"},
                                        {"B", "1B"}, {"Db", "3B"}, {"E", "12B"}, {"Bbm", "3A"}, {"Gm", "6A"}, {"F", "7B"}};
        for (const auto &[classic, alpha] : pairs) {
            QCOMPARE(keyText(QString::fromLatin1(classic), "alphanumeric"), QString::fromLatin1(alpha));
            QCOMPARE(keyText(QString::fromLatin1(alpha), "classic"), QString::fromLatin1(classic));
        }
        QCOMPARE(keyText("G#m", "alphanumeric"), QStringLiteral("1A"));  // enharmonics
        QCOMPARE(keyText("C#", "classic"), QStringLiteral("Db"));
        QCOMPARE(keyText("8A", "alphanumeric"), QStringLiteral("8A"));
        QCOMPARE(keyText("Am", "rekordbox"), QStringLiteral("Am"));      // as stored
        QCOMPARE(keyText("odd", "alphanumeric"), QStringLiteral("odd"));  // not a key: as it is
    }
    // --- Rekordbox's files --------------------------------------------------------------------
    void streamedTitlesAreReadAsRekordboxWritesThem() {
        // Rekordbox's own format, made with its key by its encrypt function's recipe (AES-128-CBC, PKCS#7, Base64).
        QCOMPARE(revealStreamed("$A7:v1:AAECAwQFBgcICQoLDA0ODw==:v+JW6Kn2zaWWJa1s8T1xJhZuMTV6BaYUE7qBzT/H7Eo="),
                 QStringLiteral("Danielle (smile on my face)"));
        QCOMPARE(revealStreamed("$A7:v1:oaGhoaGhoaGhoaGhoaGhoQ==:kl6mF/HrrVRnv6R5ZjojbQ=="), QStringLiteral("Fred again.."));
        QCOMPARE(revealStreamed("$A7:v1:Dw8PDw8PDw8PDw8PDw8PDw==:wXzKOtEs8DTU4AQ8V/loDWTs2xzghiBD8e3NuEF4y7E="),
                 QString::fromUtf8("\u00d8 \u00dcn\u00efc\u00f8d\u00e9 \u2013 Beatport"));
        QCOMPARE(revealStreamed("Plain title"), QStringLiteral("Plain title"));  // anything else, as it is
        QCOMPARE(revealStreamed("$Aloha"), QStringLiteral("$Aloha"));
        QVERIFY(revealStreamed("$A7:v1:AAECAwQFBgcICQoLDA0ODw==:AAAA").isEmpty());  // damaged: nothing, never garbage
        QVERIFY(isStreamedText("$A7:v1:x:y"));
        QVERIFY(!isStreamedText("$A$AP Rocky"));
    }

    void aesDecryptsTheStandardExample() {
        // FIPS-197 appendix C.1 through CBC: with a zero IV the first block is the standard's ciphertext; the second is
        // the PKCS#7 padding block encrypted after it.
        const QByteArray key = QByteArray::fromHex("000102030405060708090a0b0c0d0e0f");
        const QByteArray data = QByteArray::fromHex("69c4e0d86a7b0430d8cdb78070b4c55a9e978e6d16b086570ef794ef97984232");
        QCOMPARE(aes128CbcDecrypt(key, QByteArray(16, 0), data).toHex(), QByteArray("00112233445566778899aabbccddeeff"));
        QVERIFY(aes128CbcDecrypt(key, QByteArray(16, 0), data.left(16)).isEmpty());  // no valid padding: nothing
        QVERIFY(aes128CbcDecrypt(key, QByteArray(15, 0), data).isEmpty());
    }

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

    // --- Settings ------------------------------------------------------------------------------
    void settingsKeepWhatThisVersionDoesNotKnow() {
        QTemporaryDir dir;
        const QString path = dir.filePath("default_config.json");
        QFile file(path);
        QVERIFY(file.open(QIODevice::WriteOnly));
        file.write(R"({"color": {"low_color": [1, 2, 3]}, "render": {"deck_count": 2, "deck_order": "2,1,3,4"},
                      "view": {"zoom_beats": 64, "future_key": "kept", "settings_version": 1}, "extra": 5})");
        file.close();
        Settings s;
        QString error;
        QVERIFY2(s.load(path, &error), qPrintable(error));
        QCOMPARE(s.number("zoom_beats"), 64.0);
        QCOMPARE(s.text("browser"), QStringLiteral("rekordbox"));  // as 2.5 moved everyone
        QCOMPARE(s.text("keyboard"), QStringLiteral("wave"));
        QCOMPARE(s.text("tap_to_open"), QStringLiteral("double"));
        QCOMPARE(s.order(), (std::vector<int>{1, 0}));
        QCOMPARE(s.colours()[0], QColor(1, 2, 3));
        QCOMPARE(s.gains()[1], 1.0f);  // filled in from the defaults
        s.setView("keyboard", "windows");
        QVERIFY(s.save(path));
        Settings again;
        QVERIFY(again.load(path));
        QCOMPARE(again.text("keyboard"), QStringLiteral("windows"));
        QCOMPARE(again.json()["view"].toObject()["future_key"].toString(), QStringLiteral("kept"));
        QCOMPARE(again.json()["extra"].toInt(), 5);
        QCOMPARE(again.view("settings_version").toInt(), Settings::kVersion);
    }

    void damagedSettingsAreRefusedWhole() {
        QTemporaryDir dir;
        for (const char *text : {"not json", R"({"view": []})", R"({"view": {"zoom_beats": 2}})",
                                 R"({"render": {"deck_order": "1,1,2,3"}})", R"({"view": {"playhead": "x"}})"}) {
            QFile file(dir.filePath("bad.json"));
            QVERIFY(file.open(QIODevice::WriteOnly | QIODevice::Truncate));
            file.write(text);
            file.close();
            Settings s;
            QString error;
            QVERIFY2(!s.load(file.fileName(), &error), text);
            QVERIFY(!error.isEmpty());
            QCOMPARE(s.number("zoom_beats"), 32.0);
        }
        Settings fresh;
        QVERIFY(fresh.load(dir.filePath("missing.json")));  // no file yet is fine
        QCOMPARE(fresh.order(), (std::vector<int>{2, 0, 1, 3}));
        QCOMPARE(fresh.colours()[0], QColor(0, 84, 226));
    }

    void untouchedOldPalettesMoveToTheRekordboxLook() {
        QTemporaryDir dir;
        QFile file(dir.filePath("c.json"));
        QVERIFY(file.open(QIODevice::WriteOnly));
        file.write(R"({"color": {"low_color": [0,80,224], "mid_color": [248,160,0], "overlap_color": [176,96,0], "high_color": [244,234,214]}})");
        file.close();
        Settings s;
        QVERIFY(s.load(file.fileName()));
        QCOMPARE(s.colours()[1], QColor(255, 166, 0));
        QCOMPARE(s.text("style"), QStringLiteral("Rekordbox"));
    }

    // --- Rekordbox's library -------------------------------------------------------------------
    // The Python app's catalog fixture, in a database encrypted as Rekordbox's is.
    static QString makeLibrary(const QTemporaryDir &dir) {
        const QString path = dir.filePath("master.db");
        sqlite3 *db = nullptr;
        sqlite3_open(path.toUtf8().constData(), &db);
        const QByteArray key = databaseKey(false);
        sqlite3_key(db, key.constData(), int(key.size()));
        sqlite3_exec(db, R"(
          CREATE TABLE djmdContent(ID TEXT,Title TEXT,ArtistID TEXT,KeyID TEXT,BPM INTEGER,Length INTEGER,FolderPath TEXT,
            rb_local_deleted INTEGER,AnalysisDataPath TEXT,DJPlayCount INTEGER,updated_at TEXT,ImagePath TEXT);
          CREATE TABLE djmdArtist(ID TEXT,Name TEXT);
          CREATE TABLE djmdKey(ID TEXT,ScaleName TEXT);
          CREATE TABLE djmdCue(ContentID TEXT,InMsec INTEGER,OutMsec INTEGER,Kind INTEGER,rb_local_deleted INTEGER);
          INSERT INTO djmdArtist VALUES('a','Artist');
          INSERT INTO djmdKey VALUES('k','8A');
          INSERT INTO djmdContent VALUES('1','Ten','a','k',10000,300,'/ten.wav',0,'/PIONEER/USBANLZ/aa/ten/ANLZ0000.DAT',3,'2024','/PIONEER/Artwork/ten.jpg');
          INSERT INTO djmdContent VALUES('2','Nine','a','k',9000,90,'/nine.wav',0,'/PIONEER/USBANLZ/aa/nine/ANLZ0000.DAT',0,'2023','');
          INSERT INTO djmdContent VALUES('3','100%_remix','a','k',12800,240,'/remix.wav',0,'',0,'','');
          INSERT INTO djmdContent VALUES('4','100abc remix','a','k',12900,240,'/other.wav',1,'',0,'','');
          INSERT INTO djmdContent VALUES('5','$A7:v1:abc==:def==','a','k',12000,200,'spotify:track:x',0,'',0,'','');
          INSERT INTO djmdContent VALUES('6','Ten copy','a','k',10000,300,'/ten.wav',0,'/PIONEER/USBANLZ/bb/old/ANLZ0000.DAT',1,'2020','');
          INSERT INTO djmdCue VALUES('1',1500,4000,1,0);
          INSERT INTO djmdCue VALUES('1',500,-1,0,0);
          INSERT INTO djmdCue VALUES('1',9000,-1,5,0);
          INSERT INTO djmdCue VALUES('1',9500,-1,2,1);
          CREATE TABLE djmdPlaylist(ID TEXT,Seq INTEGER,Name TEXT,Attribute INTEGER,ParentID TEXT);
          CREATE TABLE djmdSongPlaylist(PlaylistID TEXT,ContentID TEXT,TrackNo INTEGER);
          INSERT INTO djmdPlaylist VALUES('f',1,'Gigs',1,'root');
          INSERT INTO djmdPlaylist VALUES('p',1,'Friday',0,'f');
          INSERT INTO djmdPlaylist VALUES('s',2,'Smart',4,'root');
          INSERT INTO djmdSongPlaylist VALUES('p','2',1);
          INSERT INTO djmdSongPlaylist VALUES('p','1',2);
        )", nullptr, nullptr, nullptr);
        sqlite3_close(db);
        return path;
    }

    void aHotCueSetMeanwhileIsSeen() {
        // Rekordbox writes a hot cue while Wave has the library open: data_version tells Wave to read the cues again.
        QTemporaryDir dir;
        const QString path = makeLibrary(dir);
        auto lib = Library::openFile(path, false);
        QVERIFY(lib);
        const QString folder = QStringLiteral("/PIONEER/USBANLZ/aa/ten");
        const auto before = lib->cues(folder);
        const qint64 version = lib->dataVersion();
        QVERIFY(version >= 0);
        QCOMPARE(lib->dataVersion(), version);  // nothing written: unchanged
        sqlite3 *db = nullptr;
        sqlite3_open(path.toUtf8().constData(), &db);
        const QByteArray key = databaseKey(false);
        sqlite3_key(db, key.constData(), int(key.size()));
        QCOMPARE(sqlite3_exec(db, "INSERT INTO djmdCue VALUES('1',20000,-1,3,0)", nullptr, nullptr, nullptr), SQLITE_OK);
        sqlite3_close(db);
        QVERIFY(lib->dataVersion() != version);
        QCOMPARE(lib->cues(folder).size(), before.size() + 1);
    }

    void theLibraryOpensOnlyWithRekordboxsKey() {
        QTemporaryDir dir;
        const QString path = makeLibrary(dir);
        QString error;
        QVERIFY2(Library::openFile(path, false, &error), qPrintable(error));
        QVERIFY(!Library::openFile(path, true, &error));  // a stick's key doesn't open the collection
        QVERIFY(error.contains("could not be read"));
        QVERIFY(!Library::openFile(dir.filePath("none.db"), false, &error));
        QCOMPARE(databaseKey(false).size(), 64);
        QVERIFY(databaseKey(false).startsWith("402fd482"));  // pyrekordbox's published master key
    }

    // A library encrypted elsewhere (SQLCipher with OpenSSL, as Rekordbox's is): every platform's own
    // crypto (CNG on Windows, CommonCrypto on macOS) must read it.
    void aLibraryEncryptedElsewhereOpens() {
        QTemporaryDir dir;
        const QString copy = dir.filePath("master.db");
        QVERIFY(QFile::copy(QStringLiteral(WAVE_TEST_DATA "/encrypted-master.db"), copy));
        QString error;
        auto lib = Library::openFile(copy, false, &error);
        QVERIFY2(lib, qPrintable(error));
        const auto [total, rows] = lib->browse({}, "Title", false, 0, 10);
        QCOMPARE(total, 1);
        QCOMPARE(rows[0].title, QStringLiteral("Encrypted elsewhere"));
        QCOMPARE(rows[0].artist, QStringLiteral("Made with OpenSSL"));
        QCOMPARE(rows[0].bpm, 123.45);
    }

    void playlistsDrillDownInRunningOrder() {
        QTemporaryDir dir;
        auto lib = Library::openFile(makeLibrary(dir), false);
        const auto top = lib->categories("PLAYLIST");
        QCOMPARE(int(top.size()), 1);  // smart lists stay in Rekordbox
        QCOMPARE(top[0].name, QStringLiteral("Gigs"));
        QVERIFY(top[0].folder);
        const auto inner = lib->categories("PLAYLIST", top[0].id);
        QCOMPARE(int(inner.size()), 1);
        QCOMPARE(inner[0].name, QStringLiteral("Friday"));
        QCOMPARE(inner[0].count, 2);
        const auto [total, rows] = lib->browse({}, "Order", false, 0, 50, std::pair<QString, QString>{"PLAYLIST", "p"});
        QCOMPARE(total, 2);
        QCOMPARE(rows[0].title, QStringLiteral("Nine"));
        QCOMPARE(rows[1].title, QStringLiteral("Ten"));
        QCOMPARE(lib->categories("KEY")[0].name, QStringLiteral("8A"));
        QCOMPARE(lib->categories("ARTIST")[0].count, 4);
        QCOMPARE(lib->browse({}, "Order", false, 0, 50).first, 0);  // no order outside a list
    }

    void rekordboxsListIsFoundByItsTracks() {
        QTemporaryDir dir;
        auto lib = Library::openFile(makeLibrary(dir), false);
        const auto found = lib->locate({2, 1}, 2);
        QVERIFY(found);
        QCOMPARE(found->kind, QStringLiteral("PLAYLIST"));
        QCOMPARE(found->id, QStringLiteral("p"));
        QCOMPARE(found->name, QStringLiteral("Friday"));
        QCOMPARE(int(found->trail.size()), 1);
        QCOMPARE(found->trail[0].second, QStringLiteral("Gigs"));
        QCOMPARE(lib->locate({1, 2, 3}, 4)->kind, QStringLiteral("TRACK"));  // the whole collection
        QVERIFY(!lib->locate({1}, 1));
        QVERIFY(!lib->locate({}, 5));
    }

    void searchIsLiteralAndScoped() {
        QTemporaryDir dir;
        auto lib = Library::openFile(makeLibrary(dir), false);
        const std::pair<QString, QString> friday{"PLAYLIST", "p"};
        QCOMPARE(lib->browse("nine", "Title", false, 0, 50, friday).second[0].title, QStringLiteral("Nine"));
        QCOMPARE(lib->browse("nine", "Title", false, 0, 50, friday).first, 1);
        QCOMPARE(lib->browse("artist", "Title", false, 0, 50).first, 4);
        QCOMPARE(lib->browse("no such track", "Title", false, 0, 50).first, 0);
        QCOMPARE(lib->browse("100%_ Artist", "Title", false, 0, 50).second[0].title, QStringLiteral("100%_remix"));
        QCOMPARE(lib->browse("100%_ Artist", "Title", false, 0, 50).first, 1);
        QCOMPARE(lib->browse("' OR 1=1 --", "Title", false, 0, 50).first, 0);
        QCOMPARE(lib->browse({}, "BPM; DROP TABLE djmdContent", false, 0, 50).first, 0);
        for (const auto &t : lib->browse({}, "Title", false, 0, 50).second) QVERIFY(!t.title.startsWith("$A7"));
        const auto [total, page] = lib->browse({}, "BPM", false, 1, 1);
        QCOMPARE(total, 4);
        QCOMPARE(page[0].bpm, 100.0);
        QCOMPARE(page[0].key, QStringLiteral("8A"));
        QCOMPARE(lib->browse({}, "Time", true, 0, 1).second[0].duration, 300.0);
        QVERIFY(lib->browse("ten", "Title", false, 0, 1).second[0].artwork.endsWith("share/PIONEER/Artwork/ten.jpg"));
    }

    void aLoadedFileFindsItsPreparedCopyAndCues() {
        QTemporaryDir dir;
        auto lib = Library::openFile(makeLibrary(dir), false);
        QVERIFY(QDir().mkpath(dir.filePath("share/PIONEER/USBANLZ/aa/ten")));
        const auto track = lib->resolvePath("/ten.wav");
        QVERIFY(track);
        QCOMPARE(track->id, QStringLiteral("1"));  // the copy with analysis on disk and cues
        QCOMPARE(track->bpm, 100.0);
        QCOMPARE(track->key, QStringLiteral("8A"));
        QVERIFY(track->analysisFolder.endsWith("share/PIONEER/USBANLZ/aa/ten"));
        QVERIFY(!lib->resolvePath("/missing.wav"));
        const auto cues = lib->cues(track->analysisFolder);
        QCOMPARE(int(cues.size()), 3);  // the deleted one isn't shown
        QVERIFY(cues[0].memory && cues[0].timeSec == .5 && !cues[0].endSec);
        QCOMPARE(cues[1].number, 1);
        QCOMPARE(*cues[1].endSec, 4.0);
        QCOMPARE(cues[2].number, 4);  // Kind 5 is hot cue D
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
