#include "host/windows.h"

#include <algorithm>

#ifdef Q_OS_WIN
#include <windows.h>
#include <uiautomation.h>
#endif

namespace wave::host {

Classified classify(const std::vector<Win> &windows, const std::set<quintptr> &presenting) {
    Classified out;
    std::vector<Win> shown;
    for (const Win &w : windows) if ((w.visible || w.minimized) && !w.tool) shown.push_back(w);
    std::vector<Win> named;
    for (const Win &w : shown) if (w.title.trimmed().toLower() == QLatin1String("rekordbox") && !w.owned) named.push_back(w);
    if (named.empty()) for (const Win &w : shown) if (!w.owned && w.resizable) named.push_back(w);
    if (!named.empty()) out.main = *std::max_element(named.begin(), named.end(), [](const Win &a, const Win &b) { return a.area() < b.area(); });
    for (const Win &w : shown) {
        if (out.main && w.hwnd == out.main->hwnd) continue;
        if (presenting.count(w.hwnd) || (w.resizable && out.main && w.area() > out.main->area() / 4)) out.panels.push_back(w);
        else if (w.visible && !w.minimized && w.area() > 0) out.popups.push_back(w);
    }
    return out;
}

#ifdef Q_OS_WIN

namespace {
HWND h(quintptr hwnd) { return reinterpret_cast<HWND>(hwnd); }

std::optional<Win> describe(HWND hwnd) {
    if (!IsWindow(hwnd)) return std::nullopt;
    Win w;
    w.hwnd = reinterpret_cast<quintptr>(hwnd);
    wchar_t title[256] = {};
    GetWindowTextW(hwnd, title, 256);
    w.title = QString::fromWCharArray(title);
    const LONG style = GetWindowLongW(hwnd, GWL_STYLE), ex = GetWindowLongW(hwnd, GWL_EXSTYLE);
    w.minimized = IsIconic(hwnd);
    RECT r{};
    if (w.minimized) {
        WINDOWPLACEMENT placement{};
        placement.length = sizeof placement;
        GetWindowPlacement(hwnd, &placement);
        r = placement.rcNormalPosition;
    } else {
        GetWindowRect(hwnd, &r);
    }
    w.rect = QRect(QPoint(r.left, r.top), QPoint(r.right - 1, r.bottom - 1));
    w.visible = IsWindowVisible(hwnd);
    w.owned = GetWindow(hwnd, GW_OWNER) != nullptr;
    w.resizable = style & (WS_THICKFRAME | WS_MAXIMIZEBOX);
    w.tool = ex & WS_EX_TOOLWINDOW;
    return w;
}
}  // namespace

std::vector<Win> windowsOf(uint32_t pid) {
    struct Found { uint32_t pid; std::vector<Win> windows; } found{pid, {}};
    EnumWindows([](HWND hwnd, LPARAM data) -> BOOL {
        auto *f = reinterpret_cast<Found *>(data);
        DWORD owner = 0;
        GetWindowThreadProcessId(hwnd, &owner);
        if (owner == f->pid) if (auto w = describe(hwnd)) f->windows.push_back(*w);
        return TRUE;
    }, reinterpret_cast<LPARAM>(&found));
    return found.windows;
}

bool alive(quintptr hwnd) { return hwnd && IsWindow(h(hwnd)) && IsWindowVisible(h(hwnd)); }

QRect windowRect(quintptr hwnd) {
    RECT r{};
    GetWindowRect(h(hwnd), &r);
    return QRect(QPoint(r.left, r.top), QPoint(r.right - 1, r.bottom - 1));
}

void present(quintptr hwnd, std::optional<QRect> rect, std::optional<QPoint> centre, bool activate) {
    HWND w = h(hwnd);
    if (IsIconic(w) || (rect && IsZoomed(w))) ShowWindow(w, activate ? SW_RESTORE : SW_SHOWNOACTIVATE);  // they ignore a new rectangle
    const UINT flags = SWP_SHOWWINDOW | (activate ? 0 : SWP_NOACTIVATE);
    if (rect) SetWindowPos(w, HWND_TOPMOST, rect->left(), rect->top(), rect->width(), rect->height(), flags);
    else if (centre) {
        RECT r{};
        GetWindowRect(w, &r);
        SetWindowPos(w, HWND_TOPMOST, centre->x() - (r.right - r.left) / 2, centre->y() - (r.bottom - r.top) / 2, 0, 0, flags | SWP_NOSIZE);
    } else {
        SetWindowPos(w, HWND_TOPMOST, 0, 0, 0, 0, flags | SWP_NOMOVE | SWP_NOSIZE);
    }
    if (activate) {
        BringWindowToTop(w);
        SetForegroundWindow(w);
    }
}

void putBack(quintptr hwnd, bool minimize) {
    HWND w = h(hwnd);
    if (!IsWindow(w)) return;
    SetWindowPos(w, HWND_NOTOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
    if (minimize) ShowWindow(w, SW_SHOWMINNOACTIVE);
}

void setTopmost(quintptr hwnd, bool on) {
    SetWindowPos(h(hwnd), on ? HWND_TOPMOST : HWND_NOTOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
}

bool splitScreenFromMenu(quintptr hwnd) {
    HMENU menu = GetMenu(h(hwnd));
    if (!menu) return false;
    auto label = [](HMENU m, int i) {
        wchar_t text[128] = {};
        GetMenuStringW(m, UINT(i), text, 128, MF_BYPOSITION);
        return QString::fromWCharArray(text).remove('&').trimmed().toLower();
    };
    for (int i = 0; i < GetMenuItemCount(menu); ++i) {
        if (label(menu, i) != QLatin1String("view")) continue;
        HMENU sub = GetSubMenu(menu, i);
        for (int j = 0; sub && j < GetMenuItemCount(sub); ++j) {
            const QString name = label(sub, j).section('\t', 0, 0);
            if (name == QLatin1String("show split screen") || name == QLatin1String("split screen")) {
                PostMessageW(h(hwnd), WM_COMMAND, GetMenuItemID(sub, j) & 0xffff, 0);
                return true;
            }
        }
    }
    return false;
}

Automation::Automation() {
    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    IUIAutomation *a = nullptr;
    if (SUCCEEDED(CoCreateInstance(__uuidof(CUIAutomation), nullptr, CLSCTX_INPROC_SERVER, __uuidof(IUIAutomation),
                                   reinterpret_cast<void **>(&a))))
        automation_ = a;
}

Automation::~Automation() {
    if (automation_) static_cast<IUIAutomation *>(automation_)->Release();
    CoUninitialize();
}

namespace {
QString nameOf(IUIAutomationElement *e) {
    BSTR name = nullptr;
    if (FAILED(e->get_CurrentName(&name)) || !name) return {};
    const QString s = QString::fromWCharArray(name);
    SysFreeString(name);
    return s;
}

// Calls ``each`` for the window's descendants (up to 200), returning false to stop.
template <typename F> bool walk(IUIAutomation *a, quintptr hwnd, F each, QString *windowName = nullptr) {
    IUIAutomationElement *window = nullptr;
    if (FAILED(a->ElementFromHandle(reinterpret_cast<UIA_HWND>(hwnd), &window)) || !window) return false;
    if (windowName) *windowName = nameOf(window);
    IUIAutomationCondition *all = nullptr;
    IUIAutomationElementArray *found = nullptr;
    bool ok = SUCCEEDED(a->CreateTrueCondition(&all)) && SUCCEEDED(window->FindAll(TreeScope_Descendants, all, &found)) && found;
    if (ok) {
        int count = 0;
        found->get_Length(&count);
        for (int i = 0; i < std::min(count, 200); ++i) {
            IUIAutomationElement *e = nullptr;
            if (FAILED(found->GetElement(i, &e)) || !e) continue;
            CONTROLTYPEID type = 0;
            e->get_CurrentControlType(&type);
            const bool more = each(e, type);
            e->Release();
            if (!more) break;
        }
        found->Release();
    }
    if (all) all->Release();
    window->Release();
    return ok;
}
}  // namespace

bool Automation::read(quintptr hwnd, QStringList *texts, QStringList *buttons) {
    if (!automation_) return false;
    QString title;
    const bool ok = walk(static_cast<IUIAutomation *>(automation_), hwnd, [&](IUIAutomationElement *e, CONTROLTYPEID type) {
        if (type == UIA_ButtonControlTypeId) { const QString n = nameOf(e); if (!n.isEmpty()) *buttons << n; }
        else if (type == UIA_TextControlTypeId) { const QString n = nameOf(e); if (!n.isEmpty()) *texts << n; }
        return true;
    }, &title);
    if (!title.isEmpty()) texts->prepend(title);
    return ok;
}

bool Automation::press(quintptr hwnd, const QString &button) {
    if (!automation_) return false;
    bool pressed = false;
    walk(static_cast<IUIAutomation *>(automation_), hwnd, [&](IUIAutomationElement *e, CONTROLTYPEID type) {
        if (type != UIA_ButtonControlTypeId || nameOf(e) != button) return true;
        IUIAutomationInvokePattern *invoke = nullptr;
        if (SUCCEEDED(e->GetCurrentPatternAs(UIA_InvokePatternId, __uuidof(IUIAutomationInvokePattern), reinterpret_cast<void **>(&invoke))) && invoke) {
            pressed = SUCCEEDED(invoke->Invoke());
            invoke->Release();
        }
        return false;
    });
    return pressed;
}

#else  // not Windows: Rekordbox's windows are reached through macOS Accessibility (popups.cpp) instead

std::vector<Win> windowsOf(uint32_t) { return {}; }
bool alive(quintptr) { return false; }
QRect windowRect(quintptr) { return {}; }
void present(quintptr, std::optional<QRect>, std::optional<QPoint>, bool) {}
void putBack(quintptr, bool) {}
void setTopmost(quintptr, bool) {}
bool splitScreenFromMenu(quintptr) { return false; }
Automation::Automation() {}
Automation::~Automation() {}
bool Automation::read(quintptr, QStringList *, QStringList *) { return false; }
bool Automation::press(quintptr, const QString &) { return false; }

#endif

}  // namespace wave::host
