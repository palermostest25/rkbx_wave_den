// Wave's settings: the same JSON file the Python app keeps (colour, render and view sections), read and
// written through typed accessors. Keys this version doesn't know are kept, so nothing a DJ set is lost
// moving between versions.
#pragma once

#include <QColor>
#include <QJsonObject>
#include <QString>
#include <QVariant>

#include <array>
#include <optional>

namespace wave {

QString settingsFolder();          // %APPDATA%\rkbx_wave, ~/Library/Application Support/rkbx_wave
QString settingsFile();            // ...\default_config.json (RKBX_WAVE_CONFIG_DIR overrides the folder)

// "3,1,2,4" as 0-based deck indices for ``count`` decks; nullopt when it isn't each deck once.
std::optional<std::vector<int>> deckOrder(const QString &text, int count);

class Settings {
public:
    static constexpr int kVersion = 3;

    Settings();
    // Reads ``path``; on a damaged file keeps the defaults and returns false, with ``error`` saying why.
    bool load(const QString &path, QString *error = nullptr);
    bool save(const QString &path, QString *error = nullptr) const;  // atomically: written aside, then moved

    // View (the "view" section), with the Python app's defaults.
    QVariant view(const QString &key) const;
    bool flag(const QString &key) const { return view(key).toBool(); }
    double number(const QString &key) const { return view(key).toDouble(); }
    QString text(const QString &key) const { return view(key).toString(); }
    void setView(const QString &key, const QVariant &value);

    // Render (the "render" section).
    int deckCount() const;
    void setDeckCount(int count);
    QString deckOrderText() const;
    void setDeckOrderText(const QString &order);
    std::vector<int> order() const;  // the decks, top to bottom
    QString displayKey() const;
    void setDisplayKey(const QString &key);
    bool fillDisplay() const;
    void setFillDisplay(bool on);
    double uiScale() const;
    void setUiScale(double scale);
    std::array<float, 3> gains() const;  // low, mid, high levels
    void setGain(int band, double value);
    int smoothing() const;

    // Colours (the "color" section): the three bands and the brown where low and mid overlap.
    std::array<QColor, 4> colours() const;  // low, mid, high, overlap
    void setColour(int band, const QColor &colour);
    void setStyle(const QString &name);     // "Rekordbox" or "Wave classic"

    const QJsonObject &json() const { return root_; }

private:
    QJsonObject section(const char *name) const { return root_.value(QLatin1String(name)).toObject(); }
    void setIn(const char *section, const QString &key, const QJsonValue &value);
    QJsonObject root_;
};

}  // namespace wave
