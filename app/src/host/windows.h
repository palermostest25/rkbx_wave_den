// Rekordbox's top-level windows (Windows), placed and raised from Wave, and read through UI Automation, the
// screen reader interface Rekordbox's JUCE interface provides. Nothing of Rekordbox's own code is called.
#pragma once

#include <QRect>
#include <QString>
#include <QStringList>

#include <cstdint>
#include <optional>
#include <set>
#include <vector>

namespace wave::host {

struct Win {
    quintptr hwnd = 0;
    QString title;
    bool visible = false, minimized = false;
    QRect rect;            // the restored rectangle when minimised
    bool owned = false, resizable = false, tool = false;
    qint64 area() const { return qint64(std::max(0, rect.width())) * std::max(0, rect.height()); }
};

struct Classified {
    std::optional<Win> main;   // titled "rekordbox" (the largest, if several)
    std::vector<Win> panels;   // its other resizable windows, such as the Split Screen browser
    std::vector<Win> popups;   // what is left and showing: questions, warnings, notices
};
Classified classify(const std::vector<Win> &windows, const std::set<quintptr> &presenting = {});

std::vector<Win> windowsOf(uint32_t pid);
bool alive(quintptr hwnd);
QRect windowRect(quintptr hwnd);
QRect clientRect(quintptr hwnd);  // its inside (no title bar or frame), in screen pixels
// Put a Rekordbox window above Wave's always-on-top window: into ``rect`` (physical pixels), or centred at
// ``centre``, or where it is.
void present(quintptr hwnd, std::optional<QRect> rect = {}, std::optional<QPoint> centre = {}, bool activate = true,
             bool topmost = true);
void sendToBack(quintptr hwnd);  // behind every other window, without activating it
void putBack(quintptr hwnd, bool minimize = true);         // undo present
// A Rekordbox window shown as part of Wave: owned by Wave (it moves and stacks with it), without its own frame or
// title bar, filling ``rect`` (physical pixels). unembed gives it back its frame and owner as they were.
struct Embedded { quintptr hwnd = 0; qint64 style = 0, exStyle = 0, owner = 0; };
std::optional<Embedded> embed(quintptr hwnd, quintptr wave, const QRect &rect);
void place(quintptr hwnd, const QRect &rect);              // an embedded window, kept where Wave's page is
void unembed(const Embedded &embedded);
bool splitScreenFromMenu(quintptr hwnd);                   // View › Show Split Screen, when there is a native menu
void setTopmost(quintptr hwnd, bool on);

// UI Automation: a window's texts and buttons, and pressing a button, as a screen reader would.
class Automation {
public:
    Automation();
    ~Automation();
    bool ok() const { return automation_ != nullptr; }
    // Texts (the window's name first) and button names; ``press`` presses the named button of that window.
    bool read(quintptr hwnd, QStringList *texts, QStringList *buttons);
    bool press(quintptr hwnd, const QString &button);
    // Selects (or invokes) the first list or tree item of the window whose name starts with ``name``.
    bool select(quintptr hwnd, const QString &name);

private:
    void *automation_ = nullptr;
};

}  // namespace wave::host
