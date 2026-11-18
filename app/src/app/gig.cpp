#include "app/gig.h"

#include "core/settings.h"

#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QLocalServer>
#include <QLocalSocket>
#include <QMutex>
#include <QProcess>
#include <QThread>

#include <cstdio>

#ifdef Q_OS_WIN
#include <windows.h>
#include <initguid.h>
#include <objbase.h>
#include <shobjidl.h>
#endif
#ifdef Q_OS_MACOS
#include <IOKit/ps/IOPSKeys.h>
#include <IOKit/ps/IOPowerSources.h>
#include <IOKit/pwr_mgt/IOPMLib.h>
#endif

namespace wave {
namespace {

QFile *logFile = nullptr;
QMutex logLock;
QString logPath;

void rotate() {
    // Kept small: five files of a megabyte.
    if (!logFile || logFile->size() < 1000000) return;
    logFile->close();
    for (int i = 4; i >= 1; --i) {
        const QString from = i == 1 ? logPath : logPath + "." + QString::number(i - 1);
        QFile::remove(logPath + "." + QString::number(i));
        QFile::rename(from, logPath + "." + QString::number(i));
    }
    logFile->open(QIODevice::WriteOnly | QIODevice::Append | QIODevice::Text);
}

void handler(QtMsgType type, const QMessageLogContext &context, const QString &message) {
    static const char *levels[] = {"DEBUG", "WARNING", "ERROR", "CRITICAL", "INFO"};
    if (type == QtDebugMsg) return;
    const QByteArray line = QStringLiteral("%1 %2 %3: %4\n")
                                .arg(QDateTime::currentDateTime().toString("yyyy-MM-dd HH:mm:ss.zzz"),
                                     QString::fromLatin1(levels[std::min<int>(type, 4)]),
                                     QString::fromLatin1(context.category ? context.category : "wave"), message)
                                .toUtf8();
    {
        QMutexLocker l(&logLock);
        if (logFile) {
            rotate();
            logFile->write(line);
            logFile->flush();
        }
    }
    fputs(line.constData(), stderr);
    if (type == QtFatalMsg) abort();
}

#ifdef Q_OS_WIN
LONG WINAPI crashed(EXCEPTION_POINTERS *info) {
    // The last words of a crash, where the DJ's bug report will find them.
    QFile crash(QDir(QFileInfo(logPath).absolutePath()).filePath("crash.log"));
    if (crash.open(QIODevice::WriteOnly | QIODevice::Append)) {
        crash.write(QStringLiteral("%1 crashed: exception %2 at %3\n")
                        .arg(QDateTime::currentDateTime().toString(Qt::ISODate))
                        .arg(info->ExceptionRecord->ExceptionCode, 8, 16, QChar('0'))
                        .arg(quintptr(info->ExceptionRecord->ExceptionAddress), 16, 16, QChar('0'))
                        .toUtf8());
    }
    return EXCEPTION_CONTINUE_SEARCH;
}
#endif

}  // namespace

QString logFolder() { return QDir(settingsFolder()).filePath("logs"); }

bool setupLogging(const QString &folder) {
    if (!QDir().mkpath(folder)) return false;
    logPath = QDir(folder).filePath("wave.log");
    logFile = new QFile(logPath);
    if (!logFile->open(QIODevice::WriteOnly | QIODevice::Append | QIODevice::Text)) {
        delete logFile;
        logFile = nullptr;
        return false;
    }
    qInstallMessageHandler(handler);
#ifdef Q_OS_WIN
    SetUnhandledExceptionFilter(crashed);
#endif
    return true;
}

SingleInstance::SingleInstance() {
    QString user = qEnvironmentVariable("USERNAME");
    if (user.isEmpty()) user = qEnvironmentVariable("USER", "user");
    name_ = QStringLiteral("rekordbox-wave-%1").arg(user);
    QLocalSocket socket;
    socket.connectToServer(name_);
    if (socket.waitForConnected(400)) {
        socket.write("show\n");
        socket.flush();
        socket.waitForBytesWritten(400);
        socket.disconnectFromServer();
        primary_ = false;
    }
}

void SingleInstance::listen(std::function<void()> onShow) {
    QLocalServer::removeServer(name_);  // left by a Wave that crashed
    server_ = new QLocalServer(this);
    server_->setSocketOptions(QLocalServer::UserAccessOption);
    if (!server_->listen(name_)) {
        qWarning("Single-instance server: %s", qPrintable(server_->errorString()));
        return;
    }
    connect(server_, &QLocalServer::newConnection, this, [this, onShow] {
        // Any knock is "come forward": what it says may arrive before it could be read.
        while (server_->hasPendingConnections()) {
            QLocalSocket *client = server_->nextPendingConnection();
            connect(client, &QLocalSocket::disconnected, client, &QObject::deleteLater);
            onShow();
        }
    });
}

void keepAwake(bool on) {
#ifdef Q_OS_WIN
    SetThreadExecutionState(ES_CONTINUOUS | (on ? ES_SYSTEM_REQUIRED | ES_DISPLAY_REQUIRED : 0));
#elif defined(Q_OS_MACOS)
    static IOPMAssertionID assertion = 0;
    if (on && !assertion)
        IOPMAssertionCreateWithName(kIOPMAssertionTypePreventUserIdleDisplaySleep, kIOPMAssertionLevelOn,
                                    CFSTR("Rekordbox Wave is on stage"), &assertion);
    else if (!on && assertion) {
        IOPMAssertionRelease(assertion);
        assertion = 0;
    }
#else
    Q_UNUSED(on);
#endif
}

std::optional<Battery> battery() {
#ifdef Q_OS_WIN
    SYSTEM_POWER_STATUS s{};
    if (!GetSystemPowerStatus(&s) || s.BatteryFlag == 128 || s.BatteryFlag == 255 || s.BatteryLifePercent > 100) return std::nullopt;
    return Battery{s.BatteryLifePercent, s.ACLineStatus == 1};
#elif defined(Q_OS_MACOS)
    CFTypeRef info = IOPSCopyPowerSourcesInfo();
    if (!info) return std::nullopt;
    std::optional<Battery> out;
    CFArrayRef list = IOPSCopyPowerSourcesList(info);
    for (CFIndex i = 0; list && i < CFArrayGetCount(list) && !out; ++i) {
        CFDictionaryRef source = IOPSGetPowerSourceDescription(info, CFArrayGetValueAtIndex(list, i));
        if (!source) continue;
        int current = 0, max = 0;
        auto number = [&](CFStringRef key, int *value) {
            auto n = static_cast<CFNumberRef>(CFDictionaryGetValue(source, key));
            return n && CFNumberGetValue(n, kCFNumberIntType, value);
        };
        if (!number(CFSTR(kIOPSCurrentCapacityKey), &current) || !number(CFSTR(kIOPSMaxCapacityKey), &max) || max <= 0) continue;
        auto state = static_cast<CFStringRef>(CFDictionaryGetValue(source, CFSTR(kIOPSPowerSourceStateKey)));
        out = Battery{current * 100 / max, state && CFStringCompare(state, CFSTR(kIOPSACPowerValue), 0) == kCFCompareEqualTo};
    }
    if (list) CFRelease(list);
    CFRelease(info);
    return out;
#else
    return std::nullopt;
#endif
}

namespace touch {

#ifdef Q_OS_WIN
namespace {
const wchar_t *kKey = L"Software\\Microsoft\\TabletTip\\1.7";

std::optional<DWORD> get(const wchar_t *name) {
    DWORD value = 0, size = sizeof value, type = 0;
    if (RegGetValueW(HKEY_CURRENT_USER, kKey, name, RRF_RT_REG_DWORD, &type, &value, &size) != ERROR_SUCCESS) return std::nullopt;
    return value;
}

bool set(const wchar_t *name, DWORD value) {
    HKEY key = nullptr;
    if (RegCreateKeyExW(HKEY_CURRENT_USER, kKey, 0, nullptr, 0, KEY_SET_VALUE, nullptr, &key, nullptr) != ERROR_SUCCESS) return false;
    const bool ok = RegSetValueExW(key, name, 0, REG_DWORD, reinterpret_cast<const BYTE *>(&value), sizeof value) == ERROR_SUCCESS;
    RegCloseKey(key);
    return ok;
}

// The touch keyboard's own COM interface, as Windows' tray button uses it.
DEFINE_GUID(CLSID_UIHostNoLaunch, 0x4CE576FA, 0x83DC, 0x4F88, 0x95, 0x1C, 0x9D, 0x07, 0x82, 0xB4, 0xE3, 0x76);
DEFINE_GUID(IID_ITipInvocation, 0x37c994e7, 0x432b, 0x4834, 0xa2, 0xf7, 0xdc, 0xe1, 0xf1, 0x3b, 0x83, 0x4b);
struct ITipInvocation : IUnknown { virtual HRESULT STDMETHODCALLTYPE Toggle(HWND wnd) = 0; };
}  // namespace

bool available() { return true; }

int mode() {
    if (const auto v = get(L"TouchKeyboardTapInvoke"); v && *v <= 2) return int(*v);
    const auto legacy = get(L"EnableDesktopModeAutoInvoke");
    return legacy && *legacy == 0 ? kNever : kNoKeyboard;
}

bool setMode(int value) {
    const bool ok = set(L"TouchKeyboardTapInvoke", DWORD(value)) && set(L"EnableDesktopModeAutoInvoke", value == kNever ? 0 : 1);
    // As Settings does: tell running programs (the touch keyboard's host among them) that it changed.
    DWORD_PTR result = 0;
    SendMessageTimeoutW(HWND_BROADCAST, WM_SETTINGCHANGE, 0, reinterpret_cast<LPARAM>(L"TabletTip"), SMTO_ABORTIFHUNG, 1000, &result);
    qInfo("Windows touch keyboard: %s", value == kNever ? "never" : value == kNoKeyboard ? "when no keyboard is attached" : "always");
    return ok;
}

bool windowsKeyboardShown() {
    // The input pane's own answer: where the touch keyboard is on screen, empty while it is hidden.
    IFrameworkInputPane *pane = nullptr;
    bool shown = false;
    if (SUCCEEDED(CoCreateInstance(CLSID_FrameworkInputPane, nullptr, CLSCTX_INPROC_SERVER, IID_IFrameworkInputPane,
                                   reinterpret_cast<void **>(&pane))) && pane) {
        RECT r{};
        shown = SUCCEEDED(pane->Location(&r)) && r.right > r.left && r.bottom > r.top;
        pane->Release();
    }
    return shown;
}

void showWindowsKeyboard() {
    // Windows may have brought it up for the tap already: Toggle would put it away again.
    if (windowsKeyboardShown()) return;
    // TabTip.exe hosts it; started if it isn't running, then asked to show itself.
    if (!FindWindowW(L"IPTip_Main_Window", nullptr)) {
        wchar_t path[MAX_PATH];
        if (ExpandEnvironmentStringsW(L"%CommonProgramFiles%\\microsoft shared\\ink\\TabTip.exe", path, MAX_PATH))
            ShellExecuteW(nullptr, L"open", path, nullptr, nullptr, SW_SHOWNORMAL);
        Sleep(150);
    }
    ITipInvocation *tip = nullptr;
    if (SUCCEEDED(CoCreateInstance(CLSID_UIHostNoLaunch, nullptr, CLSCTX_INPROC_HANDLER | CLSCTX_LOCAL_SERVER, IID_ITipInvocation,
                                   reinterpret_cast<void **>(&tip))) && tip) {
        tip->Toggle(GetDesktopWindow());
        tip->Release();
    }
}
#else
bool windowsKeyboardShown() { return false; }
bool available() { return false; }
int mode() { return kNoKeyboard; }
bool setMode(int) { return false; }
void showWindowsKeyboard() {}
#endif

void Guard::apply() {
    if (!available()) return;
    const bool waveKeyboard = settings_.text("keyboard") != QLatin1String("windows");
    if (waveKeyboard && settings_.flag("touch_keyboard_off")) {
        if (settings_.view("touch_keyboard_restore").isNull()) {
            settings_.setView("touch_keyboard_restore", mode());
            save_();
        }
        if (mode() != kNever) setMode(kNever);
        return;
    }
    restore();
    // Windows' keyboard chosen: it must be allowed to come up when a field is tapped.
    if (!waveKeyboard && mode() == kNever) setMode(kNoKeyboard);
}

void Guard::restore() {
    const QVariant original = settings_.view("touch_keyboard_restore");
    if (original.isNull()) return;
    setMode(original.toInt());
    settings_.setView("touch_keyboard_restore", QVariant());
    save_();
}

bool Guard::choose(int value) {
    if (!setMode(value)) return false;
    if (!settings_.view("touch_keyboard_restore").isNull()) {
        settings_.setView("touch_keyboard_restore", value);  // what Wave leaves behind on closing
        save_();
    }
    return true;
}

}  // namespace touch
}  // namespace wave
