#include "ui/mainwindow.h"

#include "app/install.h"
#include "app/session.h"
#include "core/clock.h"
#include "host/rekordbox.h"
#include "host/windows.h"
#include "ui/browser.h"
#include "ui/canvas.h"
#include "ui/settings_page.h"
#include "ui/widgets.h"

#include <QApplication>
#include <QCloseEvent>
#include <QDesktopServices>
#include <QDir>
#include <QFileInfo>
#include <QHBoxLayout>
#include <QLabel>
#include <QLoggingCategory>
#include <QMessageBox>
#include <QPainter>
#include <QProcess>
#include <QScreen>
#include <QShortcut>
#include <QStackedWidget>
#include <QThread>
#include <QThreadPool>
#include <QTime>
#include <QUrl>
#include <QVBoxLayout>
#include <QWindow>

#include <cmath>

#ifdef Q_OS_WIN
#include <windows.h>
#include <dwmapi.h>
#endif

Q_LOGGING_CATEGORY(lcWindow, "wave.window")

namespace wave {
namespace {

// The booth dimmer: a black veil over the whole window that touches pass straight through.
class Dimmer : public QWidget {
public:
    Dimmer(QWidget *parent, std::function<double()> level) : QWidget(parent), level_(std::move(level)) {
        setAttribute(Qt::WA_TransparentForMouseEvents);
        setAttribute(Qt::WA_NoSystemBackground);
        parent->installEventFilter(this);
        refresh();
    }
    void refresh() {
        // Hidden at full brightness, so it costs nothing while it isn't wanted.
        setGeometry(parentWidget()->rect());
        setVisible(level_() < .999);
        raise();
        update();
    }
protected:
    bool eventFilter(QObject *, QEvent *e) override {
        if (e->type() == QEvent::Resize || e->type() == QEvent::ChildAdded) { setGeometry(parentWidget()->rect()); raise(); }
        return false;
    }
    void paintEvent(QPaintEvent *) override {
        const double level = level_();
        if (level < .999) QPainter(this).fillRect(rect(), QColor(0, 0, 0, int(std::lround(255 * (1 - std::max(.2, level))))));
    }
private:
    std::function<double()> level_;
};

bool samePath(QString a, QString b) {
    a = QDir::fromNativeSeparators(a).toCaseFolded();
    b = QDir::fromNativeSeparators(b).toCaseFolded();
    return !a.isEmpty() && !b.isEmpty() && (a == b || a.endsWith(b) || b.endsWith(a));
}

// True when a deck Wave already knew got a new track; a deck first seen, or emptied, is not a load.
bool trackLoaded(const std::map<int, QString> &before, const std::map<int, QString> &after) {
    for (const auto &[idx, path] : after) {
        const auto it = before.find(idx);
        if (!path.isEmpty() && it != before.end() && it->second != path) return true;
    }
    return false;
}

}  // namespace

QString screenKey(const QScreen *screen) {
    const QString serial = screen->serialNumber().trimmed();
    // Windows names screens by port (\\.\DISPLAY2): only fall back to it with no make or model.
    const bool named = !screen->manufacturer().isEmpty() || !screen->model().isEmpty();
    return QStringList{screen->manufacturer(), screen->model(), serial.isEmpty() ? (named ? QString() : screen->name()) : serial}.join('|');
}

MainWindow::MainWindow(Session *session, QWidget *parent)
    : QMainWindow(parent), session_(session), touch_(session->settings, [session] { session->save(); }) {
    setWindowTitle(QStringLiteral("Rekordbox Wave %1").arg(version()));
    setMinimumSize(640, 400);
    resize(1280, 800);
    setWindowFlag(Qt::WindowStaysOnTopHint, session_->settings.flag("always_on_top"));
    root_ = new QWidget;
    setCentralWidget(root_);
    auto *layout = new QVBoxLayout(root_);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(0);

    // The toolbar: deck layout, the browser, Rekordbox itself; what Wave needs from the DJ; the time.
    toolbar_ = new QFrame;
    toolbar_->setObjectName("toolbar");
    auto *top = new QHBoxLayout(toolbar_);
    top->setContentsMargins(12, 7, 12, 7);
    top->setSpacing(10);
    count_ = new Segmented({{QStringLiteral("2 DECKS"), 2}, {QStringLiteral("4 DECKS"), 4}}, session_->settings.deckCount());
    connect(count_, &Segmented::changed, this, [this](const QVariant &v) { setCount(v.toInt()); });
    top->addWidget(count_);
    browseButton_ = toolbarButton(QStringLiteral("BROWSE"), [this] { browse(); });
    browseButton_->setAccessibleName(QStringLiteral("Browse the Rekordbox collection"));
    top->addWidget(browseButton_);
    // Rekordbox itself, for anything only it can do: Wave steps aside; tap Wave to come back.
    rekordboxButton_ = toolbarButton(QStringLiteral("REKORDBOX"), [this] { showRekordbox(); });
    rekordboxButton_->setToolTip(QStringLiteral("Show Rekordbox in front of Wave. Tap Wave to come back."));
#if !defined(Q_OS_WIN) && !defined(Q_OS_MACOS)
    rekordboxButton_->hide();
#endif
    if (session_->demo()) rekordboxButton_->hide();
    top->addWidget(rekordboxButton_);
    top->addStretch();
    // The portable download, not installed yet: INSTALL puts it in Program Files with shortcuts.
    installButton_ = toolbarButton(QStringLiteral("INSTALL"), [this] { installWave(); });
    installButton_->setObjectName("install");
    installButton_->setToolTip(QStringLiteral("Install Rekordbox Wave in Program Files, with Start menu and desktop shortcuts"));
    installButton_->setVisible(install::offered());
    top->addWidget(installButton_);
    connectButton_ = toolbarButton(QStringLiteral("CONNECT"), [this] { connectRekordbox(); });
    connectButton_->setObjectName("connect");
    connectButton_->setAccessibleName(QStringLiteral("Start Rekordbox connected to Wave"));
    connectButton_->hide();
    top->addWidget(connectButton_);
    updateButton_ = toolbarButton(QStringLiteral("UPDATE"), [this] { offerUpdate(); });
    updateButton_->setObjectName("connect");
    updateButton_->hide();
    top->addWidget(updateButton_);
    status_ = new QLabel;
    status_->setObjectName("connection");
    status_->setTextFormat(Qt::RichText);
    status_->setMaximumWidth(510);
    top->addWidget(status_);
    master_ = new QLabel;
    master_->setObjectName("master");
    master_->setAccessibleName(QStringLiteral("Tempo master"));
    top->addWidget(master_);
    battery_ = new QLabel;
    battery_->setObjectName("battery");
    battery_->setTextFormat(Qt::RichText);
    battery_->hide();
    top->addWidget(battery_);
    clock_ = new QLabel(QTime::currentTime().toString("HH:mm"));
    clock_->setObjectName("clock");
    top->addWidget(clock_);
    settingsButton_ = toolbarButton(QStringLiteral("SETTINGS"), [this] { toggleSettings(); });
    top->addWidget(settingsButton_);
    layout->addWidget(toolbar_);

    // One line for what the DJ should know now: Rekordbox wants an answer, a notice Wave closed, a step
    // to take. Never a modal box during a set.
    banner_ = new QFrame;
    banner_->setObjectName("banner");
    auto *line = new QHBoxLayout(banner_);
    line->setContentsMargins(16, 6, 10, 6);
    line->setSpacing(12);
    bannerText_ = new QLabel;
    bannerText_->setObjectName("bannerText");
    bannerText_->setWordWrap(true);
    line->addWidget(bannerText_, 1);
    bannerAction_ = button(QString());
    bannerAction_->setObjectName("bannerAction");
    connect(bannerAction_, &QPushButton::clicked, this, [this] { if (bannerCallback_) bannerCallback_(); });
    line->addWidget(bannerAction_);
    auto *dismiss = button(QStringLiteral("✕"));
    dismiss->setObjectName("bannerClose");
    dismiss->setAccessibleName(QStringLiteral("Dismiss message"));
    connect(dismiss, &QPushButton::clicked, this, [this] { hideBanner(); });
    line->addWidget(dismiss);
    banner_->hide();
    layout->addWidget(banner_);
    bannerTimer_.setSingleShot(true);
    connect(&bannerTimer_, &QTimer::timeout, this, [this] { hideBanner(); });

    pages_ = new QStackedWidget;
    canvas_ = new Canvas(session_);
    connect(canvas_, &Canvas::settingsRequested, this, &MainWindow::toggleSettings);
    connect(canvas_, &Canvas::zoomBy, this, &MainWindow::zoomBy);
    pages_->addWidget(canvas_);
    browser_ = new BrowserPage;
    browser_->setOpenOn(session_->settings.text("tap_to_open"));
    browser_->setKeyboard(session_->settings.text("keyboard"));
    connect(browser_, &BrowserPage::streamingRequested, this, &MainWindow::openStreaming);
    // Beatport (or Spotify) in Wave's own browser: searched in Rekordbox, whose results Wave reads and loads.
    connect(browser_, &BrowserPage::streamingSearch, this, [this](const QString &query) {
        listFirst_ = 0;
        session_->readRekordboxList(0);
        session_->searchRekordbox(query, [this](bool ok, const QString &message) {
            if (!ok) browser_->showLoadStatus(message);
            qCInfo(lcWindow) << "Search in Rekordbox:" << ok << message;
        });
    });
    connect(browser_, &BrowserPage::streamingRows, this, [this](int first) {
        listFirst_ = first;
        session_->readRekordboxList(first);
    });
    connect(browser_, &BrowserPage::loadRowRequested, this, [this](int deck, int row, uint32_t id, const QString &title, const QString &path) {
        session_->loadRow(deck, row, id, title, path);
    });
    connect(browser_, &BrowserPage::loadRequested, this, [this](int deck, const CatalogTrack &t, const QString &source) {
        session_->loadTrack(deck, t, source);
    });
    connect(session_, &Session::loadStatus, browser_, &BrowserPage::showLoadStatus);
    pages_->addWidget(browser_);
    settings_ = new SettingsPage(this);
    pages_->addWidget(settings_);
    layout->addWidget(pages_, 1);
    dimmer_ = new Dimmer(root_, [this] { const double d = session_->settings.number("dim"); return d > 0 ? d : 1.0; });

    // Frames only while something moves: a deck playing, a preview gliding, a long title scrolling.
    frameTimer_.setTimerType(Qt::PreciseTimer);
    connect(&frameTimer_, &QTimer::timeout, this, &MainWindow::frame);
    connect(session_, &Session::changed, this, [this] {
        if (!frameTimer_.isActive()) { canvas_->update(); paceFrames(); }
    });
    connect(session_, &Session::analysisChanged, canvas_, qOverload<>(&QWidget::update));
    connect(session_, &Session::statusChanged, this, &MainWindow::updateStatus);
    connect(session_, &Session::frameArrived, this, &MainWindow::followRekordboxBrowser);
    watchTimer_.setInterval(250);
    connect(&watchTimer_, &QTimer::timeout, this, &MainWindow::watch);
    watchTimer_.start();
    clockTimer_.setInterval(1000);
    connect(&clockTimer_, &QTimer::timeout, this, &MainWindow::readouts);
    clockTimer_.start();
    screenTimer_.setInterval(1500);
    connect(&screenTimer_, &QTimer::timeout, this, &MainWindow::ensureDisplay);
    screenTimer_.start();
    saveTimer_.setSingleShot(true);
    saveTimer_.setInterval(600);  // a pinch or knob turn zooms many times a second: save once it settles
    connect(&saveTimer_, &QTimer::timeout, this, &MainWindow::persist);

    // Updates: when Wave starts and every six hours; installing waits for the DJ, never during playback.
    connect(&updater_, &Updater::checked, this, [this](std::optional<Update> u, const QString &error) {
        const bool asked = checkAsked_;
        checkAsked_ = false;
        if (!error.isEmpty()) {
            if (asked) QMessageBox::information(this, QStringLiteral("Check for updates"), QStringLiteral("GitHub could not be reached: %1").arg(error));
            return;  // tried again later, quietly
        }
        update_ = u;
        updateButton_->setVisible(u.has_value());
        if (u) {
            updateButton_->setText(QStringLiteral("UPDATE %1").arg(u->version));
            updateButton_->setToolTip(QStringLiteral("Rekordbox Wave %1 is on GitHub").arg(u->version));
        }
        if (asked)
            QMessageBox::information(this, QStringLiteral("Check for updates"),
                                     u ? QStringLiteral("Rekordbox Wave %1 is ready: press UPDATE at the top.").arg(u->version)
                                       : QStringLiteral("Rekordbox Wave %1 is the latest version.").arg(version()));
    });
    connect(&updater_, &Updater::progress, this, [this](int percent) { updateButton_->setText(QStringLiteral("UPDATING %1%").arg(percent)); });
    connect(&updater_, &Updater::failed, this, [this](const QString &why) {
        updateButton_->setEnabled(true);
        if (update_) updateButton_->setText(QStringLiteral("UPDATE %1").arg(update_->version));
        QMessageBox::information(this, QStringLiteral("Update Wave"), QStringLiteral("Wave did not update, and is unchanged: %1").arg(why));
    });
    connect(&updater_, &Updater::ready, this, [this] {
        updating_ = true;  // Rekordbox keeps running: the new Wave reconnects to it
        updateButton_->setText(QStringLiteral("RESTARTING…"));
        close();
        QApplication::quit();
    });
    updateTimer_.setInterval(6 * 3600 * 1000);
    connect(&updateTimer_, &QTimer::timeout, this, [this] { checkForUpdate(false); });
    updateTimer_.start();
    QTimer::singleShot(5000, this, [this] { checkForUpdate(false); });

    // Rekordbox setup (the supported version, when it is missing).
    connect(&setup_, &host::Setup::status, this, [this](const QString &t) { emit installProgress(-1, t); });
    connect(&setup_, &host::Setup::progress, this, [this](double f) { emit installProgress(f, {}); });
    connect(&setup_, &host::Setup::done, this, [this](bool ok, const QString &message) {
        emit installProgress(2, message);
        if (ok) {
            showBanner(QStringLiteral("Rekordbox %1 is installed: Wave starts it now.").arg(host::supportedText()), "info", {}, {}, 8000);
            session_->connectRekordbox();
        } else {
            showBanner(message, "warn", QStringLiteral("DOWNLOAD PAGE"), [] { QDesktopServices::openUrl(QUrl(QString::fromLatin1(host::kDownloadPage))); },
                       0, QStringLiteral("install"));
        }
    });

    if (!session_->demo()) {
#if defined(Q_OS_WIN) || defined(Q_OS_MACOS)
        popups_ = std::make_unique<host::PopupWatcher>();
        popups_->start();
#endif
        {
            // The browse knob, its push, BACK, VIEW and LOAD, and the zoom knobs. On Windows through Wave's bridge
            // in Rekordbox, which holds the controller (or directly where Windows shares it); on macOS directly.
            useController(new midi::Listener(this));
            midi_->start();
        }
        keepAwake(session_->settings.flag("keep_awake"));
        touch_.apply();  // Windows' touch keyboard off while Wave runs with its own
    }

    qApp->setStyleSheet(wave::styleSheet(session_->settings.number("settings_scale")));
    syncNav();
    updateStatus();
    for (const auto &[key, action] : std::vector<std::pair<QKeySequence, std::function<void()>>>{
             {QKeySequence(Qt::Key_F2), [this] { toggleSettings(); }}, {QKeySequence(Qt::Key_F11), [this] { toggleFullscreen(); }},
             {QKeySequence(Qt::Key_Escape), [this] { escape(); }}}) {
        auto *s = new QShortcut(key, this);
        connect(s, &QShortcut::activated, this, action);
    }
    for (const auto &[key, factor] : std::vector<std::pair<QString, double>>{{"+", .5}, {"=", .5}, {"-", 2}}) {
        auto *s = new QShortcut(QKeySequence(key), canvas_);
        s->setContext(Qt::WidgetWithChildrenShortcut);
        connect(s, &QShortcut::activated, this, [this, factor = factor] { zoomBy(factor); });
    }
    connect(qApp, &QGuiApplication::screenAdded, this, [this] { holdWindowed_ = false; settings_->refreshScreens(); ensureDisplay(); });
    connect(qApp, &QGuiApplication::screenRemoved, this, [this] { settings_->refreshScreens(); ensureDisplay(); });
    QTimer::singleShot(0, this, &MainWindow::ensureDisplay);
    if (!session_->settingsError.isEmpty()) showBanner(session_->settingsError, "warn", {}, {}, 15000);
    qApp->installEventFilter(this);
}

MainWindow::~MainWindow() {
    if (popups_) popups_->stop();
    if (midi_) midi_->stop();
}

QPushButton *MainWindow::toolbarButton(const QString &title, std::function<void()> action) {
    QPushButton *b = button(title);
    b->setFocusPolicy(Qt::NoFocus);  // the keyboard stays with the decks and the browser
    connect(b, &QPushButton::clicked, this, action);
    return b;
}

// --- Frames --------------------------------------------------------------------------------------------

void MainWindow::frame() {
    const bool playing = session_->animating();
    if (!(playing || canvas_->needsFrames()) || pages_->currentWidget() != canvas_ || isMinimized()) {
        frameTimer_.stop();  // idle: no frames until something changes
        canvas_->update();
        return;
    }
    if (!playing) {  // only a long title scrolling or the end warning: the info bar alone, 20 times a second
        if (frameTimer_.interval() != 50) frameTimer_.setInterval(50);
        canvas_->frame(true);
        return;
    }
#ifdef Q_OS_WIN
    // Wake just before the refresh and let the compositor release the paint: one paint per refresh.
    DwmFlush();
#endif
    const double t = now();
    const bool info = t - infoAt_ >= .05;
    if (info) infoAt_ = t;
    canvas_->frame(info);
    paceFrames();
}

void MainWindow::paceFrames() {
    if (pages_->currentWidget() != canvas_ || isMinimized()) return;
    const bool playing = session_->animating();
    if (!(playing || canvas_->needsFrames())) return;
    QScreen *s = screen() ? screen() : QGuiApplication::primaryScreen();
    const double hz = std::clamp(std::min(session_->settings.number("refresh_hz"), s ? s->refreshRate() : 60.0), 20.0, 240.0);
#ifdef Q_OS_WIN
    const int interval = playing ? std::max(1, int(std::floor(1000 / hz)) - 4) : 50;
#else
    const int interval = playing ? std::max(1, int(std::lround(1000 / hz))) : 50;
#endif
    if (frameTimer_.interval() != interval) frameTimer_.setInterval(interval);
    if (!frameTimer_.isActive()) frameTimer_.start();
}

// --- Status ----------------------------------------------------------------------------------------------

QString MainWindow::pillText() const {
    // The toolbar's connection light and a few words; the whole story is in its tooltip and Settings.
    const auto f = session_->lastFrame();
    const host::Host *h = session_->host();
    QString colour, words;
    if (session_->demo()) { colour = "#8d96a0"; words = "DEMO"; }
    else if (!aside_.isEmpty()) { colour = "#ff9f1a"; words = "REKORDBOX NEEDS YOU"; }
    else if (h && !h->doing.isEmpty()) { colour = "#ff9f1a"; words = h->doing.toUpper() + QStringLiteral("…"); }
    else if (!f) { colour = "#8d96a0"; words = session_->status().left(60); }
    else switch (f->state) {
        case host::State::Live: colour = "#2fd16b"; words = "REKORDBOX"; break;
        case host::State::Starting: colour = "#ff9f1a"; words = "REKORDBOX STARTING"; break;
        case host::State::Busy: colour = "#ff9f1a"; words = "REKORDBOX BUSY"; break;
        case host::State::Unsupported:
        case host::State::Failed: colour = "#ff4d4d"; words = "UNSUPPORTED REKORDBOX"; break;
        case host::State::None:
        case host::State::Older: colour = "#ff9f1a"; words = "CONNECTING"; break;
        case host::State::Absent: colour = "#ff4d4d"; words = "REKORDBOX OFF"; break;
    }
    return QStringLiteral("<span style=\"color:%1; font-size:20px;\">●</span>&nbsp;%2").arg(colour, words.toHtmlEscaped());
}

void MainWindow::updateStatus() {
    const QString pill = pillText();
    if (status_->text() != pill) status_->setText(pill);
    status_->setToolTip(aside_.isEmpty() ? session_->status() : aside_);
    if (settings_) settings_->setStatus(aside_.isEmpty() ? session_->status() : aside_);
}

void MainWindow::readouts() {
    clock_->setText(QTime::currentTime().toString("HH:mm"));
    const Settings &s = session_->settings;
    QString master;
    if (s.flag("master_bpm"))
        for (int i = 0; i < 4; ++i) {
            const auto &st = session_->deck(i).status;
            if (st && st->master && st->bpm > 0) master = QStringLiteral("MASTER %1 · %2").arg(i + 1).arg(st->bpm, 0, 'f', 1);
        }
    master_->setText(master);
    master_->setVisible(!master.isEmpty());
    const auto b = s.flag("battery") ? battery() : std::nullopt;
    battery_->setVisible(b.has_value());
    if (!b) return;
    const QString colour = b->plugged ? "#2fd16b" : b->percent <= 20 ? "#ff4d4d" : b->percent <= 40 ? "#ff9f1a" : "#d9dde2";
    battery_->setText(QStringLiteral("<span style=\"color:%1\">%2 %3%</span>").arg(colour, b->plugged ? QStringLiteral("⚡") : QStringLiteral("▮")).arg(b->percent));
    if (plugged_ == true && !b->plugged) {
        showBanner(QStringLiteral("Running on battery: %1%. Check the charger.").arg(b->percent), "warn", {}, {}, 15000, "battery");
        qCWarning(lcWindow) << "Charger unplugged at" << b->percent << "%";
    } else if (b->plugged && bannerKey_ == "battery") {
        hideBanner("battery");
    }
    if (!b->plugged && b->percent <= 15 && bannerKey_ != "battery")
        showBanner(QStringLiteral("Battery low: %1%. Plug in the charger.").arg(b->percent), "warn", {}, {}, 0, "battery");
    plugged_ = b->plugged;
}

void MainWindow::showBanner(const QString &text, const QString &tone, const QString &action, std::function<void()> callback,
                            int timeoutMs, const QString &key) {
    bannerKey_ = key;
    bannerText_->setText(text);
    mark(banner_, "tone", tone);
    bannerCallback_ = std::move(callback);
    bannerAction_->setText(action);
    bannerAction_->setVisible(!action.isEmpty());
    banner_->show();
    if (timeoutMs) bannerTimer_.start(timeoutMs);
    else bannerTimer_.stop();
}

void MainWindow::hideBanner(const QString &key) {
    if (key.isEmpty() || key == bannerKey_) {
        banner_->hide();
        bannerKey_.clear();
    }
}

// --- Navigation ------------------------------------------------------------------------------------------

void MainWindow::syncNav() {
    // One visual language for the toolbar: the active view glows, like the deck buttons.
    mark(browseButton_, "selected", pages_->currentWidget() == browser_ || rekordboxBrowsing_.has_value());
    mark(settingsButton_, "selected", pages_->currentWidget() == settings_);
}

void MainWindow::toggleSettings() {
    const bool opening = pages_->currentWidget() != settings_;
    pages_->setCurrentWidget(opening ? static_cast<QWidget *>(settings_) : canvas_);
    syncNav();
    if (opening) settings_->refresh();
    else { canvas_->setFocus(); canvas_->update(); paceFrames(); }
}

void MainWindow::showPage(const QString &name) {
    if (name == QLatin1String("browser")) { if (pages_->currentWidget() != browser_) toggleBrowser(); return; }
    if (pages_->currentWidget() != settings_) toggleSettings();
    if (name != QLatin1String("settings")) settings_->openPage(name.toUpper());
}

void MainWindow::browse(bool byKnob) {
    // BROWSE on screen, or VIEW on the controller: the browser chosen in Settings opens, or closes.
    if (rekordboxBrowsing_) leaveRekordbox();
    else if (session_->settings.text("browser") == QLatin1String("rekordbox") && pages_->currentWidget() != browser_) {
        if (!browseInRekordbox()) {
            showBanner(QStringLiteral("Rekordbox's browser needs Rekordbox running with Wave: here is Wave's browser meanwhile"), "info", {}, {}, 5000, "browser");
            toggleBrowser(byKnob);
        }
    } else {
        toggleBrowser(byKnob);
    }
}

void MainWindow::toggleBrowser(bool byKnob) {
    const bool opening = pages_->currentWidget() != browser_;
    pages_->setCurrentWidget(opening ? static_cast<QWidget *>(browser_) : canvas_);
    syncNav();
    if (opening && byKnob) browser_->focusForKnob();  // opened from the controller: no keyboard pops up
    else if (!opening) { canvas_->update(); paceFrames(); }
    if (opening) syncLoadTargets();
}

void MainWindow::escape() {
    if (pages_->currentWidget() == browser_) {
        if (browser_->keyboardShown()) browser_->hideKeyboard();
        else if (!browser_->goBack()) toggleBrowser();
    } else if (pages_->currentWidget() == settings_) {
        toggleSettings();
    } else if (isFullScreen()) {
        toggleFullscreen();
    }
}

// --- The controller ----------------------------------------------------------------------------------------

void MainWindow::useController(midi::Listener *listener) {
    midi_ = listener;
    connect(midi_, &midi::Listener::controller, this, &MainWindow::onController, Qt::QueuedConnection);
}

void MainWindow::onController(midi::Kind kind, int value, int deck) {
    const Settings &s = session_->settings;
    if (kind == midi::Kind::SamplerZoom) {
        if (s.flag("sampler_zoom")) {
            session_->knobZoom(value, s.flag("sampler_zoom_reverse"));
            canvas_->update();
        }
        return;
    }
    if (kind == midi::Kind::Zoom) {  // Rekordbox's own waveform zoom (SHIFT + BROWSE)
        zoomBy(std::pow(2.0, -value / 4.0));
        return;
    }
    if (kind == midi::Kind::Press) pressedAt_ = now();  // a load that follows is the same push, not a second one
    if (!controllerSeen_) {
        controllerSeen_ = true;  // from now on the controller's own messages drive the browser
        qCInfo(lcWindow) << "The browse knob is read from the controller" << (midi_ && midi_->throughRekordbox() ? "through Rekordbox" : "itself");
    }
    if (kind == midi::Kind::View) {
        browse(true);  // VIEW opens the browser, and closes it again
        return;
    }
    if (s.text("browser") == QLatin1String("rekordbox")) return;  // Rekordbox's own browser follows its controller
    // Kept from Rekordbox: Wave's browser is the only one these controls move, and Wave loads its own pick.
    // Otherwise Rekordbox acts on them too, and its LOAD is seen (and swapped for Wave's pick) when a deck changes.
    const bool own = midi_ && midi_->keeping();
    if (pages_->currentWidget() != browser_) {
        // The knob turned or pushed on the waveforms: the browser opens, as a deck's does.
        if (own && pages_->currentWidget() == canvas_ && (kind == midi::Kind::Browse || kind == midi::Kind::Press))
            toggleBrowser(true);
        return;
    }
    if (kind == midi::Kind::Browse) {
        browser_->step(value);
    } else if (kind == midi::Kind::Press) {
        if (browser_->showsList()) browser_->enter();  // a playlist or folder opens
        else if (own && deck >= 0) browser_->loadHighlighted(deck);  // a track: onto the deck the push belongs to
        else if (own) browser_->enter();
    } else if (kind == midi::Kind::Load) {
        if (own && deck >= 0) browser_->loadHighlighted(deck);
    } else if (kind == midi::Kind::Back && !browser_->goBack()) {
        toggleBrowser();  // BACK at the top: back to the waveforms
    }
}

void MainWindow::followRekordboxBrowser(std::shared_ptr<const ReaderFrame> f) {
    // The controller's browse knob, seen through Rekordbox's own browser, for when Wave can't read the
    // controller itself. Only while Wave's touch browser is open: a cursor move in Rekordbox's list moves
    // Wave's highlight; Rekordbox moving to another playlist highlights that one.
    QStringList roots;
    for (const auto &[h, root] : f->devices) roots << root;
    browser_->setDeviceRoots(roots);
    if (!f->bridged || !f->browser) { mirrored_.reset(); return; }
    if (browser_->streaming() && pages_->currentWidget() == browser_) {
        browser_->streamingList(*f->browser);
    } else if (listFirst_ != -1) {
        listFirst_ = -1;
        session_->readRekordboxList(-1);  // Rekordbox's list around its cursor again, for the knob
    }
    const bridge::Browser &b = *f->browser;
    std::vector<uint32_t> ids;
    for (const auto &[id, path] : b.tracks) ids.push_back(id);
    const auto key = std::make_tuple(b.cursor, b.focus + '|' + b.treePath, b.rows, b.first, 0, ids);
    if (mirrored_ && *mirrored_ == key) return;
    const auto previous = mirrored_;
    mirrored_ = key;
    if (!previous) return;  // the first look is where Rekordbox was, not a move
    const int cursor = std::get<0>(key), before = std::get<0>(*previous);
    const bool cursorMoved = cursor != before && cursor >= 0 && before >= 0;
    bool overlap = false;
    for (uint32_t id : ids) for (uint32_t old : std::get<5>(*previous)) overlap = overlap || id == old;
    const bool listChanged = std::get<2>(key) != std::get<2>(*previous) || std::get<1>(key) != std::get<1>(*previous) ||
                             (!cursorMoved && std::get<3>(key) == std::get<3>(*previous) && !ids.empty() && !std::get<5>(*previous).empty() && !overlap);
    if (!cursorMoved && !listChanged) return;  // Rekordbox's rows refreshed by themselves
    if (session_->settings.text("browser") == QLatin1String("rekordbox") || controllerSeen_) return;
    if (pages_->currentWidget() != browser_) return;
    if (cursorMoved) {
        if (!knobLogged_) { knobLogged_ = true; qCInfo(lcWindow) << "The browse knob is followed through Rekordbox's browser"; }
        browser_->step(cursor - before);
    } else if (listChanged && browser_->showsList()) {
        // The knob in Rekordbox's tree: highlight the same list here, if Wave shows that library.
        QString first;
        for (const auto &[id, path] : b.tracks) if (!path.isEmpty()) { first = path; break; }
        const QString source = first.isEmpty() ? QString() : browser_->sourceFor(first);
        browser_->locate(source, ids, b.rows, [this, source](std::optional<Located> found) {
            if (!found || pages_->currentWidget() != browser_ || source != browser_->sourcePath()) return;
            browser_->highlightItem(found->id);
        });
    }
}

std::map<int, QString> MainWindow::deckPaths() const {
    std::map<int, QString> paths;
    if (const auto f = session_->lastFrame(); f && f->bridged)
        for (size_t i = 0; i < f->decks.size(); ++i) paths[int(i)] = f->decks[i].path;
    return paths;
}

void MainWindow::syncLoadTargets() {
    const host::Host *h = session_->host();
    connectButton_->setVisible(h && !h->policy.ask.isEmpty());
    const auto paths = deckPaths();
    const auto before = deckPaths_;
    deckPaths_ = paths;
    if (rekordboxBrowsing_) {
        if (trackLoaded(rekordboxBrowsing_->tracks, paths)) leaveRekordbox();
        else if (rekordboxBrowsing_->kind == "window" && !host::alive(rekordboxBrowsing_->hwnd)) leaveRekordbox();  // closed in Rekordbox
        else if (rekordboxBrowsing_->kind == "whole" && !isMinimized()) rekordboxBrowsing_.reset();  // back in Wave by hand
    }
    if (redirect_) {
        // While a redirected load is under way, wait for it (or give up) before leaving the browser.
        if (session_->loading() && now() < redirect_->deadline) return;
        const auto it = paths.find(redirect_->deck);
        const bool swapped = it != paths.end() && !it->second.isEmpty() && it->second != redirect_->rekordboxPick;
        redirect_.reset();
        if (swapped && pages_->currentWidget() == browser_) toggleBrowser();  // Wave's pick is on the deck
        return;  // if it didn't land, the browser stays, with Rekordbox's answer in its message line
    }
    if (pages_->currentWidget() == browser_ && trackLoaded(before, paths)) {
        if (redirectControllerLoad(before, paths)) return;
        toggleBrowser();  // a track landed on a deck, from the screen or the controller: back to the waveforms
        return;
    }
    if (pages_->currentWidget() == browser_) {
        std::map<int, QString> locked;
        for (int i = 0; i < 4; ++i)
            locked[i] = session_->playing(i) ? QStringLiteral("Deck %1 is playing: stop it before loading over it").arg(i + 1) : QString();
        browser_->setDecks(session_->order(), locked, session_->loadUnavailable(), paths);
    }
}

bool MainWindow::redirectControllerLoad(const std::map<int, QString> &before, const std::map<int, QString> &paths) {
    // A deck changed while Wave's touch browser is open, and Wave didn't load it: that is the controller's
    // LOAD, which loads Rekordbox's own pick. Load Wave's pick onto that deck instead; with nothing picked in
    // Wave, put back what the deck had.
    if (!session_->settings.flag("knob_loads_wave")) return false;
    if (midi_ && midi_->keeping()) return false;  // Rekordbox never sees the knob: a deck change is someone else's
    const auto picked = browser_->selectedTrack();
    for (const auto &[idx, path] : paths) {
        const auto old = before.find(idx);
        if (old == before.end() || old->second == path || path.isEmpty() || session_->loadedByWave(idx, path)) continue;
        if (picked && samePath(path, picked->path)) return false;  // Rekordbox loaded what Wave shows anyway
        if (!picked && browser_->showsList() && now() - pressedAt_ > 1.5) browser_->enter();  // the push opened a playlist here
        if (picked) {
            if (session_->loadTrack(idx, *picked, browser_->sourcePath())) {
                redirect_ = Redirect{idx, path, now() + 10};
                browser_->showLoadStatus(QStringLiteral("Loading %1 on deck %2, as picked here…").arg(picked->title).arg(idx + 1));
                qCInfo(lcWindow) << "Controller LOAD on deck" << idx + 1 << ": loading the track picked in Wave instead";
                return true;
            }
            return false;
        }
        if (old->second.isEmpty()) {
            browser_->showLoadStatus(QStringLiteral("Rekordbox loaded a track on deck %1: Wave can't unload it. Highlight a track here, then LOAD.").arg(idx + 1));
            return false;
        }
        // Nothing picked in Wave: the deck gets back what it had before the stray LOAD.
        redirect_ = Redirect{idx, path, now() + 10};
        const int deck = idx;
        const QString oldPath = old->second;
        browser_->tracksForPaths({oldPath}, [this, deck, oldPath](std::map<QString, CatalogTrack> found) {
            const auto it = found.find(oldPath);
            if (it == found.end() || it->second.id.isEmpty()) return;
            const auto [track, source] = browser_->forLoading(it->second);
            session_->loadTrack(deck, track, source);
        });
        browser_->showLoadStatus(QStringLiteral("Nothing was picked here: putting the deck back as it was."));
        qCInfo(lcWindow) << "Controller LOAD on deck" << idx + 1 << "with nothing picked in Wave: restoring its track";
        return true;
    }
    return false;
}

// --- Rekordbox behind Wave ---------------------------------------------------------------------------------

void MainWindow::watch() {
    // Rekordbox's browser inside Wave's page follows the page (a resize, another screen, full screen).
    if (rekordboxBrowsing_ && rekordboxBrowsing_->embedded) host::place(rekordboxBrowsing_->hwnd, pageRect());
    if (midi_ && !session_->demo()) {
        const auto f = session_->lastFrame();
        const bool live = f && f->state == host::State::Live;
        liveSince_ = live ? (liveSince_ > 0 ? liveSince_ : now()) : 0;
        midi_->setAllowed(live && now() - liveSince_ > 8);  // Rekordbox has opened its controllers by then
        // Wave's browser takes the browser controls from Rekordbox while Wave is the screen: not while the DJ
        // chose Rekordbox's browser, Rekordbox's own browser is up (Beatport, Spotify), or Wave stepped aside.
        const Settings &s = session_->settings;
        // While Rekordbox's own browser is up the knob is Rekordbox's, and VIEW only Wave's: it brings Wave back.
        const bool wave = s.text("browser") == QLatin1String("wave") && s.flag("knob_loads_wave");
        midi_->setKeeping(!live || !aside_.isEmpty() ? midi::Keep::Nothing
                          : rekordboxBrowsing_       ? midi::Keep::View
                          : wave && !isMinimized()   ? midi::Keep::Browser
                                                     : midi::Keep::Nothing);
    }
    paceFrames();  // frames start as soon as something moves (a deck starts, a preview begins)
    if (!pendingStreaming_.isEmpty()) {
        const auto f = session_->lastFrame();
        if (f && f->state == host::State::Live && browseInRekordbox(pendingStreaming_)) pendingStreaming_.clear();
        else if (now() - pendingSince_ > 90) {
            showBanner(QStringLiteral("Rekordbox didn't come up for %1: press REKORDBOX to open it yourself").arg(pendingStreaming_), "warn", {}, {}, 10000, "streaming");
            pendingStreaming_.clear();
        }
    }
    syncLoadTargets();
    watchRekordbox();
    updateStatus();
}

void MainWindow::watchRekordbox() {
    // Rekordbox behind Wave can stop at its own questions (library sync, My Tag, sign-in…), show a popup Wave
    // could not close for the DJ, or hang on a music drive that stopped answering. Then Wave steps aside and
    // puts Rekordbox in front, and comes back once Rekordbox is running again.
    const auto f = session_->lastFrame();
    const host::State state = f ? f->state : host::State::Absent;
    const double t = now();
    std::vector<host::Popup> popups;
    if (popups_) {
        popups_->pid = f ? f->pid : 0;
        popups_->autoClose = session_->settings.flag("close_popups") && !rekordboxBrowsing_;
        popups_->setExclude(rekordboxBrowsing_ && rekordboxBrowsing_->hwnd ? std::set<quintptr>{rekordboxBrowsing_->hwnd} : std::set<quintptr>{});
        const QString error = popups_->error();
#ifdef Q_OS_MACOS
        if (!error.isEmpty() && !toldAccessibility_ && error.contains("Accessibility")) {
            toldAccessibility_ = true;
            showBanner(QStringLiteral("Allow Rekordbox Wave in Accessibility so it can close Rekordbox's popups for you"), "info",
                       QStringLiteral("OPEN SETTINGS"), [] {
                           QDesktopServices::openUrl(QUrl("x-apple.systempreferences:com.apple.preference.security?Privacy_Accessibility"));
                       }, 20000, "accessibility");
        }
#endif
        const auto closed = popups_->closed();
        if (!closed.empty() && closed.back().first > closedSeen_) {
            closedSeen_ = closed.back().first;
            showBanner(QStringLiteral("Closed a Rekordbox notice: %1").arg(closed.back().second), "info", {}, {}, 6000, "closed");
        }
        // A popup with buttons wants an answer; one without (Rekordbox's splash) only counts once it is stuck.
        if (!rekordboxBrowsing_)
            for (const host::Popup &p : popups_->popups()) if (!p.buttons.isEmpty() && t - p.firstSeen > 1.5) popups.push_back(p);
    }
    const bool stuck = state == host::State::Starting || state == host::State::Busy;
    stuckSince_ = stuck ? std::optional<double>(stuckSince_.value_or(t)) : std::nullopt;
    const double patience = state == host::State::Starting ? 20 : 8;  // a cold start takes a while
    const bool waiting = stuckSince_ && t - *stuckSince_ > patience;
    if (!popups.empty() || waiting) {
        stepAside(!popups.empty() ? QStringLiteral("Rekordbox asks: %1").arg(popups.front().summary())
                  : state == host::State::Starting ? QStringLiteral("Rekordbox is waiting: answer its questions, then Wave connects")
                  : QStringLiteral("Rekordbox is not responding: answer it if it asks something, or check your music drive"),
                  popups);
    } else if (!aside_.isEmpty() && !stuck) {
        comeBack();
    }
}

void MainWindow::stepAside(const QString &why, const std::vector<host::Popup> &popups) {
    // Put Rekordbox, or just its popups, in front of Wave until it is answered.
    const bool first = aside_.isEmpty();
    if (why != aside_) {
        aside_ = why;
        showBanner(why + QStringLiteral(" · Wave comes back by itself"), "warn", {}, {}, 0, "aside");
        qCInfo(lcWindow) << "Stepping aside for Rekordbox:" << why;
    }
#ifdef Q_OS_WIN
    const auto f = session_->lastFrame();
    if (!f || !f->pid) return;
    std::vector<quintptr> handles;
    for (const host::Popup &p : popups) if (p.hwnd) handles.push_back(p.hwnd);
    if (handles.empty()) {
        const auto c = host::classify(host::windowsOf(f->pid));
        for (const host::Win &w : c.popups) handles.push_back(w.hwnd);
        if (handles.empty() && c.main) handles.push_back(c.main->hwnd);
    }
    const QPoint centre = host::windowRect(quintptr(winId())).center();
    for (quintptr hwnd : handles)
        if (std::find(presented_.begin(), presented_.end(), hwnd) == presented_.end()) {
            host::present(hwnd, std::nullopt, centre);
            presented_.push_back(hwnd);
        }
#else
    Q_UNUSED(popups);
    if (first) {
        setOnTop(false);
        host::show();
    }
#endif
}

void MainWindow::comeBack() {
    // Rekordbox is answered: Wave on top again, Rekordbox out of sight.
    aside_.clear();
    hideBanner("aside");
    for (quintptr hwnd : presented_) host::putBack(hwnd, session_->settings.flag("hide_rekordbox"));
    presented_.clear();
    setOnTop(session_->settings.flag("always_on_top"));
    raise();
    activateWindow();
    qCInfo(lcWindow) << "Rekordbox answered: Wave is back in front";
}

void MainWindow::setOnTop(bool on) {
#ifdef Q_OS_WIN
    host::setTopmost(quintptr(winId()), on);  // without leaving full screen
#else
    if (windowHandle()) windowHandle()->setFlag(Qt::WindowStaysOnTopHint, on);
#endif
}

QRect MainWindow::pageRect() const {
    // The part of Wave below its toolbar, in the physical pixels Win32 uses.
    QRect r = host::windowRect(quintptr(winId()));
    r.setTop(r.top() + int(std::lround(pages_->mapTo(this, QPoint(0, 0)).y() * devicePixelRatioF())));
    return r;
}

bool MainWindow::browseInRekordbox(const QString &service) {
    // Rekordbox's browser in front until a track is loaded. macOS, through the bridge: its Split Screen
    // browser window over Wave. Windows: its browser window - or its main window - over Wave, below the toolbar.
    // For a streaming service Wave stops being always-on-top meanwhile, so a sign-in (Rekordbox's own, or a
    // web page) can come to the front.
    const auto f = session_->lastFrame();
    const bool streaming = !service.isEmpty();
#ifdef Q_OS_MACOS
    if (!session_->bridged()) return false;
    if (streaming) setOnTop(false);
    const QPoint centre = frameGeometry().center();
    session_->showRekordboxBrowser(true, centre.x(), centre.y());
    rekordboxBrowsing_ = Browsing{"mac", deckPaths(), 0, false, windowState(), service};
#elif defined(Q_OS_WIN)
    const uint32_t pid = f && f->pid ? f->pid : host::rekordboxPid();
    if (!pid) return false;
    const auto windows = host::windowsOf(pid);
    const auto c = host::classify(windows);
    std::optional<host::Win> target;
    // The split screen browser, by the window it opened as (it can be small: it isn't told apart by its size).
    if (splitWindow_ && host::alive(splitWindow_))
        for (const host::Win &w : windows) if (w.hwnd == splitWindow_) target = w;
    // Already open (Wave puts it away minimised; asking again would close it, as the command is a toggle): the window
    // with the split browser's title, as Wave saw it the first time it opened. Never another of Rekordbox's windows
    // (OneLibrary, INFO, Product registration... share its window class).
    const QString splitTitle = session_->settings.text("split_window_title");
    if (!target && !splitTitle.isEmpty())
        for (const host::Win &w : windows)
            if (w.title == splitTitle && !w.tool && (w.visible || w.minimized) && (!c.main || w.hwnd != c.main->hwnd)) {
                target = w;
                splitWindow_ = w.hwnd;
                break;
            }
    if (!target && c.main && !splitAsked_) {
        // Only the browser, in the window of its own that Rekordbox's split screen button (bottom right of its browser)
        // opens: through the bridge as that button runs it, or by pressing that button (btnMultiWindow) when the bridge
        // isn't there. The window it adds is the browser. Rekordbox's main window is never shown in its place.
        splitAsked_ = true;
        std::set<quintptr> before;
        for (const host::Win &w : windows) before.insert(w.hwnd);
        auto look = std::make_shared<std::function<void(int)>>();
        *look = [this, service, look, before](int left) {
            const auto f = session_->lastFrame();
            const uint32_t pid = f && f->pid ? f->pid : host::rekordboxPid();
            for (const host::Win &w : host::windowsOf(pid))
                if (!before.count(w.hwnd) && w.visible && !w.tool && w.area() > 0) {
                    splitWindow_ = w.hwnd;
                    if (w.title != session_->settings.text("split_window_title")) {
                        session_->settings.setView("split_window_title", w.title);  // known from now on, also after a restart
                        saveTimer_.start();
                    }
                    qCInfo(lcWindow) << "Rekordbox's split screen browser opened:" << w.title << w.rect;
                    break;
                }
            if (!splitWindow_ && left > 0) {
                QTimer::singleShot(250, this, [look, left] { (*look)(left - 1); });
                return;
            }
            bool shown = false;
            if (splitWindow_) shown = browseInRekordbox(service);
            else {
                qCWarning(lcWindow) << "Rekordbox's split screen browser didn't open";
                showBanner(QStringLiteral("Rekordbox's browser didn't open: tap %1 again").arg(service.isEmpty() ? QStringLiteral("BROWSE") : service),
                           "warn", {}, {}, 8000, "split");
            }
            splitAsked_ = false;
            if (!shown && pages_->currentWidget() == browser_) toggleBrowser();
        };
        const quintptr main = c.main->hwnd;
        auto byButton = [this, look, main] {
            // Rekordbox's own split screen button, as a tap on it: off the UI thread (Rekordbox may be slow to answer).
            QThreadPool::globalInstance()->start([this, look, main] {
                host::Automation automation;
                const bool pressed = automation.press(main, QStringLiteral("btnMultiWindow"));
                QMetaObject::invokeMethod(this, [look, pressed] {
                    if (!pressed) qCWarning(lcWindow) << "Rekordbox's split screen button wasn't there";
                    QTimer::singleShot(250, [look] { (*look)(16); });
                });
            });
        };
        if (!session_->bridged()) { byButton(); return true; }
        session_->showRekordboxBrowser(true, 0, 0, [look, byButton](bool ok, const QString &message) {
            if (ok) { QTimer::singleShot(250, [look] { (*look)(16); }); return; }
            qCWarning(lcWindow) << "Split screen browser through the bridge:" << message;  // an older bridge: its button then
            byButton();
        });
        return true;
    }
    if (!target) return splitAsked_;  // never Rekordbox's main window: only its browser belongs inside Wave
    if (streaming) setOnTop(false);
    // The split browser becomes part of Wave: in Wave's page, under its toolbar, without a frame of its own.
    const bool split = !c.main || target->hwnd != c.main->hwnd;
    if (pages_->currentWidget() == browser_) toggleBrowser();  // the waveforms behind it, for when it goes
    std::optional<host::Embedded> embedded;
    if (split) embedded = host::embed(target->hwnd, quintptr(winId()), pageRect());
    if (embedded && c.main) host::sendToBack(c.main->hwnd);  // opening the split browser can bring Rekordbox forward
    if (!embedded) host::present(target->hwnd, pageRect(), std::nullopt, true, !streaming);
    rekordboxBrowsing_ = Browsing{"window", deckPaths(), target->hwnd, c.main && target->hwnd == c.main->hwnd, windowState(), service, embedded};
    if (streaming) {
        // Rekordbox's tree has the service by name: select it there, off the UI thread (Rekordbox may be slow to answer).
        const quintptr hwnd = target->hwnd;
        QThreadPool::globalInstance()->start([hwnd, service] {
            host::Automation automation;
            for (int attempt = 0; attempt < 6 && !automation.select(hwnd, service); ++attempt) QThread::msleep(300);
        });
    }
#else
    Q_UNUSED(f);
    return false;
#endif
    syncNav();
    if (streaming)
        showBanner(QStringLiteral("%1 in Rekordbox: pick a track and load it (sign in there if it asks). Wave comes back by itself.").arg(service),
                   "info", QStringLiteral("BACK TO WAVE"), [this] { leaveRekordbox(); }, 0, "streaming");
    qCInfo(lcWindow) << "Rekordbox's browser is up over Wave" << service;
    return true;
}

void MainWindow::openStreaming(const QString &service) {
    // Rekordbox is opened only for this: started (hidden) if it isn't running, shown when it is ready.
    if (rekordboxBrowsing_) leaveRekordbox();
    if (session_->demo()) {
        showBanner(QStringLiteral("%1 streams through Rekordbox: start Wave without --demo").arg(service), "info", {}, {}, 5000);
        return;
    }
#ifdef Q_OS_WIN
    // Archived (3.0.10): Wave's own browser on the service, searched through Rekordbox (BrowserPage::showStreaming).
    // Rekordbox's search box wasn't reliably reachable; off, with no switch in Settings. Kept for later.
    if (session_->settings.flag("native_streaming_archived") && session_->bridged()) {
        if (pages_->currentWidget() != browser_) toggleBrowser();
        browser_->showStreaming(service);
        listFirst_ = 0;
        session_->readRekordboxList(0);
        return;
    }
#endif
    if (browseInRekordbox(service)) return;  // Wave's browser stays until Rekordbox's is up in its place
    if (pages_->currentWidget() == browser_) toggleBrowser();
    pendingStreaming_ = service;
    pendingSince_ = now();
    if (!host::rekordboxPid()) session_->connectRekordbox();
    showBanner(QStringLiteral("Opening %1 in Rekordbox…").arg(service), "info", QStringLiteral("CANCEL"),
               [this] { pendingStreaming_.clear(); hideBanner("streaming"); }, 0, "streaming");
}

void MainWindow::leaveRekordbox() {
    // Put Rekordbox's browser away and come back to the waveforms.
    const auto browsing = rekordboxBrowsing_;
    rekordboxBrowsing_.reset();
    syncNav();
    if (!browsing) return;
    hideBanner("streaming");
    if (!browsing->service.isEmpty()) setOnTop(session_->settings.flag("always_on_top"));
    if (browsing->kind == "mac") session_->showRekordboxBrowser(false, 0, 0);
    else if (browsing->kind == "window") {
        if (browsing->embedded) host::unembed(*browsing->embedded);  // its own frame back, before it goes away
        host::putBack(browsing->hwnd, !browsing->main || session_->settings.flag("hide_rekordbox"));
    }
    else {
        host::hide();
        setWindowState(browsing->state & ~Qt::WindowMinimized);
        show();
    }
    raise();
    activateWindow();
    paceFrames();
}

void MainWindow::showRekordbox() {
    // Rekordbox itself, for its preferences and so on: Wave steps out of the way until it is used again.
    setOnTop(false);
    const QString why = host::show();
    if (!why.isEmpty()) {
        setOnTop(session_->settings.flag("always_on_top"));
        QMessageBox::information(this, QStringLiteral("Rekordbox"), why);
        return;
    }
#ifdef Q_OS_WIN
    showMinimized();
#endif
}

void MainWindow::changeEvent(QEvent *e) {
    // Back in Wave after Rekordbox was shown: on top again, unless Wave is stepping aside for a question.
    if (e->type() == QEvent::ActivationChange && isActiveWindow() && aside_.isEmpty() && !rekordboxBrowsing_)
        setOnTop(session_->settings.flag("always_on_top"));
#ifdef Q_OS_WIN
    // A tap on Wave while a streaming service is up in Rekordbox: Rekordbox's browser stays in front of Wave.
    if (e->type() == QEvent::ActivationChange && isActiveWindow() && rekordboxBrowsing_ && !rekordboxBrowsing_->service.isEmpty() &&
        rekordboxBrowsing_->kind == "window") {
        const quintptr hwnd = rekordboxBrowsing_->hwnd;
        QTimer::singleShot(0, this, [this, hwnd] {
            if (!rekordboxBrowsing_ || rekordboxBrowsing_->hwnd != hwnd) return;
            if (rekordboxBrowsing_->embedded) host::place(hwnd, pageRect());  // owned by Wave: already above it
            else host::present(hwnd, pageRect(), std::nullopt, false, false);
        });
    }
#endif
    if (e->type() == QEvent::WindowStateChange) paceFrames();
    QMainWindow::changeEvent(e);
}

bool MainWindow::eventFilter(QObject *o, QEvent *e) {
    // A tap on the search box with Windows' keyboard chosen: Windows' own keyboard comes up.
    if (e->type() == QEvent::MouseButtonPress && session_->settings.text("keyboard") == QLatin1String("windows") &&
        o->inherits("QLineEdit") && static_cast<QWidget *>(o)->window() == this)
        QTimer::singleShot(300, this, [] { touch::showWindowsKeyboard(); });  // after Windows' own reaction to the tap
    return QMainWindow::eventFilter(o, e);
}

void MainWindow::connectRekordbox() {
    const bool running = host::rekordboxPid() != 0;
    const auto f = session_->lastFrame();
    const QString why = f && f->stale ? QStringLiteral("Restart Rekordbox to finish updating Wave? It still runs the bridge from the Wave you updated from.")
                                      : QStringLiteral("Restart Rekordbox so Wave can connect to it?");
    if (running && QMessageBox::question(this, QStringLiteral("Connect Rekordbox"),
                                         why + QStringLiteral("\n\nPlayback stops while it restarts. Rekordbox asks first if a deck is playing.")) != QMessageBox::Yes)
        return;
    session_->connectRekordbox();
    updateStatus();
}

// --- Settings changes --------------------------------------------------------------------------------------

void MainWindow::persist() { session_->save(); }

void MainWindow::setView(const QString &key, const QVariant &value) {
    session_->settings.setView(key, value);
    if (key == "zoom_beats") session_->endKnobZoom();  // a zoom picked in Settings takes over from the sampler knob
    if (key == "keep_awake") keepAwake(value.toBool());
    if (key == "tap_to_open") browser_->setOpenOn(value.toString());
    if (key == "keyboard") { browser_->setKeyboard(value.toString()); touch_.apply(); }
    if (key == "touch_keyboard_off") touch_.apply();
    if ((key == "keyboard" || key == "touch_keyboard_off") && settings_) settings_->refresh();
    if (key == "dim") static_cast<Dimmer *>(dimmer_)->refresh();
    if (key == "always_on_top") setOnTop(value.toBool());
    canvas_->update();
    persist();
}

void MainWindow::setCount(int count) {
    session_->settings.setDeckCount(count);
    count_->setValue(count);
    canvas_->update();
    persist();
    if (settings_) settings_->refresh();
}

void MainWindow::setDeckOrder(const QString &order) {
    if (!deckOrder(order, session_->settings.deckCount())) {
        showBanner(QStringLiteral("Deck order must use each number 1, 2, 3, 4 once"), "warn", {}, {}, 5000);
        return;
    }
    session_->settings.setDeckOrderText(order);
    canvas_->update();
    persist();
}

void MainWindow::setColoursChanged() {
    canvas_->invalidate();
    persist();
}

void MainWindow::setScale(double scale) {
    session_->settings.setView("settings_scale", scale);
    qApp->setStyleSheet(wave::styleSheet(scale));
    persist();
}

void MainWindow::zoomBy(double factor) {
    // Smooth zoom from a trackpad, wheel, controller or key: 4 to 512 beats across the lane.
    const double beats = std::clamp(session_->zoomBeats() * factor, 4.0, 512.0);
    session_->settings.setView("zoom_beats", beats);
    session_->endKnobZoom();  // a pinch or the keys take over from the knob until it moves again
    if (settings_) settings_->zoomChanged(beats);
    canvas_->update();
    saveTimer_.start();
}

void MainWindow::checkForUpdate(bool asked) {
    if (!asked && (session_->demo() || !session_->settings.flag("check_updates"))) return;
    checkAsked_ = checkAsked_ || asked;
    updater_.check();
}

void MainWindow::offerUpdate() {
    if (!update_) return;
    if (!updater_.canInstall()) {  // a development build: the release page has it
        QDesktopServices::openUrl(QUrl(update_->page));
        return;
    }
    for (int i = 0; i < 4; ++i)
        if (session_->playing(i)) {
            QMessageBox::information(this, QStringLiteral("Update Wave"), QStringLiteral("Stop every deck first: Wave restarts to update."));
            return;
        }
    if (QMessageBox::question(this, QStringLiteral("Update Wave"),
                              QStringLiteral("Update Rekordbox Wave to %1?\n\nWave downloads it from GitHub, checks it and restarts. "
                                             "Rekordbox keeps running and Wave reconnects to it.").arg(update_->version)) != QMessageBox::Yes)
        return;
    updateButton_->setEnabled(false);
    updateButton_->setText(QStringLiteral("UPDATING 0%"));
    updater_.install(*update_);
}

void MainWindow::installWave() {
    if (QMessageBox::question(this, QStringLiteral("Install Rekordbox Wave"),
                              QStringLiteral("Install Rekordbox Wave in Program Files, with shortcuts in the Start menu and on the desktop?\n\n"
                                             "Windows asks for permission. Your settings stay as they are, and Wave restarts from its new home.")) !=
        QMessageBox::Yes)
        return;
    const QString why = install::installHere();
    if (!why.isEmpty()) {
        QMessageBox::information(this, QStringLiteral("Install Rekordbox Wave"), QStringLiteral("Wave was not installed: %1").arg(why));
        return;
    }
    // The installed copy takes over: this one quits first, so it is the only Wave.
    updating_ = true;  // Rekordbox keeps running
    const QString target = install::target();
    QProcess::startDetached(target, {"--after-install"}, QFileInfo(target).absolutePath());
    close();
    QApplication::quit();
}

void MainWindow::installRekordbox(const QString &archive) {
    if (setup_.busy()) return;
    if (archive.isEmpty())
        for (int i = 0; i < 4; ++i)
            if (session_->playing(i)) {
                showBanner(QStringLiteral("Stop every deck first: installing Rekordbox restarts it."), "warn", {}, {}, 6000);
                return;
            }
    setup_.run(archive);
}

void MainWindow::openLog() {
    QDir().mkpath(logFolder());
    QDesktopServices::openUrl(QUrl::fromLocalFile(logFolder()));
}

void MainWindow::firstRun(bool installNow, bool setupPage) {
    // Wave without the Rekordbox it supports can't do its job: say so, and offer to fix it.
#if defined(Q_OS_WIN) || defined(Q_OS_MACOS)
    if (session_->demo() || host::supportedInstall()) return;
    qCInfo(lcWindow) << "Supported Rekordbox missing:" << host::summary();
    if (installNow) {
        settings_->openPage(QStringLiteral("REKORDBOX"));
        if (pages_->currentWidget() != settings_) toggleSettings();
        installRekordbox();
        return;
    }
    if (setupPage) {
        settings_->openPage(QStringLiteral("REKORDBOX"));
        if (pages_->currentWidget() != settings_) toggleSettings();
    }
    showBanner(host::summary(), "warn", QStringLiteral("INSTALL REKORDBOX %1").arg(host::supportedText()), [this] {
        settings_->openPage(QStringLiteral("REKORDBOX"));
        if (pages_->currentWidget() != settings_) toggleSettings();
        installRekordbox();
    }, 0, "rekordbox");
#else
    Q_UNUSED(installNow);
    Q_UNUSED(setupPage);
#endif
}

QString MainWindow::controllerSummary() const {
    if (!midi_) return QStringLiteral("Wave can't read the controller here.");
    const QStringList controllers = midi_->controllers();
    if (controllers.isEmpty()) return QStringLiteral("No controller with a Rekordbox map is connected yet. %1").arg(midi_->status()).trimmed();
    const auto controls = midi_->controls();
    static const std::pair<midi::Kind, const char *> names[] = {{midi::Kind::Browse, "the knob"}, {midi::Kind::Press, "its push"},
                                                              {midi::Kind::Back, "BACK"}, {midi::Kind::View, "VIEW"},
                                                              {midi::Kind::Load, "LOAD"}, {midi::Kind::SamplerZoom, "the sampler knob"}};
    QStringList found;
    for (const auto &[k, n] : names) if (controls.count(k)) found << QString::fromLatin1(n);
    const QString heard = controllerSeen_ ? QStringLiteral("and has heard from it") : QStringLiteral("and is waiting to hear from it");
#ifdef Q_OS_WIN
    if (midi_->throughRekordbox())
        return QStringLiteral("Wave reads %1 through Rekordbox %2: %3. %4").arg(controllers.join(", "), heard, found.join(", "),
            midi_->keeping() ? QStringLiteral("The browser controls move Wave's browser only, and load what Wave shows.")
                             : QStringLiteral("Rekordbox's browser has them now."));
    return QStringLiteral("%1 is connected. Wave hears it through Rekordbox once Rekordbox is running with Wave's bridge (the "
                          "status at the top says LIVE).").arg(controllers.join(", "));
#else
    return QStringLiteral("Wave reads %1 itself %2: %3.").arg(controllers.join(", "), heard, found.join(", "));
#endif
}

// --- The monitor ---------------------------------------------------------------------------------------------

void MainWindow::selectScreen(const QString &key) {
    session_->settings.setDisplayKey(key);
    // Selecting a physical monitor means filling it, not positioning a small window.
    session_->settings.setFillDisplay(!key.isEmpty());
    connectedScreen_ = nullptr;
    holdWindowed_ = false;
    if (!key.isEmpty()) ensureDisplay();
    else if (isFullScreen()) leaveFullscreen();
    persist();
    settings_->refresh();
}

void MainWindow::ensureDisplay() {
    // Fill the remembered monitor whenever it is connected; otherwise stay a window.
    const QString key = session_->settings.displayKey();
    if (key.isEmpty() || !session_->settings.fillDisplay()) return;
    QScreen *found = nullptr;
    for (QScreen *s : QGuiApplication::screens()) if (screenKey(s) == key) found = s;
    if (!found) {
        if (connectedScreen_) {
            leaveFullscreen();
            showBanner(QStringLiteral("Monitor unplugged · windowed until it is connected again"), "warn", {}, {}, 8000, "monitor");
        }
        return;
    }
    if (holdWindowed_) return;
    // macOS animates into full screen for about a second and may not report it meanwhile.
    if (found == connectedScreen_ && now() - boundAt_ < 3) return;
#ifdef Q_OS_WIN
    const bool resized = geometry() != found->geometry();  // the taskbar can take part of the monitor back
#else
    const bool resized = false;
#endif
    if (found != connectedScreen_ || !isFullScreen() || resized) bind(found);
}

void MainWindow::bind(QScreen *screen) {
    // The full screen, never the taskbar-excluding work area.
    if (!windowHandle()) winId();
    showNormal();
    windowHandle()->setScreen(screen);
    setGeometry(screen->geometry());
    showFullScreen();
#ifdef Q_OS_WIN
    HWND hwnd = reinterpret_cast<HWND>(winId());
    MONITORINFO info{};
    info.cbSize = sizeof info;
    if (GetMonitorInfoW(MonitorFromWindow(hwnd, MONITOR_DEFAULTTONEAREST), &info)) {
        const RECT r = info.rcMonitor;  // covers the panel; rcWork excludes the taskbar
        SetWindowPos(hwnd, session_->settings.flag("always_on_top") ? HWND_TOPMOST : HWND_NOTOPMOST, r.left, r.top, r.right - r.left,
                     r.bottom - r.top, SWP_NOACTIVATE | SWP_FRAMECHANGED);
    }
#endif
    connectedScreen_ = screen;
    boundAt_ = now();
}

void MainWindow::leaveFullscreen() {
    connectedScreen_ = nullptr;
    showNormal();
    const QRect area = (screen() ? screen() : QGuiApplication::primaryScreen())->availableGeometry();
    const int w = std::min(1280, area.width()), h = std::min(800, area.height());
    setGeometry(area.x() + (area.width() - w) / 2, area.y() + (area.height() - h) / 2, w, h);
}

void MainWindow::changeFullscreen(bool on) {
    session_->settings.setFillDisplay(on);
    holdWindowed_ = false;
    if (on) {
        // The monitor the window is on now becomes the one to return to.
        QScreen *s = screen() ? screen() : QGuiApplication::primaryScreen();
        session_->settings.setDisplayKey(screenKey(s));
        bind(s);
    } else {
        leaveFullscreen();
    }
    persist();
    settings_->refresh();
}

void MainWindow::toggleFullscreen() {
    if (isFullScreen()) {
        holdWindowed_ = true;  // Esc or F11 out of full screen holds a window until the monitor is plugged in again
        leaveFullscreen();
    } else {
        changeFullscreen(true);
    }
}

void MainWindow::closeEvent(QCloseEvent *e) {
    qCInfo(lcWindow) << "Wave is closing";
    if (rekordboxBrowsing_) leaveRekordbox();
    if (session_->host() && session_->settings.flag("quit_rekordbox") && !updating_) {
        bool playing = false;
        for (int i = 0; i < 4; ++i) playing = playing || session_->playing(i);
        session_->host()->quit(playing);
    }
    keepAwake(false);
    touch_.restore();  // Windows' touch keyboard as it was
    frameTimer_.stop();
    watchTimer_.stop();
    screenTimer_.stop();
    browser_->shutdown();
    if (popups_) popups_->stop();
    if (midi_) midi_->stop();
    persist();
    QMainWindow::closeEvent(e);
}

}  // namespace wave
