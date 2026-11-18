#include "ui/widgets.h"

#include <QHBoxLayout>
#include <QListView>
#include <QPainter>
#include <QStyle>

#include <cmath>

namespace wave {

QString styleSheet(double scale) {
    const int size = int(std::lround(22 * scale));
    auto px = [&](double f) { return QString::number(std::lround(size * f)); };
    return QStringLiteral(R"(
QMainWindow, QWidget { background:#000; color:#eeeeee; font-family:Arial; }
QToolTip { background:#1d2026; color:#e8eaed; border:1px solid #30353d; padding:6px 10px; border-radius:6px; }
QFrame#toolbar { background:#0e0f12; border-bottom:1px solid #1d2025; }
QLabel#connection { color:#d9dde2; font-size:13px; font-weight:600; letter-spacing:1px; background:transparent; padding:0 6px; }
QLabel#clock { color:#f2f4f6; font-size:19px; font-weight:600; background:transparent; padding:0 10px; }
QLabel#master { color:#ffa21a; font-size:14px; font-weight:700; background:transparent; padding:0 8px; letter-spacing:1px; }
QLabel#battery { color:#d9dde2; font-size:14px; font-weight:600; background:transparent; padding:0 6px; }
QPushButton { border:1px solid #2a2e35; border-radius:9px; background:#1a1d22; color:#e8eaed; padding:8px 18px;
              font-size:15px; font-weight:600; min-height:28px; }
QPushButton:hover { background:#22262c; }
QPushButton:pressed { background:#2c3138; }
QPushButton:disabled { color:#565c64; background:#141619; border-color:#1f2227; }
QPushButton:focus, QComboBox:focus, QDoubleSpinBox:focus { border:1px solid #1a79eb; }
QPushButton[selected="true"] { color:#ffffff; background:#1a79eb; border-color:#1a79eb; }
QPushButton#connect { color:#000; background:#ff9f1a; border-color:#ff9f1a; }
QPushButton#install { color:#ffffff; background:#1a79eb; border-color:#1a79eb; }
QFrame#segment { background:#1a1d22; border:1px solid #2a2e35; border-radius:11px; }
QFrame#segment QPushButton { border:0; border-radius:8px; background:transparent; padding:6px 16px; color:#a7afb8; }
QFrame#segment QPushButton[selected="true"] { background:#1a79eb; color:#ffffff; }
QScrollArea { border:0; }
QScrollBar:vertical { background:transparent; width:10px; }
QScrollBar::handle:vertical { background:#3a3f46; min-height:40px; border-radius:5px; }
QScrollBar::add-line:vertical, QScrollBar::sub-line:vertical { height:0; }
QScrollBar::add-page:vertical, QScrollBar::sub-page:vertical { background:transparent; }
QFrame#banner { background:#14304f; border-bottom:1px solid #1d4a7a; }
QFrame#banner[tone="warn"] { background:#4a3108; border-bottom:1px solid #8a5a0c; }
QFrame#banner QLabel#bannerText { color:#f2f4f6; font-size:16px; font-weight:600; background:transparent; }
QFrame#banner QPushButton { padding:4px 14px; min-height:24px; font-size:14px; }
QFrame#banner QPushButton#bannerClose { background:transparent; border:0; color:#c9ced4; font-size:18px; padding:2px 8px; }
QFrame#banner QPushButton#bannerAction { color:#000; background:#ff9f1a; border-color:#ff9f1a; }
QWidget#settings { background:#0b0c0e; }
QWidget#settings QLabel, QWidget#settings QScrollArea, QWidget#settingsBody, QWidget#settings QStackedWidget,
QWidget#settings QScrollArea > QWidget > QWidget { background:transparent; }
QWidget#settings QFrame#card { background:#14161a; border:1px solid #1f2328; border-radius:16px; }
QWidget#settings QFrame#row, QWidget#settings QWidget#buttons { background:transparent; }
QWidget#settings QFrame#rule { background:#1f2328; }
QWidget#settings QLabel#rowTitle { color:#f2f4f6; font-weight:600; }
QWidget#settings QLabel#cardTitle { color:#6f7882; font-weight:700; letter-spacing:2px; padding:4px 0 6px 0; }
QWidget#settings QLabel#aboutTitle { color:#ffffff; font-weight:700; padding:8px 0; }
QWidget#settings QPushButton#primary { color:#ffffff; background:#1a79eb; border-color:#1a79eb; }
QWidget#settings QPushButton#primary:hover { background:#2b87f3; }
QListWidget#settingsMenu { background:transparent; border:0; outline:0; }
QListWidget#settingsMenu::item { color:#9aa3ad; padding:16px 20px; margin:3px 0; border-radius:10px; font-weight:600; }
QListWidget#settingsMenu::item:hover { background:#181b20; color:#e8eaed; }
QListWidget#settingsMenu::item:selected { background:#1a79eb; color:#ffffff; }
QComboBox { background:#1d2026; color:#e8eaed; border:1px solid #30353d; border-radius:10px; padding:8px 44px 8px 16px; min-height:30px; }
QComboBox:hover { border-color:#4a525c; }
QComboBox:on { border-color:#1a79eb; }
QComboBox::drop-down { subcontrol-origin:padding; subcontrol-position:center right; width:40px; border:0; }
QComboBox::down-arrow { image:url(:/ui/chevron.png); width:18px; height:18px; }
QComboBox QLineEdit { background:transparent; border:0; padding:0; }
QComboBox QAbstractItemView { background:#1d2026; border:1px solid #30353d; border-radius:10px; padding:6px; outline:0;
                              color:#e8eaed; selection-background-color:#1a79eb; }
QComboBox QAbstractItemView::item { min-height:46px; padding:0 14px; border-radius:8px; }
QComboBox QAbstractItemView::item:hover { background:#262a31; }
QComboBox QAbstractItemView::item:selected { background:#1a79eb; color:#ffffff; }
QDoubleSpinBox { background:#1d2026; color:#e8eaed; border:1px solid #30353d; border-radius:10px; padding:8px 12px; }
QDoubleSpinBox::up-button, QDoubleSpinBox::down-button { width:28px; border:0; }
QProgressBar { background:#1d2026; border:0; border-radius:3px; }
QProgressBar::chunk { background:#1a79eb; border-radius:3px; }
QWidget#settings QFrame#segment { background:#1d2026; border:1px solid #30353d; border-radius:12px; }
QWidget#settings QFrame#segment QPushButton { padding:8px 16px; border-radius:9px; min-height:24px; }
QWidget#settings, QWidget#settings * { font-size:%1px; }
QWidget#settings QLabel#rowHelp { font-size:%2px; color:#8d96a0; }
QWidget#settings QLabel#cardTitle { font-size:%3px; }
QWidget#settings QLabel#pageHeading { font-size:%4px; font-weight:700; color:#ffffff; padding:4px 0 2px 2px; }
QWidget#settings QLabel#aboutTitle { font-size:%5px; }
QWidget#settings QPushButton, QWidget#settings QFrame#segment QPushButton { min-height:30px; font-size:%6px; }
QWidget#settings QLabel#pageTitle { font-size:%7px; font-weight:700; color:#ffffff; }
QWidget#browser { background:#0a0a0a; }
QWidget#browser QFrame#browserNav { background:#0d0d0d; border-right:1px solid #000; }
QWidget#browser QFrame#browserTop { background:#141414; border-bottom:1px solid #1d1d1d; }
QWidget#browser QLabel#heading { color:#b8b8b8; font-size:22px; background:transparent; }
QWidget#browser QWidget#crumbs { background:transparent; }
QWidget#browser QPushButton#crumb { background:transparent; border:0; border-radius:6px; color:#8fb8ff; font-size:19px;
                                    font-weight:600; padding:6px 8px; min-height:30px; }
QWidget#browser QPushButton#crumb:pressed { background:#1d2a3b; }
QWidget#browser QLabel#crumbArrow { color:#5b636d; font-size:22px; background:transparent; padding:0 2px; }
QWidget#browser QLabel#crumbHere { color:#f2f4f6; font-size:19px; font-weight:700; background:transparent; padding:0 6px; }
QWidget#browser QPushButton#back { font-size:34px; padding:0 0 6px 0; border:1px solid #2c2c2c; border-radius:4px; background:#1b1b1b; }
QWidget#browser QPushButton#back:disabled { color:#3a3a3a; border-color:#1d1d1d; background:#141414; }
QWidget#browser QPushButton#keys { font-size:17px; font-weight:600; padding:0; border:1px solid #2c2c2c; border-radius:4px; background:#1b1b1b; }
QWidget#browser QLineEdit { background:#1f1f1f; color:#fff; border:1px solid #333; border-radius:4px; padding:0 14px; font-size:21px;
                            selection-background-color:#1a79eb; }
QWidget#browser QLineEdit:focus { border:1px solid #1a79eb; }
QWidget#browser QTableView, QWidget#browser QListView { background:#0a0a0a; border:0; outline:0; }
QWidget#browser QHeaderView::section { background:#141414; color:#8a8a8a; border:0; border-bottom:1px solid #262626; padding:6px;
                                       font-size:14px; font-weight:600; }
QWidget#browser QLabel#message { background:#0d0d0d; color:#9aa3ad; font-size:15px; padding:6px 16px; border-top:1px solid #1d1d1d; }
QWidget#browser QFrame#loadbar { background:#141414; border-top:1px solid #262626; }
QWidget#browser QWidget#sourcesPage { background:#0a0a0a; }
QWidget#browser QWidget#keyboard { background:#111; border-top:1px solid #262626; }
QWidget#browser QWidget#keyboard QPushButton { background:#222; border:1px solid #333; border-radius:4px; font-size:19px; }
QWidget#browser QWidget#keyboard QPushButton:pressed { background:#1a79eb; }
QWidget#browser QScrollBar:vertical { width:10px; background:#0a0a0a; }
QWidget#browser QScrollBar::handle:vertical { background:#3a3a3a; min-height:40px; border-radius:5px; }
)").arg(px(1), px(.72), px(.6), px(1.25), px(1.3), px(.74), px(1.45));
}

QPushButton *button(const QString &title, QWidget *parent) {
    auto *b = new QPushButton(title, parent);
    b->setCursor(Qt::PointingHandCursor);
    return b;
}

void mark(QWidget *w, const char *property, const QVariant &value) {
    if (w->property(property) == value) return;
    w->setProperty(property, value);
    w->style()->unpolish(w);
    w->style()->polish(w);
    w->update();
}

Switch::Switch(bool checked, QWidget *parent) : QAbstractButton(parent), knob_(checked ? 1 : 0) {
    setCheckable(true);
    setChecked(checked);
    setCursor(Qt::PointingHandCursor);
    setFocusPolicy(Qt::StrongFocus);
    slide_.setDuration(140);
    slide_.setEasingCurve(QEasingCurve::OutCubic);
    connect(&slide_, &QVariantAnimation::valueChanged, this, [this](const QVariant &v) { knob_ = v.toDouble(); update(); });
    connect(this, &QAbstractButton::toggled, this, [this](bool on) {
        slide_.stop();
        slide_.setStartValue(knob_);
        slide_.setEndValue(on ? 1.0 : 0.0);
        slide_.start();
    });
}

void Switch::setQuietly(bool on) {
    const bool blocked = blockSignals(true);
    setChecked(on);
    blockSignals(blocked);
    slide_.stop();
    knob_ = on ? 1 : 0;
    update();
}

void Switch::paintEvent(QPaintEvent *) {
    QPainter p(this);
    p.setRenderHint(QPainter::Antialiasing);
    const QRectF track(1, 1, width() - 2, height() - 2);
    const QColor off("#3a3f46"), on(isEnabled() ? kAccent : "#2a4466");
    const QColor mix(int(off.red() + (on.red() - off.red()) * knob_), int(off.green() + (on.green() - off.green()) * knob_),
                     int(off.blue() + (on.blue() - off.blue()) * knob_));
    p.setPen(hasFocus() ? QPen(QColor("#7fb5ff"), 2) : Qt::NoPen);
    p.setBrush(mix);
    p.drawRoundedRect(track, track.height() / 2, track.height() / 2);
    const double d = track.height() - 6, x = track.left() + 3 + (track.width() - d - 6) * knob_;
    p.setPen(Qt::NoPen);
    p.setBrush(QColor(0, 0, 0, 60));
    p.drawEllipse(QRectF(x, track.top() + 4, d, d));  // a soft shadow under the knob
    p.setBrush(isEnabled() ? Qt::white : QColor("#9aa3ad"));
    p.drawEllipse(QRectF(x, track.top() + 3, d, d));
}

Segmented::Segmented(const std::vector<std::pair<QString, QVariant>> &options, const QVariant &value, QWidget *parent) : QFrame(parent) {
    setObjectName("segment");
    auto *row = new QHBoxLayout(this);
    row->setContentsMargins(3, 3, 3, 3);
    row->setSpacing(2);
    for (const auto &[text, data] : options) {
        auto *b = button(text);
        b->setCheckable(true);
        connect(b, &QPushButton::clicked, this, [this, data = data] { setValue(data, true); });
        row->addWidget(b);
        buttons_.emplace_back(b, data);
    }
    setValue(value);
}

QVariant Segmented::value() const {
    for (const auto &[b, data] : buttons_) if (b->isChecked()) return data;
    return {};
}

void Segmented::setValue(const QVariant &value, bool notify) {
    // Numbers pick the nearest option, for values set before these choices existed.
    auto distance = [&](const QVariant &data) {
        const bool numbers = value.typeId() != QMetaType::Bool && data.typeId() != QMetaType::Bool && value.canConvert<double>() &&
                             data.canConvert<double>() && value.typeId() != QMetaType::QString && data.typeId() != QMetaType::QString;
        return numbers ? std::abs(data.toDouble() - value.toDouble()) : (data == value ? 0.0 : 1.0);
    };
    size_t best = 0;
    for (size_t i = 1; i < buttons_.size(); ++i) if (distance(buttons_[i].second) < distance(buttons_[best].second)) best = i;
    for (size_t i = 0; i < buttons_.size(); ++i) {
        buttons_[i].first->setChecked(i == best);
        mark(buttons_[i].first, "selected", i == best);
    }
    if (notify && !buttons_.empty()) emit changed(buttons_[best].second);
}

QComboBox *combo(const std::vector<std::pair<QString, QVariant>> &options, const QVariant &value, const QString &accessible) {
    auto *box = new QComboBox;
    auto *view = new QListView;
    view->setVerticalScrollMode(QAbstractItemView::ScrollPerPixel);
    box->setView(view);
    box->setCursor(Qt::PointingHandCursor);
    box->setAccessibleName(accessible);
    box->setMinimumWidth(320);
    box->setSizeAdjustPolicy(QComboBox::AdjustToMinimumContentsLengthWithIcon);
    for (const auto &[text, data] : options) box->addItem(text, data);
    if (value.isValid()) box->setCurrentIndex(std::max(0, box->findData(value)));
    return box;
}

}  // namespace wave
