#include "ui/settings_page.h"

#include "app/gig.h"
#include "app/install.h"
#include "app/session.h"
#include "app/updater.h"
#include "host/rekordbox.h"
#include "host/setup.h"
#include "ui/canvas.h"
#include "ui/mainwindow.h"
#include "ui/widgets.h"

#include <QColorDialog>
#include <QDesktopServices>
#include <QDoubleSpinBox>
#include <QFileDialog>
#include <QGuiApplication>
#include <QHBoxLayout>
#include <QJsonDocument>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QMessageBox>
#include <QProgressBar>
#include <QScreen>
#include <QScrollArea>
#include <QStackedWidget>
#include <QUrl>
#include <QVBoxLayout>

#include <cmath>

namespace wave {
namespace {

const char *kRepo = "https://github.com/palermostest25/rkbx_wave_den";

// One settings page: cards of rows.
class Page : public QScrollArea {
public:
    explicit Page(const QString &title) {
        setWidgetResizable(true);
        setFrameShape(QFrame::NoFrame);
        auto *body = new QWidget;
        body->setObjectName("settingsBody");
        column_ = new QVBoxLayout(body);
        column_->setContentsMargins(4, 4, 18, 24);
        column_->setSpacing(16);
        auto *heading = new QLabel(title);
        heading->setObjectName("pageHeading");
        column_->addWidget(heading);
        column_->addStretch();
        setWidget(body);
    }
    QVBoxLayout *card(const QString &title = {}) {
        auto *card = new QFrame;
        card->setObjectName("card");
        card_ = new QVBoxLayout(card);
        card_->setContentsMargins(22, 14, 22, 14);
        card_->setSpacing(0);
        if (!title.isEmpty()) {
            auto *label = new QLabel(title.toUpper());
            label->setObjectName("cardTitle");
            card_->addWidget(label);
        }
        column_->insertWidget(column_->count() - 1, card);
        rows_ = 0;
        return card_;
    }
    // Title and help at the left, the control at the right. Tapping the words flips a switch.
    QLabel *row(const QString &title, QWidget *control = nullptr, const QString &help = {}) {
        auto *row = new QFrame;
        row->setObjectName("row");
        auto *line = new QHBoxLayout(row);
        line->setContentsMargins(0, 12, 0, 12);
        line->setSpacing(24);
        auto *words = new QVBoxLayout;
        words->setSpacing(3);
        auto *name = new QLabel(title);
        name->setObjectName("rowTitle");
        name->setWordWrap(true);
        words->addWidget(name);
        auto *text = new QLabel(help);
        text->setObjectName("rowHelp");
        text->setWordWrap(true);
        text->setVisible(!help.isEmpty());
        words->addWidget(text);
        line->addLayout(words, 1);
        if (control) {
            line->addWidget(control, 0, Qt::AlignVCenter | Qt::AlignRight);
            if (auto *sw = qobject_cast<Switch *>(control)) {
                sw->setAccessibleName(title);
                name->installEventFilter(new Toggler(sw, name));
            }
        }
        if (rows_++) {
            auto *rule = new QFrame;
            rule->setObjectName("rule");
            rule->setFixedHeight(1);
            card_->addWidget(rule);
        }
        card_->addWidget(row);
        return text;
    }
    QVBoxLayout *current() const { return card_; }

private:
    struct Toggler : QObject {
        Toggler(Switch *sw, QObject *parent) : QObject(parent), sw(sw) {}
        bool eventFilter(QObject *, QEvent *e) override {
            if (e->type() == QEvent::MouseButtonRelease) sw->toggle();
            return false;
        }
        Switch *sw;
    };
    QVBoxLayout *column_, *card_ = nullptr;
    int rows_ = 0;
};

QWidget *buttons(std::initializer_list<QWidget *> items) {
    auto *holder = new QWidget;
    holder->setObjectName("buttons");
    auto *line = new QHBoxLayout(holder);
    line->setContentsMargins(0, 0, 0, 0);
    line->setSpacing(10);
    for (QWidget *w : items) line->addWidget(w);
    return holder;
}

QPushButton *action(const QString &title, std::function<void()> f) {
    QPushButton *b = button(title);
    QObject::connect(b, &QPushButton::clicked, b, f);
    return b;
}

}  // namespace

SettingsPage::SettingsPage(MainWindow *window) : window_(window) {
    setObjectName("settings");
    setAttribute(Qt::WA_StyledBackground);
    build();
}

void SettingsPage::build() {
    Session *session = window_->session();
    Settings &s = session->settings;
    auto *column = new QVBoxLayout(this);
    column->setContentsMargins(30, 20, 30, 20);
    column->setSpacing(16);
    auto *top = new QHBoxLayout;
    auto *heading = new QLabel(QStringLiteral("Settings"));
    heading->setObjectName("pageTitle");
    top->addWidget(heading);
    top->addStretch();
    QPushButton *done = action(QStringLiteral("DONE"), [this] { window_->toggleSettings(); });
    done->setObjectName("primary");
    top->addWidget(done);
    column->addLayout(top);
    auto *body = new QHBoxLayout;
    body->setSpacing(22);
    menu_ = new QListWidget;
    menu_->setObjectName("settingsMenu");
    menu_->setAccessibleName(QStringLiteral("Settings sections"));
    menu_->setFixedWidth(240);
    menu_->setVerticalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    stack_ = new QStackedWidget;
    stack_->setObjectName("settingsPages");
    connect(menu_, &QListWidget::currentRowChanged, stack_, &QStackedWidget::setCurrentIndex);
    body->addWidget(menu_);
    body->addWidget(stack_, 1);
    column->addLayout(body, 1);

    auto page = [this](const QString &name, const QString &title) {
        auto *p = new Page(title);
        stack_->addWidget(p);
        menu_->addItem(name);
        return p;
    };
    auto sw = [this, &s](const QString &key, std::function<void(bool)> changed = {}) {
        auto *control = new Switch(s.flag(key));
        connect(control, &Switch::toggled, this, changed ? changed : std::function<void(bool)>([this, key](bool v) { window_->setView(key, v); }));
        refreshers_.push_back([control, &s, key] { control->setQuietly(s.flag(key)); });
        return control;
    };
    auto seg = [this, &s](const QString &key, const std::vector<std::pair<QString, QVariant>> &options) {
        auto *control = new Segmented(options, s.view(key));
        connect(control, &Segmented::changed, this, [this, key](const QVariant &v) { window_->setView(key, v); });
        refreshers_.push_back([control, &s, key] { control->setValue(s.view(key)); });
        return control;
    };

    // DISPLAY ------------------------------------------------------------------------------------------
    Page *p = page(QStringLiteral("DISPLAY"), QStringLiteral("Display"));
    p->card(QStringLiteral("Screen"));
    screens_ = combo({}, {}, QStringLiteral("Target monitor"));
    connect(screens_, &QComboBox::activated, this, [this](int i) { window_->selectScreen(screens_->itemData(i).toString()); });
    p->row(QStringLiteral("Monitor"), screens_, QStringLiteral("Wave fills this screen whenever it is connected."));
    fullscreen_ = new Switch(s.fillDisplay());
    connect(fullscreen_, &Switch::toggled, this, [this](bool on) { window_->changeFullscreen(on); });
    p->row(QStringLiteral("Full screen"), fullscreen_, QStringLiteral("Cover the whole monitor, taskbar included."));
    p->row(QStringLiteral("Keep Wave on top"), sw("always_on_top"), QStringLiteral("Other windows never cover the decks."));
    p->row(QStringLiteral("Booth dimmer"), seg("dim", {{"100%", 1.0}, {"80%", .8}, {"60%", .6}, {"40%", .4}}),
           QStringLiteral("Darken the whole screen in a dark booth."));
    p->card(QStringLiteral("Keyboard"));
    if (touch::available()) {
        p->row(QStringLiteral("Typing in the browser"),
               seg("keyboard", {{QStringLiteral("Wave's keyboard"), "wave"}, {QStringLiteral("Windows touch keyboard"), "windows"}}),
               QStringLiteral("Wave's own keys under the search box, or Windows' touch keyboard, which comes up when you tap a text box."));
        touchMode_ = new Segmented({{QStringLiteral("Never"), touch::kNever}, {QStringLiteral("When no keyboard is attached"), touch::kNoKeyboard}},
                                   touch::mode());
        connect(touchMode_, &Segmented::changed, this, [this](const QVariant &v) { window_->touchGuard().choose(v.toInt()); });
        p->row(QStringLiteral("Windows shows its touch keyboard"), touchMode_, QStringLiteral("Windows' own setting, for typing in every app."));
        p->row(QStringLiteral("Turn Windows' keyboard off while Wave runs"), sw("touch_keyboard_off"),
               QStringLiteral("With Wave's keyboard chosen: off when Wave starts, back as it was when Wave closes."));
    } else {
        p->row(QStringLiteral("Typing in the browser"), nullptr, QStringLiteral("Wave's own keys come up under the search box: tap ABC."));
    }
    p->card(QStringLiteral("Decks"));
    count_ = new Segmented({{QStringLiteral("2 decks"), 2}, {QStringLiteral("4 decks"), 4}}, s.deckCount());
    connect(count_, &Segmented::changed, this, [this](const QVariant &v) { window_->setCount(v.toInt()); });
    p->row(QStringLiteral("Layout"), count_, QStringLiteral("Two or four stacked waveforms."));
    order_ = combo({{QStringLiteral("FLX10 · 3, 1, 2, 4"), "3,1,2,4"}, {QStringLiteral("Numeric · 1, 2, 3, 4"), "1,2,3,4"},
                    {QStringLiteral("Paired · 1, 3, 2, 4"), "1,3,2,4"}, {QStringLiteral("Reverse · 4, 3, 2, 1"), "4,3,2,1"}},
                   s.deckOrderText(), QStringLiteral("Deck order"));
    order_->setEditable(true);
    if (order_->findData(s.deckOrderText()) < 0) order_->setEditText(s.deckOrderText());
    connect(order_, &QComboBox::activated, this, [this](int i) { window_->setDeckOrder(order_->itemData(i).toString()); });
    connect(order_->lineEdit(), &QLineEdit::editingFinished, this, [this] {
        const int i = order_->findText(order_->currentText());
        window_->setDeckOrder(i >= 0 ? order_->itemData(i).toString() : order_->currentText().remove(' '));
    });
    p->row(QStringLiteral("Deck order"), order_, QStringLiteral("Top to bottom. Type your own, such as 2,1,4,3."));
    p->card(QStringLiteral("Text"));
    auto *scale = new Segmented({{QStringLiteral("Standard"), 1.0}, {QStringLiteral("Large"), 1.25}, {QStringLiteral("Larger"), 1.5},
                                 {QStringLiteral("Largest"), 2.0}}, s.number("settings_scale"));
    connect(scale, &Segmented::changed, this, [this](const QVariant &v) { window_->setScale(v.toDouble()); });
    p->row(QStringLiteral("Settings text"), scale, QStringLiteral("The size of everything on this page."));
    auto *ui = new Segmented({{QStringLiteral("Small"), .85}, {QStringLiteral("Standard"), 1.0}, {QStringLiteral("Large"), 1.25},
                              {QStringLiteral("Larger"), 1.5}}, s.uiScale());
    connect(ui, &Segmented::changed, this, [this, &s](const QVariant &v) { s.setUiScale(v.toDouble()); window_->setColoursChanged(); });
    p->row(QStringLiteral("Deck text"), ui, QStringLiteral("Cue flags, bar numbers and deck numbers on the waveforms."));

    // WAVEFORM -----------------------------------------------------------------------------------------
    p = page(QStringLiteral("WAVEFORM"), QStringLiteral("Waveform"));
    p->card(QStringLiteral("View"));
    std::vector<std::pair<QString, QVariant>> zooms;
    for (int b : {4, 8, 16, 32, 64, 128, 256, 512})
        zooms.emplace_back(QStringLiteral("%1 %2 · %3 beats").arg(b / 4).arg(b == 4 ? "bar" : "bars").arg(b), b);
    zoom_ = combo(zooms, {}, QStringLiteral("Waveform zoom"));
    zoomChanged(s.number("zoom_beats"));
    connect(zoom_, &QComboBox::activated, this, [this](int i) { window_->setView("zoom_beats", zoom_->itemData(i).toDouble()); });
    p->row(QStringLiteral("Zoom"), zoom_, QStringLiteral("Also: pinch, scroll, or + and − on the keyboard."));
    p->row(QStringLiteral("Sampler knob zooms"), sw("sampler_zoom"),
           QStringLiteral("The controller's sampler volume knob: its middle is the zoom above, its ends 1 bar and 32 bars. "
                          "Rekordbox still hears it too, unless you unmap it in Rekordbox."));
    p->row(QStringLiteral("Sampler knob direction"),
           seg("sampler_zoom_reverse", {{QStringLiteral("Clockwise zooms in"), false}, {QStringLiteral("Clockwise zooms out"), true}}));
    p->row(QStringLiteral("Playhead"), seg("playhead", {{QStringLiteral("AZ · 32%"), .32}, {QStringLiteral("Centre"), .5}, {QStringLiteral("Left · 20%"), .2}}),
           QStringLiteral("Where the playing point sits across the lane."));
    auto *style = new Segmented({{QStringLiteral("Rekordbox"), "Rekordbox"}, {QStringLiteral("Wave classic"), "Wave classic"}}, s.text("style"));
    connect(style, &Segmented::changed, this, [this, &s](const QVariant &v) { s.setStyle(v.toString()); window_->setColoursChanged(); });
    p->row(QStringLiteral("Colours"), style, QStringLiteral("Rekordbox's own three-band colours, or Wave's brighter set."));
    auto pick = [this, &s](int band, const QString &name) {
        return action(name.toUpper(), [this, &s, band, name] {
            const QColor c = QColorDialog::getColor(s.colours()[size_t(band)], this, QStringLiteral("%1 waveform band").arg(name));
            if (c.isValid()) { s.setColour(band, c); window_->setColoursChanged(); }
        });
    };
    p->row(QStringLiteral("Band colours"), buttons({pick(0, "Low"), pick(1, "Mid"), pick(2, "High")}), QStringLiteral("Pick each band yourself."));
    p->card(QStringLiteral("Marks"));
    p->row(QStringLiteral("Beat and bar markers"), sw("grid"), QStringLiteral("Red downbeats, white beats, outside the waveform."));
    p->row(QStringLiteral("Bar numbers"), sw("bar_numbers"), QStringLiteral("Every fourth bar, under the lane."));
    p->row(QStringLiteral("Hot cues and memory cues"), sw("cues"), QStringLiteral("Coloured flags, as Rekordbox has them."));
    p->row(QStringLiteral("Saved and live loops"), sw("loops"), QStringLiteral("Brackets for saved loops, shading while one plays."));
    p->card(QStringLiteral("Levels"));
    for (int band = 0; band < 3; ++band) {
        auto *spin = new QDoubleSpinBox;
        spin->setRange(.1, 3);
        spin->setSingleStep(.1);
        spin->setSuffix(QStringLiteral(" ×"));
        spin->setValue(s.gains()[size_t(band)]);
        spin->setMinimumWidth(160);
        connect(spin, &QDoubleSpinBox::valueChanged, this, [this, &s, band](double v) { s.setGain(band, v); window_->setColoursChanged(); });
        static const char *names[] = {"Low", "Mid", "High"};
        p->row(QStringLiteral("%1 band level").arg(names[band]), spin);
    }

    // DECK INFO ----------------------------------------------------------------------------------------
    p = page(QStringLiteral("DECK INFO"), QStringLiteral("Deck information"));
    p->card();
    p->row(QStringLiteral("Information bar"), sw("deck_info"), QStringLiteral("Track, time, BPM, key and the whole-track overview under the lanes."));
    p->row(QStringLiteral("Artwork"), sw("artwork"), QStringLiteral("The sleeve beside each deck."));
    time_ = seg("time_mode", {{QStringLiteral("Remaining"), "remain"}, {QStringLiteral("Elapsed"), "elapsed"}});
    p->row(QStringLiteral("Time"), time_, QStringLiteral("Tap the time on a deck to flip it, too."));
    p->row(QStringLiteral("Bar size"), seg("info_size", {{QStringLiteral("Compact"), .8}, {QStringLiteral("Standard"), 1.0}, {QStringLiteral("Large"), 1.25}}));
    p->row(QStringLiteral("End-of-track warning"), seg("end_warning", {{QStringLiteral("Off"), 0}, {QStringLiteral("15 s"), 15}, {QStringLiteral("30 s"), 30}, {QStringLiteral("60 s"), 60}}),
           QStringLiteral("The time turns red and the overview flashes."));
    p->row(QStringLiteral("Battery"), sw("battery"), QStringLiteral("Battery level in the top bar, and a warning when the charger comes out."));
    p->row(QStringLiteral("Master tempo in the top bar"), sw("master_bpm"), QStringLiteral("The tempo master's BPM, beside the clock."));

    // BROWSER ------------------------------------------------------------------------------------------
    p = page(QStringLiteral("BROWSER"), QStringLiteral("Browser"));
    p->card();
    browser_ = seg("browser", {{QStringLiteral("Wave's touch browser"), "wave"}, {QStringLiteral("Rekordbox's browser"), "rekordbox"}});
    p->row(QStringLiteral("BROWSE and VIEW open"), browser_,
           QStringLiteral("Wave's touch browser: your collection or a USB stick, made for fingers. Rekordbox's: its own browser, over Wave."));
    controller_ = p->row(QStringLiteral("Controller"), nullptr, QStringLiteral(" "));
    p->row(QStringLiteral("Open a playlist with"), seg("tap_to_open", {{QStringLiteral("Double tap"), "double"}, {QStringLiteral("Single tap"), "single"}}),
           QStringLiteral("Double tap lets a finger scroll the list and pick without opening anything by accident."));
    p->row(QStringLiteral("Beatport and Spotify in Wave's browser (beta)"), sw("native_streaming"),
           QStringLiteral("Search the service in Wave's own browser and load from it; Rekordbox does the searching. Off: "
                          "Rekordbox's own browser opens inside Wave instead."));
    p->row(QStringLiteral("The knob drives Wave's browser"), sw("knob_loads_wave"),
           QStringLiteral("The browse knob, its push, BACK and VIEW move Wave's browser and load the track picked here; "
                          "Rekordbox's browser doesn't move with them."));

    // REKORDBOX ----------------------------------------------------------------------------------------
    p = page(QStringLiteral("REKORDBOX"), QStringLiteral("Rekordbox"));
    p->card(QStringLiteral("Connection"));
    status_ = p->row(QStringLiteral("Status"), nullptr, session->status().isEmpty() ? QStringLiteral(" ") : session->status());
#if defined(Q_OS_WIN) || defined(Q_OS_MACOS)
    installed_ = p->row(QStringLiteral("Installed"), nullptr, host::summary());
    install_ = action(QStringLiteral("INSTALL REKORDBOX %1").arg(host::supportedText()), [this] { window_->installRekordbox(); });
    install_->setObjectName("primary");
    QPushButton *choose = action(QStringLiteral("USE A DOWNLOADED INSTALLER…"), [this] {
        const QString file = QFileDialog::getOpenFileName(this, QStringLiteral("Rekordbox %1 installer").arg(host::supportedText()), {},
                                                          QStringLiteral("Rekordbox installer (*.zip *.exe *.pkg)"));
        if (!file.isEmpty()) window_->installRekordbox(file);
    });
    QPushButton *pageButton = action(QStringLiteral("DOWNLOAD PAGE"), [] { QDesktopServices::openUrl(QUrl(QString::fromLatin1(host::kDownloadPage))); });
    installText_ = p->row(QStringLiteral("Install the supported Rekordbox"), buttons({install_, choose, pageButton}),
                          QStringLiteral("Wave gets AlphaTheta's installer from rekordbox.com and runs it after checking its signature."));
    progress_ = new QProgressBar;
    progress_->setTextVisible(false);
    progress_->setFixedHeight(6);
    progress_->hide();
    p->current()->addWidget(progress_);
    connect(window_, &MainWindow::installProgress, this, [this](double fraction, const QString &text) {
        if (!text.isEmpty()) installText_->setText(text);
        if (fraction > 1.5) { progress_->hide(); refresh(); return; }
        progress_->show();
        if (fraction < 0) progress_->setRange(0, 0);
        else { progress_->setRange(0, 1000); progress_->setValue(int(fraction * 1000)); }
    });
    p->card(QStringLiteral("Behind Wave"));
    p->row(QStringLiteral("Start Rekordbox with Wave"), sw("start_rekordbox"), QStringLiteral("Rekordbox starts, out of sight, when Wave does."));
    p->row(QStringLiteral("Connect by itself"), sw("auto_connect"),
           QStringLiteral("To a Rekordbox started without Wave. Windows connects live; macOS restarts it only while no deck has a track."));
    p->row(QStringLiteral("Start Rekordbox again if it closes"), sw("keep_rekordbox_running"), QStringLiteral("Up to three times in ten minutes."));
    p->row(QStringLiteral("Quit Rekordbox with Wave"), sw("quit_rekordbox"), QStringLiteral("Never while a deck is playing."));
    p->row(QStringLiteral("Keep Rekordbox hidden"), sw("hide_rekordbox"), QStringLiteral("Minimised behind Wave."));
    p->row(QStringLiteral("Close Rekordbox's notices and offers"), sw("close_popups"),
           QStringLiteral("Questions are always left for you, brought over Wave."));
    p->row(QStringLiteral("Rekordbox itself"), buttons({action(QStringLiteral("SHOW REKORDBOX"), [this] { window_->showRekordbox(); })}),
           QStringLiteral("For its preferences. Come back by tapping Wave."));
#else
    p->row(QStringLiteral("Rekordbox"), nullptr, QStringLiteral("Wave connects to Rekordbox on Windows and macOS."));
#endif

    // SYSTEM -------------------------------------------------------------------------------------------
    p = page(QStringLiteral("SYSTEM"), QStringLiteral("System"));
    if (install::offered()) {
        p->card(QStringLiteral("Install"));
        QPushButton *installWave = action(QStringLiteral("INSTALL WAVE"), [this] { window_->installWave(); });
        installWave->setObjectName("primary");
        p->row(QStringLiteral("Install Rekordbox Wave"), buttons({installWave}),
               QStringLiteral("This is the portable download. Install puts it in Program Files with Start menu and desktop shortcuts; "
                              "remove it from Windows' installed apps."));
    }
    p->card(QStringLiteral("While Wave runs"));
    p->row(QStringLiteral("Keep the screen awake"), sw("keep_awake"), QStringLiteral("No screen saver or sleep during a set."));
    p->card(QStringLiteral("Updates"));
    p->row(QStringLiteral("Check for updates"), sw("check_updates"), QStringLiteral("When Wave starts and every few hours. Never installs during playback."));
    p->row(QStringLiteral("Now"), buttons({action(QStringLiteral("CHECK FOR UPDATES"), [this] { window_->checkForUpdate(true); })}));
#ifdef Q_OS_MACOS
    p->card(QStringLiteral("Permissions"));
    p->row(QStringLiteral("Accessibility"), buttons({action(QStringLiteral("OPEN SETTINGS"), [] {
               QDesktopServices::openUrl(QUrl("x-apple.systempreferences:com.apple.preference.security?Privacy_Accessibility"));
           })}), QStringLiteral("Lets Wave read and close Rekordbox's popups."));
#endif
    p->card(QStringLiteral("Profiles"));
    QPushButton *importButton = action(QStringLiteral("IMPORT…"), [this, session] {
        const QString file = QFileDialog::getOpenFileName(this, QStringLiteral("Import profile"), {}, QStringLiteral("JSON profiles (*.json)"));
        if (file.isEmpty()) return;
        Settings imported;
        QString error;
        if (!imported.load(file, &error)) { QMessageBox::warning(this, QStringLiteral("Profile could not be loaded"), error); return; }
        session->settings = imported;
        window_->persist();
        window_->setColoursChanged();
        window_->setScale(session->settings.number("settings_scale"));
        refresh();
    });
    QPushButton *exportButton = action(QStringLiteral("EXPORT…"), [this, session] {
        const QString file = QFileDialog::getSaveFileName(this, QStringLiteral("Export profile"), QStringLiteral("rekordbox-wave.json"),
                                                          QStringLiteral("JSON profiles (*.json)"));
        QString error;
        if (!file.isEmpty() && !session->settings.save(file, &error)) QMessageBox::warning(this, QStringLiteral("Profile could not be saved"), error);
    });
    p->row(QStringLiteral("Settings profiles"), buttons({importButton, exportButton}), QStringLiteral("Take your setup to another computer."));
    p->card(QStringLiteral("Problems"));
    p->row(QStringLiteral("Log"), buttons({action(QStringLiteral("OPEN LOG"), [this] { window_->openLog(); })}), QStringLiteral("What Wave did, for a bug report."));
    p->row(QStringLiteral("Report a problem"), buttons({action(QStringLiteral("OPEN GITHUB"), [] {
               QDesktopServices::openUrl(QUrl(QString::fromLatin1(kRepo) + "/issues/new/choose"));
           })}));
    p->row(QStringLiteral("Keys"), nullptr, QStringLiteral("F2 settings · F11 full screen · + / − zoom · Escape back"));

    // ABOUT --------------------------------------------------------------------------------------------
    p = page(QStringLiteral("ABOUT"), QStringLiteral("About"));
    QVBoxLayout *about = p->card();
    auto *title = new QLabel(QStringLiteral("Rekordbox Wave %1").arg(version()));
    title->setObjectName("aboutTitle");
    about->addWidget(title);
    auto *text = new QLabel(QStringLiteral("An XDJ-AZ style deck screen and touch browser for Rekordbox. Rekordbox stays the audio engine and "
                                           "keeps your controller; Wave is the screen. Native C++: light on the CPU, cool on a tablet."));
    text->setObjectName("rowHelp");
    text->setWordWrap(true);
    about->addWidget(text);
    p->row(QStringLiteral("Website"), buttons({action(QStringLiteral("GITHUB"), [] { QDesktopServices::openUrl(QUrl(QString::fromLatin1(kRepo))); })}));
    QVBoxLayout *credits = p->card(QStringLiteral("Credits and licences"));
    auto *words = new QLabel(QStringLiteral(
        "Based on rkbx_wave by mrmilbe (MIT). Qt under the LGPL v3; SQLCipher by Zetetic (BSD); database keys as pyrekordbox (MIT) "
        "publishes them; waveform formats from Deep Symmetry. Rekordbox, XDJ-AZ and DDJ-FLX10 are trademarks of AlphaTheta "
        "Corporation; Wave is not affiliated with or endorsed by AlphaTheta."));
    words->setObjectName("rowHelp");
    words->setWordWrap(true);
    credits->addWidget(words);

    menu_->setCurrentRow(0);
    refreshScreens();
}

void SettingsPage::refresh() {
    Session *session = window_->session();
    const Settings &s = session->settings;
    for (const auto &r : refreshers_) r();
    if (count_) count_->setValue(s.deckCount());
    if (fullscreen_) fullscreen_->setQuietly(s.fillDisplay());
    if (touchMode_) touchMode_->setValue(touch::mode());
    if (zoom_) zoomChanged(s.number("zoom_beats"));
    if (controller_) controller_->setText(window_->controllerSummary());
#if defined(Q_OS_WIN) || defined(Q_OS_MACOS)
    if (installed_) installed_->setText(host::summary());
    if (install_) {
        const bool have = host::supportedInstall().has_value();
        install_->setText(have ? QStringLiteral("REINSTALL") : QStringLiteral("INSTALL REKORDBOX %1").arg(host::supportedText()));
        install_->setObjectName(have ? QString() : QStringLiteral("primary"));
        install_->style()->unpolish(install_);
        install_->style()->polish(install_);
    }
#endif
    refreshScreens();
}

void SettingsPage::refreshScreens() {
    if (!screens_) return;
    const QString key = window_->session()->settings.displayKey();
    screens_->blockSignals(true);
    screens_->clear();
    screens_->addItem(QStringLiteral("Windowed · move freely"), QString());
    for (QScreen *screen : QGuiApplication::screens()) {
        const QRect r = screen->geometry();
        const double ratio = screen->devicePixelRatio();
        screens_->addItem(QStringLiteral("%1 · %2 × %3").arg(screen->name()).arg(std::lround(r.width() * ratio)).arg(std::lround(r.height() * ratio)),
                          screenKey(screen));
    }
    screens_->setCurrentIndex(std::max(0, screens_->findData(key)));
    screens_->blockSignals(false);
}

void SettingsPage::setStatus(const QString &text) {
    if (status_ && status_->text() != text) status_->setText(text.isEmpty() ? QStringLiteral(" ") : text);
}

void SettingsPage::zoomChanged(double beats) {
    if (!zoom_) return;
    int nearest = 0;
    for (int i = 1; i < zoom_->count(); ++i)
        if (std::abs(std::log2(zoom_->itemData(i).toDouble() / beats)) < std::abs(std::log2(zoom_->itemData(nearest).toDouble() / beats))) nearest = i;
    zoom_->blockSignals(true);
    zoom_->setCurrentIndex(nearest);
    zoom_->blockSignals(false);
}

void SettingsPage::openPage(const QString &name) {
    for (int row = 0; row < menu_->count(); ++row)
        if (menu_->item(row)->text() == name) menu_->setCurrentRow(row);
}

}  // namespace wave
