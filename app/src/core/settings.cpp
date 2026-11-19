#include "core/settings.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QSaveFile>
#include <QStandardPaths>

#include <algorithm>
#include <cmath>
#include <map>
#include <tuple>

namespace wave {
namespace {

// The Python app's view defaults (rb_waveform_core/qt_app.py), and the ones added since.
const std::map<QString, QVariant> &viewDefaults() {
    static const std::map<QString, QVariant> defaults = {
        {"zoom_beats", 32}, {"playhead", .32}, {"deck_info", true}, {"time_mode", "remain"}, {"info_size", 1.0},
        {"artwork", true}, {"end_warning", 30}, {"start_rekordbox", true}, {"hide_rekordbox", true},
        {"check_updates", true}, {"grid", true}, {"cues", true}, {"loops", true}, {"bar_numbers", true},
        {"source", "Native"}, {"settings_scale", 1.0}, {"always_on_top", true}, {"refresh_hz", 60},
        {"style", "Rekordbox"}, {"browser", "wave"}, {"knob_loads_wave", true}, {"native_streaming", true}, {"auto_connect", true},
        {"keep_rekordbox_running", true}, {"quit_rekordbox", true}, {"close_popups", true}, {"keep_awake", true},
        {"settings_version", Settings::kVersion}, {"tap_to_open", "double"}, {"sampler_zoom", true},
        {"sampler_zoom_reverse", false}, {"touch_keyboard_off", true}, {"touch_keyboard_restore", QVariant()},
        {"dim", 1.0}, {"battery", true}, {"master_bpm", true},
        {"keyboard", "wave"},  // new: Wave's own keyboard, or Windows' touch keyboard ("windows")
    };
    return defaults;
}

struct Style { const char *name; int low[3], mid[3], overlap[3], high[3]; };
// Rekordbox: flat-fill pixels measured from rekordbox 7's decks. Wave classic: the brighter first look.
constexpr Style kStyles[] = {
    {"Rekordbox", {0, 84, 226}, {255, 166, 0}, {180, 102, 7}, {246, 236, 217}},
    {"Wave classic", {20, 95, 235}, {244, 164, 20}, {169, 107, 39}, {255, 255, 255}},
};

QJsonArray rgb(const int (&c)[3]) { return {c[0], c[1], c[2]}; }
QColor colourOf(const QJsonValue &value, const QColor &fallback) {
    const QJsonArray a = value.toArray();
    return a.size() == 3 ? QColor(a[0].toInt(), a[1].toInt(), a[2].toInt()) : fallback;
}
bool same(const QJsonValue &value, const int (&c)[3]) {
    const QJsonArray a = value.toArray();
    return a.size() == 3 && a[0].toInt() == c[0] && a[1].toInt() == c[1] && a[2].toInt() == c[2];
}

}  // namespace

QString settingsFolder() {
    const QString override = qEnvironmentVariable("RKBX_WAVE_CONFIG_DIR");
    if (!override.isEmpty()) return override;
#ifdef Q_OS_WIN
    return QDir(qEnvironmentVariable("APPDATA", QDir::homePath())).filePath("rkbx_wave");
#else
    return QDir::home().filePath("Library/Application Support/rkbx_wave");
#endif
}

QString settingsFile() { return QDir(settingsFolder()).filePath("default_config.json"); }

std::optional<std::vector<int>> deckOrder(const QString &text, int count) {
    std::vector<int> order;
    for (const QString &part : text.split(',', Qt::SkipEmptyParts)) {
        bool ok = false;
        const int deck = part.trimmed().toInt(&ok);
        if (!ok || deck < 1 || deck > 4) return std::nullopt;
        order.push_back(deck - 1);
    }
    std::vector<int> sorted = order;
    std::sort(sorted.begin(), sorted.end());
    if (sorted != std::vector<int>{0, 1, 2, 3} || (count != 2 && count != 4)) return std::nullopt;
    if (count == 2) {  // the first two decks of the order that are 1 or 2... as the Python app shows them
        std::vector<int> two;
        for (int deck : order) if (deck < 2) two.push_back(deck);
        return two;
    }
    return order;
}

Settings::Settings() {
    QJsonObject colour;
    colour["low_color"] = rgb(kStyles[0].low);
    colour["mid_color"] = rgb(kStyles[0].mid);
    colour["high_color"] = rgb(kStyles[0].high);
    colour["overlap_color"] = rgb(kStyles[0].overlap);
    root_["color"] = colour;
    QJsonObject render;
    render["deck_count"] = 4;
    render["deck_order"] = "3,1,2,4";
    render["display_key"] = "";
    render["fill_display"] = false;
    render["ui_scale"] = 1.0;
    render["low_gain"] = 1.0;
    render["mid_gain"] = 1.0;
    render["high_gain"] = 1.0;
    render["smoothing_bins"] = 1;
    root_["render"] = render;
    root_["view"] = QJsonObject();
}

bool Settings::load(const QString &path, QString *error) {
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) {
        if (error) *error = file.exists() ? file.errorString() : QString();
        return !file.exists();  // no file yet: the defaults, which is fine
    }
    QJsonParseError parse{};
    const QJsonDocument doc = QJsonDocument::fromJson(file.readAll(), &parse);
    auto fail = [&](const QString &why) {
        if (error) *error = QStringLiteral("Previous settings could not be loaded: %1").arg(why);
        return false;
    };
    if (parse.error != QJsonParseError::NoError || !doc.isObject()) return fail(parse.errorString());
    QJsonObject data = doc.object();
    for (const char *name : {"color", "render", "view"})
        if (data.contains(QLatin1String(name)) && !data.value(QLatin1String(name)).isObject())
            return fail(QStringLiteral("profile sections must be JSON objects"));
    // As the Python app validates: a profile that would show nothing sensible is refused whole.
    const QJsonObject view = data.value("view").toObject(), render = data.value("render").toObject();
    const int decks = render.value("deck_count").toInt(4);
    if (!deckOrder(render.value("deck_order").toString("3,1,2,4"), decks)) return fail(QStringLiteral("invalid deck order"));
    for (auto [key, lo, hi] : {std::tuple{"zoom_beats", 4.0, 512.0}, {"playhead", .15, .75}, {"settings_scale", 1.0, 2.0},
                               {"refresh_hz", 20.0, 120.0}, {"info_size", .5, 2.0}, {"end_warning", 0.0, 600.0}}) {
        if (!view.contains(QLatin1String(key))) continue;
        const double v = view.value(QLatin1String(key)).toDouble(std::nan(""));
        if (!std::isfinite(v) || v < lo || v > hi) return fail(QStringLiteral("invalid %1").arg(key));
    }
    // Start from the defaults and lay the file over them, keeping keys this version doesn't know.
    Settings defaults;
    for (const char *name : {"color", "render"}) {
        QJsonObject merged = defaults.section(name);
        const QJsonObject saved = data.value(QLatin1String(name)).toObject();
        for (auto it = saved.begin(); it != saved.end(); ++it) merged[it.key()] = it.value();
        data[QLatin1String(name)] = merged;
    }
    QJsonObject v = view;
    if (v.value("settings_version").toInt(1) < 2) v["browser"] = "rekordbox";  // as 2.5 moved everyone
    v["settings_version"] = kVersion;
    data["view"] = v;
    // Untouched older palettes move to the current Rekordbox look, as the Python app does.
    QJsonObject colour = data.value("color").toObject();
    const int first[4][3] = {{0, 80, 224}, {248, 160, 0}, {176, 96, 0}, {244, 234, 214}};
    const bool firstRekordbox = same(colour["low_color"], first[0]) && same(colour["mid_color"], first[1]) &&
                                same(colour["overlap_color"], first[2]) && same(colour["high_color"], first[3]);
    const bool classicUntouched = !view.contains("style") && same(colour["low_color"], kStyles[1].low) &&
                                  same(colour["mid_color"], kStyles[1].mid) && same(colour["high_color"], kStyles[1].high);
    root_ = data;
    if (firstRekordbox || classicUntouched) setStyle(QStringLiteral("Rekordbox"));
    return true;
}

bool Settings::save(const QString &path, QString *error) const {
    QDir().mkpath(QFileInfo(path).absolutePath());
    QSaveFile file(path);
    if (!file.open(QIODevice::WriteOnly)) {
        if (error) *error = file.errorString();
        return false;
    }
    file.write(QJsonDocument(root_).toJson(QJsonDocument::Indented));
    if (!file.commit()) {
        if (error) *error = file.errorString();
        return false;
    }
    return true;
}

QVariant Settings::view(const QString &key) const {
    const QJsonObject v = section("view");
    if (v.contains(key) && !v.value(key).isNull()) return v.value(key).toVariant();
    const auto &defaults = viewDefaults();
    const auto it = defaults.find(key);
    return it == defaults.end() ? QVariant() : it->second;
}

void Settings::setView(const QString &key, const QVariant &value) { setIn("view", key, QJsonValue::fromVariant(value)); }

void Settings::setIn(const char *name, const QString &key, const QJsonValue &value) {
    QJsonObject s = section(name);
    s[key] = value;
    root_[QLatin1String(name)] = s;
}

int Settings::deckCount() const { return section("render").value("deck_count").toInt(4) == 2 ? 2 : 4; }
void Settings::setDeckCount(int count) { setIn("render", "deck_count", count == 2 ? 2 : 4); }
QString Settings::deckOrderText() const { return section("render").value("deck_order").toString("3,1,2,4"); }
void Settings::setDeckOrderText(const QString &order) { setIn("render", "deck_order", order); }
std::vector<int> Settings::order() const {
    return deckOrder(deckOrderText(), deckCount()).value_or(deckCount() == 2 ? std::vector<int>{0, 1} : std::vector<int>{2, 0, 1, 3});
}
QString Settings::displayKey() const { return section("render").value("display_key").toString(); }
void Settings::setDisplayKey(const QString &key) { setIn("render", "display_key", key); }
bool Settings::fillDisplay() const { return section("render").value("fill_display").toBool(); }
void Settings::setFillDisplay(bool on) { setIn("render", "fill_display", on); }
double Settings::uiScale() const { return std::clamp(section("render").value("ui_scale").toDouble(1.0), .5, 2.0); }
void Settings::setUiScale(double scale) { setIn("render", "ui_scale", scale); }
std::array<float, 3> Settings::gains() const {
    const QJsonObject r = section("render");
    return {float(r.value("low_gain").toDouble(1)), float(r.value("mid_gain").toDouble(1)), float(r.value("high_gain").toDouble(1))};
}
void Settings::setGain(int band, double value) {
    static const char *keys[] = {"low_gain", "mid_gain", "high_gain"};
    setIn("render", keys[std::clamp(band, 0, 2)], value);
}
int Settings::smoothing() const { return std::max(1, section("render").value("smoothing_bins").toInt(1)); }

std::array<QColor, 4> Settings::colours() const {
    const QJsonObject c = section("color");
    const Style &d = kStyles[0];
    return {colourOf(c["low_color"], QColor(d.low[0], d.low[1], d.low[2])),
            colourOf(c["mid_color"], QColor(d.mid[0], d.mid[1], d.mid[2])),
            colourOf(c["high_color"], QColor(d.high[0], d.high[1], d.high[2])),
            colourOf(c["overlap_color"], QColor(d.overlap[0], d.overlap[1], d.overlap[2]))};
}

void Settings::setColour(int band, const QColor &colour) {
    static const char *keys[] = {"low_color", "mid_color", "high_color", "overlap_color"};
    setIn("color", keys[std::clamp(band, 0, 3)], QJsonArray{colour.red(), colour.green(), colour.blue()});
}

void Settings::setStyle(const QString &name) {
    for (const Style &style : kStyles) {
        if (name != QLatin1String(style.name)) continue;
        setIn("color", "low_color", rgb(style.low));
        setIn("color", "mid_color", rgb(style.mid));
        setIn("color", "overlap_color", rgb(style.overlap));
        setIn("color", "high_color", rgb(style.high));
        setView("style", name);
    }
}

}  // namespace wave
