// Rekordbox's own popups - notices, promotions, warnings, questions - never leave Wave stuck. A watcher
// looks at Rekordbox's windows about once a second. A popup is either closed for the DJ, when it is plainly
// a notice (one acknowledging button, or a "later" kind of button on something that reads like news or an
// offer), or brought over Wave so the DJ can answer it. Nothing Wave can't read is ever pressed.
#pragma once

#include <QMutex>
#include <QString>
#include <QStringList>
#include <QThread>

#include <atomic>
#include <optional>
#include <set>
#include <vector>

namespace wave::host {

struct Popup {
    QString key;                 // the window, as the platform knows it
    quintptr hwnd = 0;           // Windows
    QString title;
    QStringList texts, buttons;  // button names, as shown
    double firstSeen = 0;
    QString summary() const;
};

// The button Wave may press to close this popup for the DJ, or nothing: then it is shown to them.
std::optional<QString> dismissWith(const Popup &popup);

class PopupWatcher {
public:
    PopupWatcher();
    ~PopupWatcher();
    void start();
    void stop();

    // Set from the UI thread.
    std::atomic<uint32_t> pid{0};
    std::atomic<bool> autoClose{true};
    void setExclude(const std::set<quintptr> &windows);

    // Read from the UI thread.
    std::vector<Popup> popups() const;                          // showing now, oldest first
    std::vector<std::pair<double, QString>> closed() const;     // (when, summary) of popups Wave closed
    QString error() const;

private:
    void run();
    mutable QMutex lock_;
    std::vector<Popup> popups_;
    std::vector<std::pair<double, QString>> closed_;
    std::set<quintptr> exclude_;
    QString error_;
    QThread *thread_ = nullptr;
    std::atomic<bool> stop_{false};
};

bool accessibilityTrusted(bool ask);  // macOS: may Wave read other apps' windows

}  // namespace wave::host
