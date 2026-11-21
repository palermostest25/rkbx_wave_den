// Wave's one window: the toolbar, a one-line banner for what the DJ should know now, and the deck screen,
// the touch browser or Settings below. It keeps Rekordbox behind it, steps aside when Rekordbox needs an
// answer, follows the controller, and only draws while something on screen moves.
#pragma once

#include "app/gig.h"
#include "app/updater.h"
#include "host/midi.h"
#include "host/popups.h"
#include "host/setup.h"
#include "host/windows.h"

#include <QMainWindow>
#include <QPointer>
#include <QTimer>

#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <set>

class QLabel;
class QPushButton;
class QStackedWidget;
class QFrame;
class QScreen;

namespace wave {

class Session;
class Canvas;
class BrowserPage;
class SettingsPage;
class Segmented;
struct ReaderFrame;

class MainWindow : public QMainWindow {
    Q_OBJECT
public:
    explicit MainWindow(Session *session, QWidget *parent = nullptr);
    ~MainWindow() override;

    Session *session() const { return session_; }
    Canvas *canvas() const { return canvas_; }
    BrowserPage *browser() const { return browser_; }
    touch::Guard &touchGuard() { return touch_; }
    host::Setup &setup() { return setup_; }
    midi::Listener *midi() const { return midi_; }
    void useController(midi::Listener *listener);  // the controller's messages (tests give one of their own)
    bool controllerSeen() const { return controllerSeen_; }

    // What Settings changes.
    void setView(const QString &key, const QVariant &value);
    void setCount(int count);
    void setDeckOrder(const QString &order);
    void setColoursChanged();
    void setScale(double scale);
    void setOnTop(bool on);
    void selectScreen(const QString &key);
    void changeFullscreen(bool on);
    void zoomBy(double factor);
    void toggleSettings();
    void showPage(const QString &name);   // "settings", "browser" or a settings page by name
    void showRekordbox();
    void showRekordboxPreferences();  // Rekordbox's own Preferences, inside Wave; again: closed
    void installRekordbox(const QString &archive = {});
    void checkForUpdate(bool asked);
    void installWave();
    void openLog();
    void persist();
    void firstRun(bool install, bool setup);
    void showBanner(const QString &text, const QString &tone = "info", const QString &action = {},
                    std::function<void()> callback = {}, int timeoutMs = 0, const QString &key = {});
    void hideBanner(const QString &key = {});
    QString controllerSummary() const;
    QString pillText() const;

signals:
    void installProgress(double fraction, const QString &text);  // for Settings: -1 while it can't tell, 2 when done

protected:
    void closeEvent(QCloseEvent *) override;
    void changeEvent(QEvent *) override;
    bool nativeEvent(const QByteArray &type, void *message, qintptr *result) override;
    bool eventFilter(QObject *, QEvent *) override;

private:
    QPushButton *toolbarButton(const QString &title, std::function<void()> action);
    void frame();
    void paceFrames();
    void watch();                         // four times a second: Rekordbox, popups, decks
    void readouts();                      // once a second: clock, master tempo, battery
    void updateStatus();
    void syncNav();
    void browse(bool byKnob = false);
    void toggleBrowser(bool byKnob = false);
    void escape();
    void onController(midi::Kind kind, int value, int deck);
    void followRekordboxBrowser(std::shared_ptr<const ReaderFrame> frame);
    void syncLoadTargets();
    bool redirectControllerLoad(const std::map<int, QString> &before, const std::map<int, QString> &paths);
    void watchRekordbox();
    void stepAside(const QString &why, const std::vector<host::Popup> &popups);
    void comeBack();
    bool browseInRekordbox(const QString &service = {});
    void openStreaming(const QString &service);  // Beatport, Spotify: Rekordbox's own browser, only for this
    void leaveRekordbox(bool minimising = false);  // minimising: Wave stays minimised
    void connectRekordbox();
    void offerUpdate();
    void ensureDisplay();
    void bind(QScreen *screen);
    void leaveFullscreen();
    void toggleFullscreen();
    std::map<int, QString> deckPaths() const;
    QRect pageRect() const;
    QRect browsingRect() const;

    Session *session_;
    touch::Guard touch_;
    host::Setup setup_;
    Updater updater_;
    std::unique_ptr<host::PopupWatcher> popups_;
    midi::Listener *midi_ = nullptr;
    QWidget *root_;
    QFrame *toolbar_;
    Segmented *count_;
    QPushButton *browseButton_, *rekordboxButton_, *installButton_, *connectButton_, *updateButton_, *settingsButton_, *minimiseButton_, *closeButton_, *prefsButton_;
    QLabel *status_, *master_, *battery_, *clock_;
    QFrame *banner_;
    QLabel *bannerText_;
    QPushButton *bannerAction_;
    std::function<void()> bannerCallback_;
    QString bannerKey_;
    QTimer bannerTimer_, frameTimer_, watchTimer_, clockTimer_, screenTimer_, updateTimer_, saveTimer_;
    QStackedWidget *pages_;
    Canvas *canvas_;
    BrowserPage *browser_;
    SettingsPage *settings_ = nullptr;
    QWidget *dimmer_;

    std::optional<Update> update_;
    bool updating_ = false, checkAsked_ = false;
    std::optional<bool> plugged_;
    // Rekordbox behind Wave.
    QString aside_;                               // why Wave stepped aside for Rekordbox, while it has
    std::vector<quintptr> presented_;             // Rekordbox windows Wave put over itself (Windows)
    std::optional<double> stuckSince_;
    double closedSeen_ = 0;
    bool toldAccessibility_ = false;
    struct Browsing { QString kind; std::map<int, QString> tracks; quintptr hwnd = 0; bool main = false; Qt::WindowStates state; QString service;
                      std::optional<host::Embedded> embedded;
                      bool whole = false;
                      QSize prefs; };  // Rekordbox's Preferences (its own size, centred on black), when not empty  // the split browser (or, for REKORDBOX, Rekordbox's main window) inside Wave's page
    std::optional<Browsing> rekordboxBrowsing_;
    // Every Rekordbox window Wave has taken in (as it was), until given back: one left behind is put away by watch().
    std::map<quintptr, host::Embedded> taken_;
    std::optional<host::Embedded> take(quintptr hwnd, const QRect &rect, quintptr owner);
    void giveBack(const host::Embedded &e);
    void putAwayStrays();
    double minimisedAt_ = 0;
    quintptr mainTaken_ = 0, prefsTaken_ = 0;
    bool onTop_ = false;  // as last set, for the log  // Rekordbox's main window and Preferences, as Wave last took them
    bool splitAsked_ = false;
    quintptr splitWindow_ = 0;
    double splitReshownLogged_ = 0, splitScanAt_ = 0;
    QString hostError_;  // the last start or restart failure shown
    std::set<quintptr> splitWindows_;  // every one Rekordbox has given: none left showing
    QWidget *blank_ = nullptr;   // black behind Rekordbox's Preferences
    QWidget *pageBeforePrefs_ = nullptr;
    double prefsOpeningUntil_ = 0;  // Rekordbox's Preferences are opening: no popup handling for them meanwhile
    bool rekordboxShown_ = false;  // REKORDBOX pressed: Rekordbox and its browser up until Wave is used again
    int listFirst_ = -1;                          // the rows of Rekordbox's list Wave reads: -1 around its cursor                    // Rekordbox's split screen browser, once Wave has seen it open
    std::optional<QString> pendingStreaming_;     // Rekordbox's browser (on a service, or "") asked for while it was starting
    double pendingSince_ = 0;
    // The controller.
    std::optional<std::tuple<int, QString, int, int, int, std::vector<uint32_t>>> mirrored_;
    bool controllerSeen_ = false, knobLogged_ = false;
    double pressedAt_ = 0;
    double liveSince_ = 0;                        // when Rekordbox's bridge went live, for the controller
    std::map<int, QString> deckPaths_;
    struct Redirect { int deck; QString rekordboxPick; double deadline; };
    std::optional<Redirect> redirect_;
    // The monitor.
    QPointer<QScreen> connectedScreen_;
    bool holdWindowed_ = false;
    double boundAt_ = -1e9;
    double infoAt_ = 0;               // when the info bar was last drawn
};

QString screenKey(const QScreen *screen);

}  // namespace wave
