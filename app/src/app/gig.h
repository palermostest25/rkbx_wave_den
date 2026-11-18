// What keeps Wave dependable through a set: a log of what happened, one Wave at a time, a screen that
// never sleeps, the battery in view, and Windows' touch keyboard kept out of the way while Wave has its own.
#pragma once

#include <QObject>
#include <QString>

#include <functional>
#include <optional>

class QLocalServer;

namespace wave {

class Settings;

QString logFolder();
bool setupLogging(const QString &folder);   // wave.log, rotated, and Qt's own messages; false if unwritable

// One Wave at a time: a second start brings the running Wave forward and quits.
class SingleInstance : public QObject {
    Q_OBJECT
public:
    SingleInstance();
    bool primary() const { return primary_; }
    void listen(std::function<void()> onShow);

private:
    QString name_;
    bool primary_ = true;
    QLocalServer *server_ = nullptr;
};

void keepAwake(bool on);                    // no screen saver, display sleep or system sleep

struct Battery { int percent = 0; bool plugged = false; };
std::optional<Battery> battery();           // none on a desktop

// Windows' touch keyboard (Settings › Time & language › Typing › Touch keyboard):
// HKCU\Software\Microsoft\TabletTip\1.7 TouchKeyboardTapInvoke, 0 never, 1 when no keyboard is attached.
namespace touch {
constexpr int kNever = 0, kNoKeyboard = 1, kAlways = 2;
bool available();
int mode();                                 // Windows' setting now
bool setMode(int value);
void showWindowsKeyboard();                 // Windows' touch keyboard up, for the field that has focus

// Off while Wave runs with its own keyboard, back as it was when Wave closes. The setting to put back is
// kept in Wave's settings, so a Wave that crashed restores it the next time.
class Guard {
public:
    Guard(Settings &settings, std::function<void()> save) : settings_(settings), save_(std::move(save)) {}
    void apply();                           // per the settings: Wave's keyboard (off) or Windows' (as chosen)
    void restore();                         // Windows' setting as it was
    bool choose(int value);                 // the DJ picked a setting by hand

private:
    Settings &settings_;
    std::function<void()> save_;
};
}  // namespace touch

}  // namespace wave
