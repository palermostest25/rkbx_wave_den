#include "host/host.h"

#include "core/clock.h"
#include "host/rekordbox.h"

#include <QLoggingCategory>
#include <QtConcurrent>

#include <algorithm>

Q_LOGGING_CATEGORY(lcHost, "wave.host")

namespace wave::host {

const char *const kAskStart = "Rekordbox is not running";
const char *const kAskConnect = "Rekordbox runs without Wave: CONNECT restarts it with Wave (stop the decks first)";
const char *const kAskGaveUp = "Rekordbox keeps closing: CONNECT starts it again";
const char *const kAskUpdate = "Rekordbox runs an earlier Wave's bridge: CONNECT restarts it to finish updating (stop the decks first)";

Policy::Policy(bool windows) : windows(windows) {}

Action Policy::decide(const Seen &seen, double now) {
    const bool first = first_;
    first_ = false;
    if (!seen.pid) {
        const uint32_t wentAway = lastPid_;
        lastPid_ = 0;
        if (quitting) return Action::Nothing;
        if (wentAway && owned.count(wentAway)) {
            relaunches_.erase(std::remove_if(relaunches_.begin(), relaunches_.end(), [&](double t) { return now - t >= kRelaunchWindow; }),
                              relaunches_.end());
            if (!keepRunning) { ask = kAskStart; return Action::Nothing; }
            if (int(relaunches_.size()) >= kRelaunches) { ask = kAskGaveUp; return Action::Nothing; }
            qCWarning(lcHost) << "Rekordbox" << wentAway << "closed while Wave was using it: starting it again";
            relaunches_.push_back(now);
            return launch(now);
        }
        if (starting(now)) return Action::Nothing;
        if (first && autoStart) return launch(now);
        ask = kAskStart;
        return Action::Nothing;
    }
    lastPid_ = seen.pid;
    if (seen.state == State::Live || seen.state == State::Busy || seen.state == State::Starting) {
        ask.clear();
        owned.insert(seen.pid);
        if (seen.state == State::Live && seen.stale && !staleRestarted_ && idle(seen) && autoConnect && !starting(now) &&
            mayRestart(seen, now)) {
            qCInfo(lcHost) << "Rekordbox runs an earlier Wave bridge and is idle: restarting it to finish updating";
            staleRestarted_ = true;  // once: if that didn't help, the earlier bridge still works
            return restartNow(seen, now);
        }
        if (seen.state == State::Live && seen.stale && !staleRestarted_) ask = kAskUpdate;
        return Action::Nothing;
    }
    if (seen.state == State::Unsupported || seen.state == State::Failed) {
        ask.clear();  // nothing Wave can do; the status says why
        return Action::Nothing;
    }
    // None (no bridge) or Older (an earlier Wave's incompatible bridge).
    if (seen.state == State::None && starting(now)) {
        // Wave just started it: the bridge arrives once Rekordbox reads OpenSSL's config. On Windows, a
        // bridge that hasn't come after half a minute is connected live instead.
        if (!(windows && seen.age && *seen.age > 30)) return Action::Nothing;
    }
    if (windows && seen.state == State::None && !seen.bridgeLoaded && autoConnect && !triedAttach_.count(seen.pid)) {
        triedAttach_.insert(seen.pid);
        return Action::Attach;
    }
    if (autoConnect && mayRestart(seen, now) &&
        ((seen.known && idle(seen)) || (seen.age && *seen.age < kFresh && !seen.loaded)))
        return restartNow(seen, now);
    ask = kAskConnect;
    return Action::Nothing;
}

bool Policy::mayRestart(const Seen &seen, double now) {
    restarts_.erase(std::remove_if(restarts_.begin(), restarts_.end(), [&](double t) { return now - t >= kRelaunchWindow; }),
                    restarts_.end());
    return !restarted_.count(seen.pid) && int(restarts_.size()) < kRelaunches;
}

Action Policy::launch(double now) {
    ask.clear();
    launchedAt = now;
    return Action::Launch;
}

Action Policy::restartNow(const Seen &seen, double now) {
    ask.clear();
    restarted_.insert(seen.pid);
    restarts_.push_back(now);
    launchedAt = now;
    return Action::Restart;
}

#ifdef Q_OS_WIN
constexpr bool kWindows = true;
#else
constexpr bool kWindows = false;
#endif

Host::Host(std::function<bool()> hidden, QObject *parent) : QObject(parent), policy(kWindows), hidden_(std::move(hidden)) {
    connect(&job_, &QFutureWatcher<QString>::finished, this, [this] {
        error = job_.result();
        if (!error.isEmpty()) qCWarning(lcHost) << doing << "failed:" << error;
        doing.clear();
    });
}

Host::~Host() { job_.waitForFinished(); }

Action Host::poll(const Seen &seen, double now) {
    if (busy()) return Action::Nothing;
    const Action action = policy.decide(seen, now);
    if (action != Action::Nothing) run(action, seen.pid);
    return action;
}

void Host::run(Action action, uint32_t pid) {
    doing = action == Action::Launch ? QStringLiteral("Starting Rekordbox")
          : action == Action::Attach ? QStringLiteral("Connecting to Rekordbox")
                                     : QStringLiteral("Restarting Rekordbox with Wave");
    qCInfo(lcHost) << doing << "( pid" << pid << ")";
    const bool hidden = hidden_();
    job_.setFuture(QtConcurrent::run([this, action, pid, hidden]() -> QString {
        QString failure;
        if (action == Action::Launch) failure = launch(hidden);
        else if (action == Action::Restart) failure = restart(hidden);
        else {
            QString message;
            failure = attach(pid, &message);
            qCInfo(lcHost) << "Live connect to Rekordbox" << pid << ":" << (failure.isEmpty() ? message : failure);
        }
        if (failure.isEmpty()) {
            const uint32_t started = rekordboxPid();
            QMetaObject::invokeMethod(this, [this, started] { policy.launched(started); }, Qt::QueuedConnection);
        }
        return failure;
    }));
}

void Host::connectNow() {
    if (busy()) return;
    policy.ask.clear();
    policy.launchedAt = wave::now();
    run(rekordboxPid() ? Action::Restart : Action::Launch, 0);
}

bool Host::quit(bool playing) {
    const uint32_t pid = rekordboxPid();
    if (!pid || playing || !policy.owned.count(pid)) return false;
    policy.quitting = true;
    qCInfo(lcHost) << "Quitting Rekordbox" << pid << "with Wave";
    askToQuit(pid);
    return true;
}

}  // namespace wave::host
