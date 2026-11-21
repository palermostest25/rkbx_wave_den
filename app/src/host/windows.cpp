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
    w.owner = reinterpret_cast<quintptr>(GetWindow(hwnd, GW_OWNER));
    w.owned = w.owner != 0;
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
bool exists(quintptr hwnd) { return hwnd && IsWindow(h(hwnd)); }
void closeWindow(quintptr hwnd) { if (IsWindow(h(hwnd))) PostMessageW(h(hwnd), WM_CLOSE, 0, 0); }
void stash(quintptr hwnd) { if (IsWindow(h(hwnd))) ShowWindow(h(hwnd), SW_HIDE); }
void reveal(quintptr hwnd) { if (IsWindow(h(hwnd)) && !IsWindowVisible(h(hwnd))) ShowWindow(h(hwnd), SW_SHOWNA); }

QRect windowRect(quintptr hwnd) {
    RECT r{};
    GetWindowRect(h(hwnd), &r);
    return QRect(QPoint(r.left, r.top), QPoint(r.right - 1, r.bottom - 1));
}

QRect clientRect(quintptr hwnd) {
    RECT r{};
    POINT origin{0, 0};
    if (!GetClientRect(h(hwnd), &r) || !ClientToScreen(h(hwnd), &origin)) return windowRect(hwnd);
    return QRect(origin.x, origin.y, r.right - r.left, r.bottom - r.top);
}

void present(quintptr hwnd, std::optional<QRect> rect, std::optional<QPoint> centre, bool activate, bool topmost) {
    HWND w = h(hwnd);
    const HWND order = topmost ? HWND_TOPMOST : HWND_TOP;
    if (IsIconic(w) || (rect && IsZoomed(w))) ShowWindow(w, activate ? SW_RESTORE : SW_SHOWNOACTIVATE);  // they ignore a new rectangle
    const UINT flags = SWP_SHOWWINDOW | (activate ? 0 : SWP_NOACTIVATE);
    if (rect) SetWindowPos(w, order, rect->left(), rect->top(), rect->width(), rect->height(), flags);
    else if (centre) {
        RECT r{};
        GetWindowRect(w, &r);
        SetWindowPos(w, order, centre->x() - (r.right - r.left) / 2, centre->y() - (r.bottom - r.top) / 2, 0, 0, flags | SWP_NOSIZE);
    } else {
        SetWindowPos(w, order, 0, 0, 0, 0, flags | SWP_NOMOVE | SWP_NOSIZE);
    }
    if (activate) {
        BringWindowToTop(w);
        SetForegroundWindow(w);
    }
}

void sendToBack(quintptr hwnd) {
    HWND w = h(hwnd);
    if (IsWindow(w)) SetWindowPos(w, HWND_BOTTOM, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
}

void putBack(quintptr hwnd, bool minimize) {
    HWND w = h(hwnd);
    if (!IsWindow(w)) return;
    SetWindowPos(w, HWND_NOTOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
    if (minimize) ShowWindow(w, SW_SHOWMINNOACTIVE);
}

std::optional<Embedded> embed(quintptr hwnd, quintptr wave, const QRect &rect, quintptr owner) {
    HWND w = h(hwnd);
    if (!IsWindow(w)) return std::nullopt;
    Embedded e{hwnd, GetWindowLongPtrW(w, GWL_STYLE), GetWindowLongPtrW(w, GWL_EXSTYLE), GetWindowLongPtrW(w, GWLP_HWNDPARENT)};
    // Already Wave's (embedded twice, or not given back): what it's given back to is Rekordbox, never Wave, or it would
    // stay above Wave, on top or not.
    if (e.owner == LONG_PTR(wave)) {
        e.owner = LONG_PTR(owner);
        e.style |= WS_CAPTION | WS_THICKFRAME | WS_SYSMENU | WS_MINIMIZEBOX | WS_MAXIMIZEBOX;
    }
    // A minimised, maximised or hidden window ignores a new rectangle: shown first, as it was, without taking the focus
    // (Rekordbox's own restore of its split browser can leave it minimised).
    if (IsIconic(w) || !IsWindowVisible(w)) ShowWindow(w, SW_SHOWNOACTIVATE);
    if (IsZoomed(w)) ShowWindow(w, SW_RESTORE);
    SetWindowPos(w, HWND_NOTOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
    SetWindowLongPtrW(w, GWLP_HWNDPARENT, LONG_PTR(wave));  // owned by Wave: always just above it, and with it
    SetWindowLongPtrW(w, GWL_STYLE, e.style & ~LONG_PTR(WS_CAPTION | WS_THICKFRAME | WS_SYSMENU | WS_MINIMIZEBOX | WS_MAXIMIZEBOX));
    SetWindowLongPtrW(w, GWL_EXSTYLE, e.exStyle & ~LONG_PTR(WS_EX_DLGMODALFRAME | WS_EX_CLIENTEDGE | WS_EX_STATICEDGE | WS_EX_WINDOWEDGE |
                                                           WS_EX_APPWINDOW | WS_EX_TOPMOST));
    SetWindowPos(w, HWND_TOP, rect.left(), rect.top(), rect.width(), rect.height(), SWP_FRAMECHANGED | SWP_SHOWWINDOW);
    if (!IsWindowVisible(w)) ShowWindow(w, SW_SHOWNOACTIVATE);  // hidden with a minimised owner, before it was Wave's
    SetForegroundWindow(w);
    return e;
}

bool iconic(quintptr hwnd) { return IsWindow(h(hwnd)) && IsIconic(h(hwnd)); }

void reshow(quintptr hwnd, const QRect &rect) {
    HWND w = h(hwnd);
    if (!IsWindow(w)) return;
    ShowWindow(w, IsIconic(w) ? SW_RESTORE : SW_SHOWNOACTIVATE);
    if (IsZoomed(w)) ShowWindow(w, SW_RESTORE);
    SetWindowPos(w, HWND_TOP, rect.left(), rect.top(), rect.width(), rect.height(), SWP_SHOWWINDOW);
}

void place(quintptr hwnd, const QRect &rect) {
    HWND w = h(hwnd);
    RECT r{};
    if (!IsWindow(w)) return;
    if (IsIconic(w)) ShowWindow(w, SW_SHOWNOACTIVATE);  // minimised behind Wave's back: up again in Wave's page
    if (!GetWindowRect(w, &r)) return;
    if (r.left == rect.left() && r.top == rect.top() && r.right - r.left == rect.width() && r.bottom - r.top == rect.height()) return;
    SetWindowPos(w, nullptr, rect.left(), rect.top(), rect.width(), rect.height(), SWP_NOZORDER | SWP_NOACTIVATE);
}

void unembed(const Embedded &e) {
    HWND w = h(e.hwnd);
    if (!IsWindow(w)) return;
    SetWindowLongPtrW(w, GWL_STYLE, LONG_PTR(e.style));
    SetWindowLongPtrW(w, GWL_EXSTYLE, LONG_PTR(e.exStyle));
    SetWindowLongPtrW(w, GWLP_HWNDPARENT, LONG_PTR(e.owner));
    SetWindowPos(w, nullptr, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE | SWP_FRAMECHANGED);
}

void setTopmost(quintptr hwnd, bool on) {
    // Only a change: setting it again also brings the window to the front of every other on-top window (Wave's startup
    // screen flickered each time Wave checked it was still on top).
    const bool now = GetWindowLongPtrW(h(hwnd), GWL_EXSTYLE) & WS_EX_TOPMOST;
    if (now == on) return;
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

QString helpOf(IUIAutomationElement *e) {
    BSTR help = nullptr;
    if (FAILED(e->get_CurrentHelpText(&help)) || !help) return {};
    const QString s = QString::fromWCharArray(help);
    SysFreeString(help);
    return s;
}

// Calls ``each`` for the window's descendants (up to ``limit``), returning false to stop.
template <typename F> bool walk(IUIAutomation *a, quintptr hwnd, F each, QString *windowName = nullptr, int limit = 200) {
    IUIAutomationElement *window = nullptr;
    if (FAILED(a->ElementFromHandle(reinterpret_cast<UIA_HWND>(hwnd), &window)) || !window) return false;
    if (windowName) *windowName = nameOf(window);
    IUIAutomationCondition *all = nullptr;
    IUIAutomationElementArray *found = nullptr;
    bool ok = SUCCEEDED(a->CreateTrueCondition(&all)) && SUCCEEDED(window->FindAll(TreeScope_Descendants, all, &found)) && found;
    if (ok) {
        int count = 0;
        found->get_Length(&count);
        for (int i = 0; i < std::min(count, limit); ++i) {
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
        // By its name, or by its tooltip (JUCE gives a button's tooltip as its help text: Rekordbox's source icons have
        // no name, only "Display Beatport" and the like).
        if (type != UIA_ButtonControlTypeId || (nameOf(e) != button && helpOf(e).compare(button, Qt::CaseInsensitive) != 0)) return true;
        IUIAutomationInvokePattern *invoke = nullptr;
        if (SUCCEEDED(e->GetCurrentPatternAs(UIA_InvokePatternId, __uuidof(IUIAutomationInvokePattern), reinterpret_cast<void **>(&invoke))) && invoke) {
            pressed = SUCCEEDED(invoke->Invoke());
            invoke->Release();
        }
        return false;
    }, nullptr, 5000);  // Rekordbox's windows have hundreds of elements
    return pressed;
}

bool Automation::select(quintptr hwnd, const QString &name) {
    if (!automation_) return false;
    bool done = false;
    walk(static_cast<IUIAutomation *>(automation_), hwnd, [&](IUIAutomationElement *e, CONTROLTYPEID type) {
        if (type != UIA_TreeItemControlTypeId && type != UIA_ListItemControlTypeId && type != UIA_DataItemControlTypeId &&
            type != UIA_ButtonControlTypeId && type != UIA_TextControlTypeId)
            return true;
        if (!nameOf(e).trimmed().startsWith(name, Qt::CaseInsensitive)) return true;
        IUIAutomationSelectionItemPattern *item = nullptr;
        if (SUCCEEDED(e->GetCurrentPatternAs(UIA_SelectionItemPatternId, __uuidof(IUIAutomationSelectionItemPattern),
                                             reinterpret_cast<void **>(&item))) && item) {
            done = SUCCEEDED(item->Select());
            item->Release();
        }
        if (!done) {
            IUIAutomationInvokePattern *invoke = nullptr;
            if (SUCCEEDED(e->GetCurrentPatternAs(UIA_InvokePatternId, __uuidof(IUIAutomationInvokePattern), reinterpret_cast<void **>(&invoke))) &&
                invoke) {
                done = SUCCEEDED(invoke->Invoke());
                invoke->Release();
            }
        }
        if (!done) {
            IUIAutomationLegacyIAccessiblePattern *legacy = nullptr;
            if (SUCCEEDED(e->GetCurrentPatternAs(UIA_LegacyIAccessiblePatternId, __uuidof(IUIAutomationLegacyIAccessiblePattern),
                                                 reinterpret_cast<void **>(&legacy))) && legacy) {
                done = SUCCEEDED(legacy->DoDefaultAction());
                legacy->Release();
            }
        }
        return !done;
    });
    return done;
}

#else  // not Windows: Rekordbox's windows are reached through macOS Accessibility (popups.cpp) instead

std::vector<Win> windowsOf(uint32_t) { return {}; }
bool alive(quintptr) { return false; }
bool exists(quintptr) { return false; }
void stash(quintptr) {}
void closeWindow(quintptr) {}
void reveal(quintptr) {}
QRect windowRect(quintptr) { return {}; }
QRect clientRect(quintptr) { return {}; }
void present(quintptr, std::optional<QRect>, std::optional<QPoint>, bool, bool) {}
void putBack(quintptr, bool) {}
void sendToBack(quintptr) {}
void setTopmost(quintptr, bool) {}
std::optional<Embedded> embed(quintptr, quintptr, const QRect &, quintptr) { return std::nullopt; }
void place(quintptr, const QRect &) {}
bool iconic(quintptr) { return false; }
void reshow(quintptr, const QRect &) {}
void unembed(const Embedded &) {}
bool splitScreenFromMenu(quintptr) { return false; }
Automation::Automation() {}
Automation::~Automation() {}
bool Automation::read(quintptr, QStringList *, QStringList *) { return false; }
bool Automation::press(quintptr, const QString &) { return false; }
bool Automation::select(quintptr, const QString &) { return false; }

#endif

}  // namespace wave::host
