// Rekordbox as Wave's engine: started, connected and kept running behind Wave, without being asked.
// ``Policy`` decides (pure logic, tested); ``Host`` carries the decisions out, one at a time, off the UI
// thread. The policy never restarts a Rekordbox that might be playing: when Wave can't see the decks it
// waits for the DJ, and Rekordbox itself still asks before quitting with a deck playing.
#pragma once

#include <QFutureWatcher>
#include <QObject>
#include <QString>

#include <cstdint>
#include <functional>
#include <optional>
#include <set>
#include <vector>

namespace wave::host {

// What the bridge in the running Rekordbox is doing, as the reader reports it.
enum class State { Absent, None, Starting, Live, Busy, Older, Unsupported, Failed };
enum class Action { Nothing, Launch, Attach, Restart };

extern const char *const kAskStart, *const kAskConnect, *const kAskGaveUp, *const kAskUpdate;

struct Seen {
    uint32_t pid = 0;
    State state = State::Absent;
    bool stale = false;            // live, but with an earlier Wave's bridge build
    bool playing = false;          // any deck
    bool loaded = false;           // any deck has a track
    bool known = false;            // Wave can see the decks
    std::optional<double> age;     // seconds since this Rekordbox started
    bool bridgeLoaded = false;     // Windows: some Wave bridge library is already inside it
};

class Policy {
public:
    static constexpr double kStartGrace = 75, kFresh = 90, kRelaunchWindow = 600;
    static constexpr int kRelaunches = 3;

    explicit Policy(bool windows);
    Action decide(const Seen &seen, double now);
    void launched(uint32_t pid) { if (pid) owned.insert(pid); }
    bool starting(double now) const { return launchedAt && now - *launchedAt < kStartGrace; }

    bool windows, autoStart = true, autoConnect = true, keepRunning = true;
    bool quitting = false;
    QString ask;                   // what the DJ is asked to do, when Wave won't act alone
    std::optional<double> launchedAt;
    std::set<uint32_t> owned;

private:
    static bool idle(const Seen &s) { return !s.playing && !s.loaded; }
    bool mayRestart(const Seen &seen, double now);
    Action launch(double now);
    Action restartNow(const Seen &seen, double now);

    bool first_ = true, staleRestarted_ = false;
    uint32_t lastPid_ = 0;
    std::set<uint32_t> triedAttach_, restarted_;
    std::vector<double> relaunches_, restarts_;
};

class Host : public QObject {
    Q_OBJECT
public:
    explicit Host(std::function<bool()> hidden, QObject *parent = nullptr);
    ~Host() override;
    // A couple of times a second, with what Wave sees.
    Action poll(const Seen &seen, double now);
    void connectNow();                    // the DJ pressed CONNECT
    bool quit(bool playing);              // Wave is closing: quit the Rekordbox it used, unless a deck plays
    bool busy() const { return job_.isRunning(); }

    Policy policy;
    QString doing;                        // what Wave is doing for Rekordbox right now, for the status
    QString error;                        // the last action's failure

private:
    void run(Action action, uint32_t pid);
    std::function<bool()> hidden_;
    QFutureWatcher<QString> job_;
};

}  // namespace wave::host
