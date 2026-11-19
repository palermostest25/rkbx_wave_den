#include "host/popups.h"

#include "core/clock.h"
#include "host/rekordbox.h"
#include "host/windows.h"

#include <QLoggingCategory>
#include <QRegularExpression>

#include <chrono>
#include <map>
#include <thread>

#ifdef Q_OS_MACOS
#include <ApplicationServices/ApplicationServices.h>
#endif

Q_LOGGING_CATEGORY(lcPopups, "wave.popups")

namespace wave::host {
namespace {

// One button only, and it just acknowledges: pressing it is all the popup can do.
const std::set<QString> kAcknowledge = {"ok", "close", "got it", "done", "dismiss", "understood", "i understand", "continue", "x", "×"};
// Buttons that put something off without changing anything.
const std::set<QString> kLater = {"later", "not now", "remind me later", "maybe later", "skip", "no thanks", "no, thanks",
                                  "not yet", "ask me later", "don't show again", "do not show again", "don't show this again"};
const std::set<QString> kSoftClose = {"close", "cancel", "x", "×"};
// Questions the DJ has answered for good: the button Wave always presses on them, which changes nothing.
// "My Tag in this device is synced with another computer. Do you want to delete My Tag stored in the device...?"
const std::set<QString> kAlwaysAnswer = {"do not sync with my tag", "don't sync with my tag"};
// Rekordbox offering a newer version of itself: Wave stays on the version its bridge supports, so always declined.
const std::set<QString> kDeclineUpdate = {"later", "not now", "remind me later", "skip", "skip this version", "cancel", "close", "no",
                                          "no thanks", "don't update", "do not update", "x", "×"};
const QRegularExpression &updateOffer() {
    static const QRegularExpression re("\\b(new version|newer version|latest version|update available|software update|update rekordbox|"
                                       "rekordbox \\d+\\.\\d+|\\d+\\.\\d+\\.\\d+)\\b", QRegularExpression::CaseInsensitiveOption);
    return re;
}

const QRegularExpression &notice() {
    // Words of a notice or an offer: then closing or cancelling it is also fine.
    static const QRegularExpression re(
        "\\b(what'?s new|new feature|news|update available|new version|version|release notes|tips?|tutorial|subscription|plan|"
        "trial|offer|campaign|discount|sale|upgrade|premium|creative|professional|core|cloud|survey|feedback|notice|"
        "information|announcement|learn more|introducing)\\b", QRegularExpression::CaseInsensitiveOption);
    return re;
}

const QRegularExpression &keep() {
    // Never dismissed for the DJ: Rekordbox may need these answered.
    static const QRegularExpression re(
        "\\b(sign ?in|log ?in|password|account|licen[cs]e|activat\\w*|quit|exit|delete|remove|overwrite|database|backup|"
        "restore|convert|corrupt\\w*|audio device|driver)\\b", QRegularExpression::CaseInsensitiveOption);
    return re;
}

QString label(QString text) {
    text = text.simplified();
    while (text.endsWith('.') || text.endsWith('!')) text.chop(1);
    while (text.startsWith('.') || text.startsWith('!')) text.remove(0, 1);
    return text.trimmed().toLower().replace(QChar(0x2019), '\'');
}

}  // namespace

QString Popup::summary() const {
    QStringList words;
    for (const QString &t : QStringList{title} + texts)
        if (!t.isEmpty() && t.toLower() != QLatin1String("rekordbox") && !words.contains(t)) words << t;
    const QString s = words.join(QStringLiteral(" · ")).left(140);
    return s.isEmpty() ? QStringLiteral("Rekordbox is asking something") : s;
}

std::optional<QString> dismissWith(const Popup &popup) {
    QStringList names;
    for (const QString &b : popup.buttons) names << label(b);
    const QString words = (QStringList{popup.title} + popup.texts).join(' ');
    for (const QString &name : names)
        if (kAlwaysAnswer.count(name)) return popup.buttons[names.indexOf(name)];
    if (updateOffer().match(words).hasMatch())
        for (const QString &name : names)
            if (kDeclineUpdate.count(name)) return popup.buttons[names.indexOf(name)];
    if (names.isEmpty() || keep().match(words).hasMatch()) return std::nullopt;
    if (names.size() == 1 && kAcknowledge.count(names[0])) return popup.buttons[0];
    for (const QString &name : names)
        if (kLater.count(name)) return popup.buttons[names.indexOf(name)];
    if (notice().match(words).hasMatch())
        for (const QString &name : names)
            if (kSoftClose.count(name)) return popup.buttons[names.indexOf(name)];
    return std::nullopt;
}

PopupWatcher::PopupWatcher() = default;
PopupWatcher::~PopupWatcher() { stop(); }

void PopupWatcher::start() {
    if (thread_) return;
    stop_ = false;
    thread_ = QThread::create([this] { run(); });
    thread_->setObjectName("rekordbox-popups");
    thread_->start(QThread::LowPriority);
}

void PopupWatcher::stop() {
    if (!thread_) return;
    stop_ = true;
    thread_->wait(3000);
    delete thread_;
    thread_ = nullptr;
}

void PopupWatcher::setExclude(const std::set<quintptr> &windows) {
    QMutexLocker l(&lock_);
    exclude_ = windows;
}

std::vector<Popup> PopupWatcher::popups() const { QMutexLocker l(&lock_); return popups_; }
std::vector<std::pair<double, QString>> PopupWatcher::closed() const { QMutexLocker l(&lock_); return closed_; }
QString PopupWatcher::error() const { QMutexLocker l(&lock_); return error_; }

#ifdef Q_OS_MACOS
namespace {
CFStringRef cf(const char *s) { return CFStringCreateWithCString(nullptr, s, kCFStringEncodingUTF8); }

QString textOf(AXUIElementRef e, const char *attribute) {
    CFStringRef name = cf(attribute);
    CFTypeRef value = nullptr;
    QString out;
    if (AXUIElementCopyAttributeValue(e, name, &value) == kAXErrorSuccess && value) {
        if (CFGetTypeID(value) == CFStringGetTypeID()) out = QString::fromCFString(static_cast<CFStringRef>(value));
        CFRelease(value);
    }
    CFRelease(name);
    return out;
}

CFArrayRef arrayOf(AXUIElementRef e, const char *attribute) {
    CFStringRef name = cf(attribute);
    CFTypeRef value = nullptr;
    AXUIElementCopyAttributeValue(e, name, &value);
    CFRelease(name);
    if (value && CFGetTypeID(value) != CFArrayGetTypeID()) { CFRelease(value); value = nullptr; }
    return static_cast<CFArrayRef>(value);
}

double areaOf(AXUIElementRef e) {
    CFStringRef name = cf("AXSize");
    CFTypeRef value = nullptr;
    double area = 0;
    if (AXUIElementCopyAttributeValue(e, name, &value) == kAXErrorSuccess && value) {
        CGSize size{};
        if (AXValueGetValue(static_cast<AXValueRef>(value), kAXValueTypeCGSize, &size)) area = size.width * size.height;
        CFRelease(value);
    }
    CFRelease(name);
    return area;
}

bool isMain(AXUIElementRef e) {
    CFStringRef name = cf("AXMain");
    CFTypeRef value = nullptr;
    bool main = false;
    if (AXUIElementCopyAttributeValue(e, name, &value) == kAXErrorSuccess && value) {
        main = value == kCFBooleanTrue;
        CFRelease(value);
    }
    CFRelease(name);
    return main;
}

// Rekordbox's popups: its windows other than its main one and the big ones Wave shows on purpose.
std::vector<Popup> scanMac(uint32_t pid, const std::map<QString, AXUIElementRef> **, std::map<QString, std::map<QString, AXUIElementRef>> &buttons,
                           std::vector<CFTypeRef> &held) {
    std::vector<Popup> found;
    AXUIElementRef app = AXUIElementCreateApplication(pid_t(pid));
    if (!app) return found;
    held.push_back(app);
    AXUIElementSetMessagingTimeout(app, .25f);
    CFArrayRef windows = arrayOf(app, "AXWindows");
    if (!windows) return found;
    held.push_back(windows);
    double largest = 0;
    std::vector<std::pair<double, AXUIElementRef>> sized;
    for (CFIndex i = 0; i < CFArrayGetCount(windows); ++i) {
        auto w = static_cast<AXUIElementRef>(CFArrayGetValueAtIndex(windows, i));
        sized.emplace_back(areaOf(w), w);
        largest = std::max(largest, sized.back().first);
    }
    for (const auto &[area, window] : sized) {
        const QString title = textOf(window, "AXTitle");
        if (isMain(window) || area >= largest * .6 || area <= 0) continue;
        Popup p;
        p.title = title;
        p.key = QStringLiteral("%1|%2").arg(title).arg(qRound64(area));
        std::vector<std::pair<AXUIElementRef, int>> stack{{window, 0}};
        int seen = 0;
        while (!stack.empty() && seen < 300) {
            auto [e, depth] = stack.back();
            stack.pop_back();
            ++seen;
            const QString role = textOf(e, "AXRole");
            if (role == QLatin1String("AXButton")) {
                QString name = textOf(e, "AXTitle");
                if (name.isEmpty()) name = textOf(e, "AXDescription");
                if (name.isEmpty() && textOf(e, "AXSubrole") == QLatin1String("AXCloseButton")) name = QStringLiteral("Close");
                if (!name.isEmpty()) { p.buttons << name; buttons[p.key][name] = e; }
            } else if (role == QLatin1String("AXStaticText")) {
                QString t = textOf(e, "AXValue");
                if (t.isEmpty()) t = textOf(e, "AXTitle");
                if (!t.isEmpty()) p.texts << t;
            }
            if (depth < 10) if (CFArrayRef children = arrayOf(e, "AXChildren")) {
                held.push_back(children);
                for (CFIndex i = CFArrayGetCount(children) - 1; i >= 0; --i)
                    stack.emplace_back(static_cast<AXUIElementRef>(CFArrayGetValueAtIndex(children, i)), depth + 1);
            }
        }
        found.push_back(p);
    }
    return found;
}
}  // namespace

bool accessibilityTrusted(bool ask) {
    const void *keys[] = {kAXTrustedCheckOptionPrompt};
    const void *values[] = {ask ? kCFBooleanTrue : kCFBooleanFalse};
    CFDictionaryRef options = CFDictionaryCreate(nullptr, keys, values, 1, &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks);
    const bool trusted = AXIsProcessTrustedWithOptions(options);
    CFRelease(options);
    return trusted;
}
#else
bool accessibilityTrusted(bool) { return true; }
#endif

void PopupWatcher::run() {
    std::map<QString, double> firstSeen;
#ifdef Q_OS_WIN
    Automation automation;
#endif
#ifdef Q_OS_MACOS
    bool asked = false;
#endif
    while (!stop_) {
        std::vector<Popup> found, current;
        QString error;
        std::set<quintptr> exclude;
        { QMutexLocker l(&lock_); exclude = exclude_; }
        const uint32_t target = pid;
        if (autoUpdateOff && stopUpdater()) qCInfo(lcPopups) << "Closed Rekordbox's update manager: Wave keeps Rekordbox at" << supportedText();
#ifdef Q_OS_MACOS
        std::map<QString, std::map<QString, AXUIElementRef>> buttons;
        std::vector<CFTypeRef> held;
#endif
        if (target) {
#ifdef Q_OS_WIN
            for (const Win &w : classify(windowsOf(target), exclude).popups) {
                Popup p;
                p.hwnd = w.hwnd;
                p.key = QString::number(w.hwnd);
                p.title = w.title;
                QStringList texts;
                automation.read(w.hwnd, &texts, &p.buttons);
                for (const QString &t : texts) if (t != w.title) p.texts << t;
                found.push_back(p);
            }
#elif defined(Q_OS_MACOS)
            if (!accessibilityTrusted(false)) {
                if (!asked) { asked = true; accessibilityTrusted(true); }  // macOS shows its own prompt, once
                error = QStringLiteral("Allow Rekordbox Wave in Accessibility to handle Rekordbox popups");
            } else {
                found = scanMac(target, nullptr, buttons, held);
            }
#endif
        }
        const double t = now();
        for (Popup &p : found) {
            p.firstSeen = firstSeen.count(p.key) ? firstSeen[p.key] : t;
            const auto choice = autoClose ? dismissWith(p) : std::nullopt;
            // A moment's grace: a popup still drawing has not shown all its buttons yet.
            if (choice && t - p.firstSeen > .6) {
                bool pressed = false;
#ifdef Q_OS_WIN
                pressed = automation.press(p.hwnd, *choice);
#elif defined(Q_OS_MACOS)
                CFStringRef press = cf("AXPress");
                pressed = AXUIElementPerformAction(buttons[p.key][*choice], press) == kAXErrorSuccess;
                CFRelease(press);
#endif
                if (pressed) {
                    qCInfo(lcPopups) << "Closed a Rekordbox popup with" << *choice << ":" << p.summary();
                    QMutexLocker l(&lock_);
                    closed_.emplace_back(t, p.summary());
                    if (closed_.size() > 20) closed_.erase(closed_.begin());
                    continue;
                }
                qCWarning(lcPopups) << "Could not close Rekordbox popup" << p.summary();
            }
            current.push_back(p);
        }
#ifdef Q_OS_MACOS
        for (CFTypeRef r : held) CFRelease(r);
#endif
        firstSeen.clear();
        for (const Popup &p : current) firstSeen[p.key] = p.firstSeen;
        std::sort(current.begin(), current.end(), [](const Popup &a, const Popup &b) { return a.firstSeen < b.firstSeen; });
        {
            QMutexLocker l(&lock_);
            popups_ = current;
            error_ = error;
        }
        for (int i = 0; i < 50 && !stop_; ++i) std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
}

}  // namespace wave::host
