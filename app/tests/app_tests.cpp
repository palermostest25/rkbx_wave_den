// Wave around the core: Rekordbox's host policy (the Python app's tests, ported case by case), its popups,
// the controller's maps, the updater's choices, and the window itself, driven as a DJ would.
#include "app/session.h"
#include "app/updater.h"
#include "core/bridge.h"
#include "../../native/mac_bridge/bridge_layout.h"
#include "core/library.h"
#include "host/host.h"
#include "host/midi.h"
#include "host/popups.h"
#include "host/setup.h"
#include "host/windows.h"
#include "ui/browser.h"
#include "ui/canvas.h"
#include "ui/mainwindow.h"
#include "ui/widgets.h"

#include <QApplication>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLabel>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QTest>
#include <QListView>
#include <QTableView>

#include <sqlite3.h>

#include <algorithm>
#include <cstring>

using namespace wave;
using host::Action;
using host::State;

namespace {
host::Seen seen(uint32_t pid, State state = State::Absent, std::optional<double> age = {}) {
    host::Seen s;
    s.pid = pid;
    s.state = state;
    s.age = age;
    return s;
}
host::Seen with(host::Seen s, std::function<void(host::Seen &)> f) { f(s); return s; }

const char *kFlx10 = "@file,1,DDJ-FLX10,,,,,,,,,,,,\n"
                     "#name,function,type,input,deck1,deck2,deck3,deck4,output,deck1,deck2,deck3,deck4,option,comment\n"
                     "LoopIn,LoopIn,Button,9010,0,1,2,3,9010,0,1,2,3,Dual;Fast,Loop in/Loop in adjust\n"
                     "Browse,Browse,Rotary,B640,,,,,,,,,,,Browse(Track/Folder Select)\n"
                     "BrowsePress,BrowsePress,Button,9641,,,,,,,,,,,\"Load, or open a folder\"\n"
                     "Back,Back,Button,9665,,,,,,,,,,,Back\n"
                     "View,View,Button,9666,,,,,,,,,,,View\n"
                     "LoadDeck1,Load,Button,9046,0,1,2,3,,,,,,,Load\n"
                     "SamplerVolume,SamplerVolume,Knob,B602,,,,,,,,,,,Sampler volume\n"
                     "Zoom,Browse+Shift,Rotary,B664,,,,,,,,,,,Scaling Waveform for all deck\n"
                     "GetLoopActive,LoopState,Indicator,,,,,,,9F04,9F05,9F06,9F07,RO;Priority=100,\n";
// The browser rows of Rekordbox 7.2.19's own DDJ-FLX10.midi.csv, as it ships.
const char *kFlx10Real = "@file,1,DDJ-FLX10\n"
    "#name,function,type,input,deck1,deck2,deck3,deck4,output,deck1,deck2,deck3,deck4,option,comment\n"
    "Browse,Browse,Rotary,B640,,,,,,,,,,,Browse(Track/Folder Select)\n"
    "Zoom,Browse+Shift,Rotary,B664,,,,,,,,,,,Scaling Waveform for all deck\n"
    "ForwardAndLoad,Browse+Press,Button,,9646,9647,9648,9649,,,,,,,Load to Deck / Instant Double (double click)\n"
    "Back,Back,Button,9665,,,,,,,,,,,Library Back (Folder Close)\n"
    "Area,Back+Shift,Button,9666,,,,,,,,,,,Area (Change panel type from among several types)\n"
    "SwitchActiveWindow,BrowseView,Button,967A,,,,,,,,,,,BROWSE VIEW\n"
    "AddToTagList,BrowseView+LongPress,Button,9667,,,,,,,,,,,Tag Track\n"
    "RelatedTracks,BrowseView+Shift,Button,9668,,,,,,,,,,,Related Track\n"
    "SamplerVolume,SamplerVolume,KnobSliderHiRes,B603,,,,,,,,,,,Sampler Volume\n"
    "LoadedIndicator,LoadedIndicator,Indicator,,,,,,,9F00,9F01,9F02,9F03,RO;Priority=100,Trigger for Load illumination\n";
}  // namespace

class AppTests : public QObject {
    Q_OBJECT

private slots:
    void initTestCase() { qputenv("RKBX_WAVE_CONFIG_DIR", QDir::tempPath().toUtf8() + "/wave-app-tests"); }

    // --- Rekordbox behind Wave ----------------------------------------------------------------
    void startsRekordboxWhenWaveStarts() {
        host::Policy p(false);
        QCOMPARE(p.decide(seen(0), 0), Action::Launch);
        QCOMPARE(p.decide(seen(0), 5), Action::Nothing);  // starting: no second launch
        QCOMPARE(p.decide(seen(7, State::None, 3), 10), Action::Nothing);  // its bridge arrives with OpenSSL's config
        QCOMPARE(p.decide(seen(7, State::Starting, 8), 15), Action::Nothing);
        QCOMPARE(p.decide(seen(7, State::Live, 20), 25), Action::Nothing);
        QVERIFY(p.ask.isEmpty());
    }

    void neverStartsRekordboxWhenToldNotTo() {
        host::Policy p(false);
        p.autoStart = false;
        QCOMPARE(p.decide(seen(0), 0), Action::Nothing);
        QCOMPARE(p.ask, QString::fromLatin1(host::kAskStart));
    }

    void windowsConnectsLiveToARekordboxStartedWithoutWave() {
        host::Policy p(true);
        const auto busy = with(seen(7, State::None, 3600), [](host::Seen &s) { s.playing = s.loaded = s.known = true; });
        QCOMPARE(p.decide(busy, 0), Action::Attach);
        QCOMPARE(p.decide(busy, 1), Action::Nothing);  // tried once: then never restarted under a playing deck
        QCOMPARE(p.ask, QString::fromLatin1(host::kAskConnect));
        host::Policy q(true);
        QVERIFY(q.decide(with(busy, [](host::Seen &s) { s.bridgeLoaded = true; }), 0) != Action::Attach);  // never a second bridge
    }

    void windowsConnectsLiveWhenItsOwnStartBroughtNoBridge() {
        host::Policy p(true);
        QCOMPARE(p.decide(seen(0), 0), Action::Launch);
        QCOMPARE(p.decide(seen(7, State::None, 10), 10), Action::Nothing);
        QCOMPARE(p.decide(seen(7, State::None, 35), 35), Action::Attach);
    }

    void restartsOnlyWhatIsSafeToRestart() {
        host::Policy mac(false);
        mac.first = false;
        QCOMPARE(mac.decide(with(seen(7, State::None, 600), [](host::Seen &s) { s.known = true; }), 0), Action::Restart);
        QCOMPARE(mac.decide(seen(8, State::None, 2), 1), Action::Nothing);  // the restarted one, starting
        host::Policy fresh(false);
        fresh.first = false;
        QCOMPARE(fresh.decide(seen(7, State::None, 20), 0), Action::Restart);  // a fresh one, even unseen
        host::Policy blind(false);
        blind.first = false;
        QCOMPARE(blind.decide(seen(7, State::None, 900), 0), Action::Nothing);  // can't see in: never alone
        QCOMPARE(blind.ask, QString::fromLatin1(host::kAskConnect));
        for (auto s : {with(seen(7, State::None, 900), [](host::Seen &x) { x.known = x.loaded = true; }),
                       with(seen(7, State::Older, 900), [](host::Seen &x) { x.known = x.playing = true; }),
                       with(seen(7, State::None, 20), [](host::Seen &x) { x.loaded = true; })}) {
            host::Policy p(false);
            p.first = false;
            QCOMPARE(p.decide(s, 0), Action::Nothing);  // a loaded or playing Rekordbox waits for the DJ
            QCOMPARE(p.ask, QString::fromLatin1(host::kAskConnect));
        }
    }

    void anIdleRekordboxWithAnEarlierBridgeIsRestartedOnce() {
        host::Policy p(false);
        const auto stale = with(seen(7, State::Live, 900), [](host::Seen &s) { s.stale = s.known = true; });
        QCOMPARE(p.decide(stale, 0), Action::Restart);
        p.launched(8);
        QCOMPARE(p.decide(with(seen(8, State::Live, 5), [](host::Seen &s) { s.stale = s.known = true; }), 30), Action::Nothing);
        host::Policy q(false);
        QCOMPARE(q.decide(with(stale, [](host::Seen &s) { s.loaded = true; }), 0), Action::Nothing);
        QVERIFY(q.ask.contains("earlier Wave"));
    }

    void rekordboxClosingUnderWaveIsStartedAgainWithALimit() {
        host::Policy p(false);
        p.decide(seen(7, State::Live), 0);
        QCOMPARE(p.decide(seen(0), 10), Action::Launch);
        int n = 0;
        for (double start : {200.0, 400.0}) {
            p.decide(seen(uint32_t(10 + n++), State::Live), start);
            QCOMPARE(p.decide(seen(0), start + 10), Action::Launch);
        }
        p.decide(seen(20, State::Live), 450);
        QCOMPARE(p.decide(seen(0), 460), Action::Nothing);  // three in ten minutes: crashing, stop
        QCOMPARE(p.ask, QString::fromLatin1(host::kAskGaveUp));
        host::Policy quit(false);
        quit.decide(seen(7, State::Live), 0);
        quit.quitting = true;
        QCOMPARE(quit.decide(seen(0), 10), Action::Nothing);
        host::Policy unused(false);
        unused.autoStart = false;
        unused.decide(seen(7, State::Unsupported), 0);
        QCOMPARE(unused.decide(seen(0), 10), Action::Nothing);
        host::Policy unsupported(true);
        QCOMPARE(unsupported.decide(seen(7, State::Unsupported, 999), 0), Action::Nothing);
        QVERIFY(unsupported.ask.isEmpty());
    }

    void rekordboxsWindowsAreToldApart() {
        auto win = [](quintptr h, const char *title, QRect r, bool owned = false, bool resizable = true) {
            host::Win w;
            w.hwnd = h; w.title = title; w.visible = true; w.rect = r; w.owned = owned; w.resizable = resizable;
            return w;
        };
        const auto c = host::classify({win(1, "rekordbox", {0, 0, 1600, 900}), win(2, "Browser", {0, 0, 1000, 700}, true),
                                       win(3, "", {0, 0, 400, 200}, true, false)});
        QCOMPARE(c.main->hwnd, quintptr(1));
        QCOMPARE(int(c.panels.size()), 1);
        QCOMPARE(c.panels[0].hwnd, quintptr(2));
        QCOMPARE(int(c.popups.size()), 1);
        QCOMPARE(c.popups[0].hwnd, quintptr(3));
    }

    // --- Rekordbox's popups --------------------------------------------------------------------
    void noticesAreClosedAndQuestionsStay() {
        auto popup = [](const QString &title, QStringList texts, QStringList buttons) {
            host::Popup p;
            p.title = title; p.texts = texts; p.buttons = buttons;
            return p;
        };
        QCOMPARE(*host::dismissWith(popup("rekordbox", {"The USB device was removed."}, {"OK"})), QStringLiteral("OK"));
        QCOMPARE(*host::dismissWith(popup("", {"A new version is available"}, {"Update now", "Later"})), QStringLiteral("Later"));
        QCOMPARE(*host::dismissWith(popup("", {"Try rekordbox Creative plan"}, {"Learn more", "Not now"})), QStringLiteral("Not now"));
        QCOMPARE(*host::dismissWith(popup("What's new", {"Stems, cloud and more"}, {"Learn more", "Close"})), QStringLiteral("Close"));
        QVERIFY(!host::dismissWith(popup("", {"Analyse 30 tracks?"}, {"Analyse", "Close"})));
        QCOMPARE(*host::dismissWith(popup("Sync with My Tag",
                                          {"My Tag in this device is synced with another computer. Do you want to delete My Tag stored "
                                           "in the device and sync with My Tag in this computer?"},
                                          {"Sync with My Tag", "Do not sync with My Tag"})),
                 QStringLiteral("Do not sync with My Tag"));
        QCOMPARE(*host::dismissWith(popup("rekordbox", {"rekordbox 7.2.20 is available. Do you want to quit rekordbox and update now?"},
                                          {"Update", "Cancel"})),
                 QStringLiteral("Cancel"));
        QVERIFY(!host::dismissWith(popup("", {"The library needs an update. Update the database now?"}, {"Update", "Cancel"})));
        QCOMPARE(*host::dismissWith(popup("rekordbox", {"The device was removed while rekordbox was accessing to it. The database in the "
                                                         "device might be damaged."}, {"OK"})),
                 QStringLiteral("OK"));
        for (const auto &p : {popup("", {"Sync your library now?"}, {"Sync", "Cancel"}), popup("", {"Learn more about plans"}, {"Learn more"}),
                              popup("Sign in", {"Log in to your account"}, {"Later"}),
                              popup("", {"The database is damaged: restore the backup?"}, {"OK"}), popup("", {"Something"}, {}),
                              popup("", {"Activate your licence"}, {"OK"})})
            QVERIFY2(!host::dismissWith(p), qPrintable(p.summary()));
        QCOMPARE(popup("rekordbox", {"Quit?"}, {}).summary(), QStringLiteral("Quit?"));
    }

    // --- The controller ------------------------------------------------------------------------
    void rekordboxsMapsGiveTheBrowserControls() {
        const auto [name, table] = midi::loadMap(QString::fromLatin1(kFlx10), "DDJ-FLX10.midi.csv");
        QCOMPARE(name, QStringLiteral("DDJ-FLX10"));
        auto kind = [&](int status, int data) { return table.count({status, data}) ? int(table.at({status, data}).kind) : -1; };
        QCOMPARE(kind(0xB6, 0x40), int(midi::Kind::Browse));
        QCOMPARE(kind(0xB6, 0x64), int(midi::Kind::Zoom));  // SHIFT + BROWSE
        QCOMPARE(kind(0x96, 0x41), int(midi::Kind::Press));
        QCOMPARE(kind(0x96, 0x65), int(midi::Kind::Back));
        QCOMPARE(kind(0x96, 0x66), int(midi::Kind::View));
        QCOMPARE(kind(0xB6, 0x02), int(midi::Kind::SamplerZoom));
        QCOMPARE(kind(0x92, 0x46), int(midi::Kind::Load));
        QCOMPARE(table.at({0x92, 0x46}).deck, 2);  // each deck's LOAD, by its channel offset
        QCOMPARE(kind(0x90, 0x10), -1);  // loop buttons are Rekordbox's alone
        QCOMPARE(int(midi::browseAction("LoadDeck3", 0x96)->deck), 2);
        QVERIFY(!midi::browseAction("SamplerVolume", 0x96));  // a button is not the knob
        QVERIFY(!midi::browseAction("Browse+Shift", 0xB6));
        QVERIFY(!midi::browseAction("Preview", 0x96));
        QVERIFY(!midi::browseAction("PlayPause", 0x90));
    }

    void theMostSpecificMapIsChosenAndTheStreamDecoded() {
        std::vector<std::pair<QString, midi::Table>> maps = {{"DDJ-FLX10", {}}, {"DDJ-FLX1", {}}, {"XDJ", {}}};
        QCOMPARE(midi::findMap("DDJ-FLX10 MIDI 1", maps)->first, QStringLiteral("DDJ-FLX10"));
        QCOMPARE(midi::findMap("Pioneer DJ DDJ-FLX1", maps)->first, QStringLiteral("DDJ-FLX1"));
        QVERIFY(!midi::findMap("Launchpad", maps));
        const auto m = midi::messages(QByteArray::fromHex("f0010203f7" "b6407e" "4101" "fe" "96417f"));
        QCOMPARE(int(m.size()), 3);  // sysex skipped, running status kept, realtime ignored
        QCOMPARE(m[0][0], 0xB6);
        QCOMPARE(m[1][1], 0x41);  // running status: B6 41 ...
    }

    void knobTurnsAndPressesReachWave() {
        midi::Listener listener;
        midi::Table table = midi::loadMap(QString::fromLatin1(kFlx10), "x.midi.csv").second;
        listener.addDevice(1, "DDJ-FLX10", table);
        QSignalSpy spy(&listener, &midi::Listener::controller);
        listener.receive(1, QByteArray::fromHex("b6407e" "96417f" "964100" "92467f" "b60240"));
        QCOMPARE(spy.count(), 4);  // the release is not a second press
        QCOMPARE(spy[0][1].toInt(), -2);  // 0x7E: two steps back
        QCOMPARE(spy[1][0].value<midi::Kind>(), midi::Kind::Press);
        QCOMPARE(spy[2][2].toInt(), 2);  // LOAD on deck 3
        QCOMPARE(spy[3][1].toInt(), 0x40);  // the sampler knob, absolute
        QVERIFY(listener.controls().count(midi::Kind::View));
    }

    void rekordboxsOwnFlx10MapIsReadAsItShips() {
        const auto [name, table] = midi::loadMap(QString::fromLatin1(kFlx10Real), "DDJ-FLX10.midi.csv");
        QCOMPARE(name, QStringLiteral("DDJ-FLX10"));
        auto at = [&](int status, int data) { return table.count({status, data}) ? std::optional(table.at({status, data})) : std::nullopt; };
        QCOMPARE(int(at(0xB6, 0x40)->kind), int(midi::Kind::Browse));
        QCOMPARE(int(at(0xB6, 0x64)->kind), int(midi::Kind::Zoom));
        for (int deck = 0; deck < 4; ++deck) {  // each deck's LOAD: a full code in the deck's own column
            QVERIFY(at(0x96, 0x46 + deck));
            QCOMPARE(int(at(0x96, 0x46 + deck)->kind), int(midi::Kind::Press));
            QCOMPARE(at(0x96, 0x46 + deck)->deck, deck);
        }
        QCOMPARE(int(at(0x96, 0x65)->kind), int(midi::Kind::Back));
        QCOMPARE(int(at(0x96, 0x7A)->kind), int(midi::Kind::View));  // BROWSE VIEW
        QCOMPARE(int(at(0xB6, 0x03)->kind), int(midi::Kind::SamplerZoom));
        QVERIFY(!at(0x96, 0x66));  // SHIFT + BACK, VIEW held and SHIFT + VIEW are Rekordbox's alone
        QVERIFY(!at(0x96, 0x67));
        QVERIFY(!at(0x96, 0x68));
        QVERIFY(!at(0x9F, 0x00));  // an indicator, out
        midi::Listener listener;
        listener.addDevice(1, "DDJ-FLX10", table);
        QSignalSpy spy(&listener, &midi::Listener::controller);
        listener.receive(1, QByteArray::fromHex("967a7f" "967a00" "96487f"));
        QCOMPARE(spy.count(), 2);
        QCOMPARE(spy[0][0].value<midi::Kind>(), midi::Kind::View);
        QCOMPARE(spy[1][2].toInt(), 2);
        // What Wave keeps from Rekordbox: the knob, each deck's push, BACK and VIEW; never the zooms or SHIFT layers.
        const auto keys = midi::Listener::keys(table);
        auto kept = [&](int status, int data1) { return std::find(keys.begin(), keys.end(), std::make_pair(status, data1)) != keys.end(); };
        QVERIFY(kept(0xB6, 0x40));
        for (int d = 0x46; d <= 0x49; ++d) QVERIFY(kept(0x96, d));
        QVERIFY(kept(0x96, 0x65));
        QVERIFY(kept(0x96, 0x7A));
        QVERIFY(!kept(0xB6, 0x64));  // SHIFT + BROWSE: Rekordbox zooms too
        QVERIFY(!kept(0xB6, 0x03));  // the sampler volume stays the sampler's
        QVERIFY(!kept(0x96, 0x66));
        // Heard through Rekordbox: whole messages, decoded with every connected controller's map.
        spy.clear();
        listener.receiveMessage(listener.merged(), 0xB6, 0x40, 0x01);
        listener.receiveMessage(listener.merged(), 0x96, 0x47, 0x7F);
        listener.receiveMessage(listener.merged(), 0x96, 0x47, 0x00);
        QCOMPARE(spy.count(), 2);
        QCOMPARE(spy[0][1].toInt(), 1);
        QCOMPARE(spy[1][2].toInt(), 1);  // the push on deck 2
    }

    void theBridgesControllerInputIsReadOnceAndKeepsWhatWaveAsks() {
        QTemporaryDir dir;
        const QString path = dir.filePath("state.bin");
        QFile file(path);
        QVERIFY(file.open(QIODevice::ReadWrite));
        file.resize(sizeof(BridgeState));
        auto *state = reinterpret_cast<BridgeState *>(file.map(0, sizeof(BridgeState)));
        QVERIFY(state);
        std::memset(state, 0, sizeof *state);
        std::memcpy(state->magic, kBridgeMagic, sizeof kBridgeMagic);
        state->version = kBridgeVersion;
        state->pid = 42;
        state->status = kStatusLive;
        state->uptime_ns = bridge::uptimeNs();
        state->midi.hooked = 1;
        uint32_t seq = 0;
        auto event = [&](int status, int data1, int data2) {
            ++seq;
            BridgeMidiEvent &e = state->midi.events[(seq - 1) % kMidiEvents];
            e.status = uint8_t(status); e.data1 = uint8_t(data1); e.data2 = uint8_t(data2);
            e.seq = seq;
            state->midi.written = seq;
        };
        event(0xB6, 0x40, 0x01);  // before Wave looked: not Wave's
        bridge::Tap tap(path);
        QVERIFY(tap.take().empty());
        QVERIFY(tap.hooked());
        event(0xB6, 0x40, 0x7F);
        event(0x96, 0x46, 0x7F);
        auto got = tap.take();
        QCOMPARE(int(got.size()), 2);
        QCOMPARE(got[0][2], 0x7F);
        QCOMPARE(got[1][1], 0x46);
        QVERIFY(tap.take().empty());  // each once
        for (int i = 0; i < int(kMidiEvents) + 10; ++i) event(0xB6, 0x40, 1);  // Wave fell behind: the newest ring
        QCOMPARE(int(tap.take().size()), int(kMidiEvents));
        // A half-written event waits for its number.
        BridgeMidiEvent &next = state->midi.events[seq % kMidiEvents];
        state->midi.written = seq + 1;
        next.status = 0x96;
        QVERIFY(tap.take().empty());
        next.seq = seq + 1; ++seq;
        QCOMPARE(int(tap.take().size()), 1);
        // Keeping: the controls, a note-off as its note-on, and a fresh heartbeat; nothing kept clears both.
        tap.keep({{0xB6, 0x40}, {0x86, 0x46}});
        QCOMPARE(state->midi.keep_count, 2u);
        QCOMPARE(int(state->midi.keep[0]), 0xB640);
        QCOMPARE(int(state->midi.keep[1]), 0x9646);
        QVERIFY(state->midi.wave_ns > 0 && bridge::uptimeNs() - state->midi.wave_ns < 1000000000ULL);
        tap.keep({});
        QCOMPARE(state->midi.keep_count, 0u);
        QCOMPARE(state->midi.wave_ns, uint64_t(0));
        // A bridge that stopped (no heartbeat), or doesn't hear the controller, is not heard through.
        state->midi.hooked = 0;
        tap.take();
        QVERIFY(!tap.hooked());
        file.unmap(reinterpret_cast<uchar *>(state));
    }

    // --- Updates and setup -----------------------------------------------------------------------
    void onlyANewerChecksummedGitHubReleaseIsOffered() {
        QCOMPARE(versionNumbers("v3.0.10"), (QList<int>{3, 0, 10}));
        QVERIFY(versionNumbers("v3.0-beta").isEmpty());
        QVERIFY(versionNumbers("2.10") > versionNumbers("2.9"));
        auto release = [](const QString &tag, const QString &url, const QString &digest, bool draft = false) {
            QJsonObject asset{{"name", "Rekordbox-Wave-Windows-x64.exe"}, {"browser_download_url", url}, {"digest", digest}, {"size", 1234}};
            return QJsonDocument(QJsonObject{{"tag_name", tag}, {"draft", draft}, {"assets", QJsonArray{asset}},
                                             {"html_url", "https://github.com/palermostest25/rkbx_wave_den/releases/tag/" + tag}}).toJson();
        };
        const QString good = "https://github.com/palermostest25/rkbx_wave_den/releases/download/v3.1.0/Rekordbox-Wave-Windows-x64.exe";
        const QString sha = "sha256:" + QString(64, 'a');
        const auto u = newerIn(release("v3.1.0", good, sha), "3.0.0", "Rekordbox-Wave-Windows-x64.exe");
        QVERIFY(u);
        QCOMPARE(u->version, QStringLiteral("3.1.0"));
        QCOMPARE(u->sha256, QString(64, 'a'));
        QCOMPARE(u->size, 1234);
        QVERIFY(!newerIn(release("v3.0.0", good, sha), "3.0.0", "Rekordbox-Wave-Windows-x64.exe"));  // not newer
        QVERIFY(!newerIn(release("v3.1.0", good, sha, true), "3.0.0", "Rekordbox-Wave-Windows-x64.exe"));  // a draft
        QVERIFY(!newerIn(release("v3.1.0", "https://example.com/x.exe", sha), "3.0.0", "Rekordbox-Wave-Windows-x64.exe"));
        QVERIFY(!newerIn(release("v3.1.0", good, "md5:abc"), "3.0.0", "Rekordbox-Wave-Windows-x64.exe"));
        QVERIFY(!newerIn(release("v3.1.0", good, sha), "3.0.0", "Rekordbox-Wave-macOS-Intel.dmg"));  // not this computer's
        QVERIFY(newerIn(release("v3.0.0", good, sha), "2.6.4", "Rekordbox-Wave-Windows-x64.exe"));  // the Python Wave moves on
    }

    void rekordboxsInstallerIsFoundOnItsOwnSiteOnly() {
        const QString name = host::installerName();
        QString stem = name;
        stem.remove(QRegularExpression("(\\.pkg)?\\.zip$"));
        const QString page = QStringLiteral("<a href=\"https://cdn.rekordbox.com/files/20250101/%1.zip\">x</a> <a href=\"https://evil.example/%1.zip\">").arg(stem);
        QCOMPARE(host::downloadFromPage(page), QStringLiteral("https://cdn.rekordbox.com/files/20250101/%1.zip").arg(stem));
        QVERIFY(host::downloadFromPage("<a href=\"https://evil.example/" + stem + ".zip\">").isEmpty());
        QVERIFY(host::trustedSigner("CN=AlphaTheta Corporation, O=AlphaTheta Corporation"));
        QVERIFY(host::trustedSigner("CN=Pioneer DJ Corporation"));
        QVERIFY(!host::trustedSigner("CN=Someone Else"));
        QVERIFY(!host::trustedSigner(""));
    }

    // --- The window --------------------------------------------------------------------------------
    void theDeckScreenDrawsOnlyWhileSomethingMoves() {
        QTemporaryDir dir;
        Session session(dir.filePath("settings.json"), true);
        MainWindow window(&session);
        window.resize(1280, 800);
        window.show();
        QVERIFY(QTest::qWaitForWindowExposed(&window));
        QVERIFY(session.animating());  // the demo decks play
        QImage shot = window.grab().toImage();
        QVERIFY(!shot.isNull());
        // A waveform is drawn in the first lane: its colours, not just black.
        int coloured = 0;
        for (int x = 100; x < 1200; x += 7) if (qRed(shot.pixel(x, 120)) > 150) ++coloured;
        QVERIFY2(coloured > 20, qPrintable(QString::number(coloured)));
    }

    void theControllerDrivesWavesBrowserWhenRekordboxDoesnt() {
        QTemporaryDir dir;
        const QString path = dir.filePath("master.db");
        sqlite3 *db = nullptr;
        sqlite3_open(path.toUtf8().constData(), &db);
        const QByteArray key = databaseKey(false);
        sqlite3_key(db, key.constData(), int(key.size()));
        sqlite3_exec(db, R"(
          CREATE TABLE djmdContent(ID TEXT,Title TEXT,ArtistID TEXT,KeyID TEXT,BPM INTEGER,Length INTEGER,FolderPath TEXT,rb_local_deleted INTEGER);
          CREATE TABLE djmdArtist(ID TEXT,Name TEXT);
          CREATE TABLE djmdKey(ID TEXT,ScaleName TEXT);
          INSERT INTO djmdContent VALUES('1','Ten','','',10000,300,'/ten.wav',0);
          INSERT INTO djmdContent VALUES('2','Nine','','',9000,90,'/nine.wav',0);
        )", nullptr, nullptr, nullptr);
        sqlite3_close(db);
        qputenv("RKBX_WAVE_MASTER_DB", path.toUtf8());
        Session session(dir.filePath("settings.json"), true);
        session.settings.setView("browser", "wave");
        MainWindow window(&session);
        window.resize(1280, 800);
        window.show();
        midi::Listener controller;
        window.useController(&controller);
        controller.setKeeping(true);
        controller.assumeThroughRekordbox(true);  // the bridge keeps the knob from Rekordbox
        QVERIFY(controller.keeping());
        BrowserPage *page = window.browser();
        QSignalSpy loads(page, &BrowserPage::loadRequested);
        auto *table = page->findChild<QTableView *>();
        QTRY_COMPARE(table->model()->rowCount(), 2);
        // The knob turned on the waveforms: Wave's browser opens; turned again, its highlight moves.
        emit controller.controller(midi::Kind::Browse, 1, -1);
        QTRY_VERIFY(page->isVisible());
        emit controller.controller(midi::Kind::Browse, 1, -1);
        QTRY_VERIFY(page->selectedTrack().has_value());
        QCOMPARE(page->selectedTrack()->title, QStringLiteral("Nine"));
        emit controller.controller(midi::Kind::Browse, 1, -1);
        QTRY_COMPARE(page->selectedTrack()->title, QStringLiteral("Ten"));
        // The push on deck 2: Wave's pick goes onto deck 2 (Rekordbox never heard it). Demo decks refuse loads, so
        // the decks are opened up for this moment, as a live Rekordbox's are.
        page->setDecks({0, 1, 2, 3}, {}, {}, {});
        emit controller.controller(midi::Kind::Press, 0, 1);
        QCoreApplication::processEvents();
        QCOMPARE(loads.count(), 1);
        QCOMPARE(loads[0][0].toInt(), 1);
        QCOMPARE(loads[0][1].value<CatalogTrack>().title, QStringLiteral("Ten"));
        // Not kept (Rekordbox's browser has the controls): the push is Rekordbox's to load, never Wave's too.
        controller.setKeeping(false);
        page->setDecks({0, 1, 2, 3}, {}, {}, {});
        emit controller.controller(midi::Kind::Press, 0, 1);
        QCoreApplication::processEvents();
        QCOMPARE(loads.count(), 1);
        // VIEW closes the browser.
        emit controller.controller(midi::Kind::View, 0, -1);
        QTRY_VERIFY(!page->isVisible());
        page->shutdown();
        qunsetenv("RKBX_WAVE_MASTER_DB");
    }

    void aStickIsFoundHoweverRekordboxWritesItsDrive() {
        QCOMPARE(rootKey("E:"), QStringLiteral("e:"));
        QCOMPARE(rootKey("E:\\"), QStringLiteral("e:"));
        QCOMPARE(rootKey("e:/"), QStringLiteral("e:"));
        QCOMPARE(rootKey("\\\\?\\E:\\"), QStringLiteral("e:"));
        QCOMPARE(rootKey("/Volumes/USB/"), QStringLiteral("/volumes/usb"));
        QCOMPARE(rootKey("/"), QStringLiteral("/"));
        const std::vector<std::pair<int64_t, QString>> devices{{7, "D:"}, {9, "E:\\"}};
        QCOMPARE(deviceHandle("E:/", devices), int64_t(9));
        QCOMPARE(deviceHandle("d:/", devices), int64_t(7));
        QCOMPARE(deviceHandle("F:/", devices), int64_t(0));
        QCOMPARE(deviceHandle("/Volumes/USB", {{3, "/Volumes/USB/"}}), int64_t(3));
        QCOMPARE(deviceHandle("E:/", {}), int64_t(0));
        // A stick's own path for a file on it: at a drive's root (Windows), or in a folder (macOS).
        QCOMPARE(stickPath("E:/", "E:/Contents/Fred again../a.m4a"), QStringLiteral("/Contents/Fred again../a.m4a"));
        QCOMPARE(stickPath("E:/", "e:\\Contents\\a.m4a"), QStringLiteral("/Contents/a.m4a"));
        QCOMPARE(stickPath("E:", "E:/Contents/a.m4a"), QStringLiteral("/Contents/a.m4a"));
        QCOMPARE(stickPath("/Volumes/USB", "/Volumes/USB/Contents/a.m4a"), QStringLiteral("/Contents/a.m4a"));
        QVERIFY(stickPath("E:/", "F:/Contents/a.m4a").isEmpty());
        QVERIFY(stickPath("/Volumes/USB", "/Volumes/USB2/a.m4a").isEmpty());
    }

    void controlsKeepTheirSettings() {
        QTemporaryDir dir;
        Session session(dir.filePath("settings.json"), true);
        MainWindow window(&session);
        window.setView("keyboard", "windows");
        window.setView("tap_to_open", "single");
        window.setCount(2);
        window.zoomBy(2);
        Settings again;
        window.persist();
        QVERIFY(again.load(dir.filePath("settings.json")));
        QCOMPARE(again.text("keyboard"), QStringLiteral("windows"));
        QCOMPARE(again.text("tap_to_open"), QStringLiteral("single"));
        QCOMPARE(again.deckCount(), 2);
        QCOMPARE(again.number("zoom_beats"), 64.0);
        window.zoomBy(1000);
        QCOMPARE(session.zoomBeats(), 512.0);  // 4 to 512 beats across a lane
    }

    void theSamplerKnobZoomsAroundTheSetting() {
        QTemporaryDir dir;
        Session session(dir.filePath("settings.json"), true);
        session.settings.setView("zoom_beats", 32);
        QCOMPARE(session.knobZoom(64, false), 32.0);  // the middle: the zoom set in Settings
        QCOMPARE(session.knobZoom(127, false), 4.0);  // clockwise: in, to a bar
        QCOMPARE(session.knobZoom(0, false), 128.0);  // and out, to 32 bars
        QCOMPARE(session.knobZoom(127, true), 128.0);  // reversed
        session.endKnobZoom();
        QCOMPARE(session.zoomBeats(), 32.0);
    }

    void segmentedChoicesPickTheNearestNumber() {
        Segmented s({{"A", .8}, {"B", 1.0}, {"C", 1.25}}, 1.1);
        QCOMPARE(s.value().toDouble(), 1.0);
        QSignalSpy spy(&s, &Segmented::changed);
        s.setValue(1.25, true);
        QCOMPARE(spy.count(), 1);
        Segmented t({{"Wave", "wave"}, {"Windows", "windows"}}, "windows");
        QCOMPARE(t.value().toString(), QStringLiteral("windows"));
    }

    void theBrowserDrillsDownStepsAndLoads() {
        QTemporaryDir dir;
        const QString path = dir.filePath("master.db");
        sqlite3 *db = nullptr;
        sqlite3_open(path.toUtf8().constData(), &db);
        const QByteArray key = databaseKey(false);
        sqlite3_key(db, key.constData(), int(key.size()));
        sqlite3_exec(db, R"(
          CREATE TABLE djmdContent(ID TEXT,Title TEXT,ArtistID TEXT,KeyID TEXT,BPM INTEGER,Length INTEGER,FolderPath TEXT,rb_local_deleted INTEGER);
          CREATE TABLE djmdArtist(ID TEXT,Name TEXT);
          CREATE TABLE djmdKey(ID TEXT,ScaleName TEXT);
          INSERT INTO djmdArtist VALUES('a','Artist');
          INSERT INTO djmdKey VALUES('k','8A');
          INSERT INTO djmdContent VALUES('1','Ten','a','k',10000,300,'/ten.wav',0);
          INSERT INTO djmdContent VALUES('2','Nine','a','k',9000,90,'/nine.wav',0);
          INSERT INTO djmdContent VALUES('3','Eight','a','k',12800,240,'/eight.wav',0);
          CREATE TABLE djmdPlaylist(ID TEXT,Seq INTEGER,Name TEXT,Attribute INTEGER,ParentID TEXT);
          CREATE TABLE djmdSongPlaylist(PlaylistID TEXT,ContentID TEXT,TrackNo INTEGER);
          INSERT INTO djmdPlaylist VALUES('f',1,'Gigs',1,'root');
          INSERT INTO djmdPlaylist VALUES('p',1,'Friday',0,'f');
          INSERT INTO djmdSongPlaylist VALUES('p','2',1);
          INSERT INTO djmdSongPlaylist VALUES('p','1',2);
        )", nullptr, nullptr, nullptr);
        sqlite3_close(db);
        qputenv("RKBX_WAVE_MASTER_DB", path.toUtf8());
        BrowserPage page;
        page.resize(1200, 800);
        page.show();
        auto *table = page.findChild<QTableView *>();
        QTRY_COMPARE(table->model()->rowCount(), 3);  // TRACK: the whole collection, by title
        QVERIFY(!page.selectedTrack());
        QCOMPARE(page.step(1), 0);  // the knob highlights the first track
        QCOMPARE(page.selectedTrack()->title, QStringLiteral("Eight"));
        page.step(5);
        QCOMPARE(page.selectedTrack()->title, QStringLiteral("Ten"));  // and stops at the last
        QSignalSpy loads(&page, &BrowserPage::loadRequested);
        page.setDecks({0, 1}, {{0, ""}, {1, "Deck 2 is playing"}}, {}, {});
        // Into the playlists: double tap opens, a single tap only highlights.
        page.setOpenOn("double");
        QVERIFY(page.findChild<QListView *>());
        const auto nav = page.findChildren<QAbstractButton *>();
        for (QAbstractButton *b : nav) if (b->accessibleName() == QLatin1String("Browse by playlist")) b->click();
        auto *list = page.findChild<QListView *>();
        QTRY_COMPARE(list->model()->rowCount(), 1);
        QVERIFY(page.showsList());
        QCOMPARE(page.step(1), 0);
        QVERIFY(page.enter());  // the knob's push opens the folder
        QTRY_COMPARE(list->model()->rowCount(), 1);
        QCOMPARE(list->model()->index(0, 0).data().toString(), QStringLiteral("Friday"));
        QVERIFY(!page.selectedTrack());  // nothing is loadable from a list of playlists
        page.step(1);
        QVERIFY(page.enter());
        QTRY_COMPARE(table->model()->rowCount(), 2);
        QCOMPARE(table->model()->index(0, 1).data().toString(), QStringLiteral("Nine"));  // the playlist's own order
        page.step(1);
        QCOMPARE(page.selectedTrack()->title, QStringLiteral("Nine"));
        QVERIFY(page.goBack());  // BACK: up to Gigs
        QTRY_COMPARE(list->model()->rowCount(), 1);
        QVERIFY(page.goBack());  // and to the top
        QTRY_COMPARE(list->model()->rowCount(), 1);
        QCOMPARE(list->model()->index(0, 0).data().toString(), QStringLiteral("Gigs"));
        QVERIFY(!page.goBack());  // the top: BACK leaves the browser
        // The playlist Rekordbox shows, found by its tracks.
        bool located = false;
        page.locate({}, {2, 1}, 2, [&](std::optional<Located> found) { located = found && found->name == QLatin1String("Friday"); });
        QTRY_VERIFY(located);
        page.shutdown();
        qunsetenv("RKBX_WAVE_MASTER_DB");
    }

    void streamedTracksAreNamedByTheirService() {
        QCOMPARE(streamingService("spotify:track:4uLU6hMCjMI75M1A2tKUQC"), QStringLiteral("Spotify"));
        QCOMPARE(streamingService("https://api.beatport.com/v4/catalog/tracks/123"), QStringLiteral("Beatport"));
        QCOMPARE(streamingService("tidal:track:1"), QStringLiteral("TIDAL"));
        QCOMPARE(streamingService("/v4/catalog/tracks/20817385/"), QStringLiteral("Beatport"));  // as Rekordbox 7 names it on Windows
        QVERIFY(streamingService("C:/Music/Spotify Hits/track.mp3").isEmpty());  // a file is a file
        QVERIFY(streamingService("/Volumes/USB/Contents/a.mp3").isEmpty());
    }

    void streamingTilesAskForRekordboxsBrowser() {
        BrowserPage page;
        QSignalSpy spy(&page, &BrowserPage::streamingRequested);
        for (QAbstractButton *b : page.findChildren<QAbstractButton *>())
            if (b->accessibleName() == QLatin1String("Spotify in Rekordbox")) b->click();
        QCOMPARE(spy.count(), 1);
        QCOMPARE(spy[0][0].toString(), QStringLiteral("Spotify"));
        page.shutdown();
    }

    void timesAndLoopsReadAsOnTheDecks() {
        QCOMPARE(clockText(83.47), QStringLiteral("01:23.4"));
        QCOMPARE(clockText(83.47, false), QStringLiteral("01:23"));
        QCOMPARE(clockText(-3), QStringLiteral("00:00.0"));
        QCOMPARE(beatLabel(.25), QStringLiteral("1/4"));
        QCOMPARE(beatLabel(4.02), QStringLiteral("4"));
        QCOMPARE(beatLabel(3.5), QStringLiteral("3.5"));
    }
};

QTEST_MAIN(AppTests)
#include "app_tests.moc"
