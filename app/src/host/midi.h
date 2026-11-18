// The controller's browser controls, read beside Rekordbox and decoded with Rekordbox's own MIDI maps:
// the browse knob turning and pressed, BACK, VIEW, each deck's LOAD, waveform zoom (SHIFT + BROWSE) and the
// sampler volume knob as a zoom. Listening is passive; nothing is ever sent to the controller. Only where
// the system shares the controller: classic Windows MIDI gives a port to one app, and taking it from
// Rekordbox would stop its decks, so on Windows only with Windows MIDI Services.
#pragma once

#include <QObject>
#include <QString>
#include <QStringList>
#include <QThread>

#include <atomic>
#include <map>
#include <mutex>
#include <optional>
#include <set>
#include <vector>

namespace wave::midi {

enum class Kind { Browse, Press, Back, View, Load, Zoom, SamplerZoom };
struct Action { Kind kind; int deck = -1; };  // deck: for LOAD, -1 when the map doesn't say
using Table = std::map<std::pair<int, int>, Action>;  // (status, data1) -> action

bool shared();                                         // the controller can be read beside Rekordbox here
std::optional<Action> browseAction(const QString &function, int status);
std::pair<QString, Table> loadMap(const QString &csvText, const QString &fileName);
std::optional<std::pair<QString, Table>> findMap(const QString &device, const std::vector<std::pair<QString, Table>> &maps);
std::vector<std::array<int, 3>> messages(const QByteArray &data);  // (status, data1, data2)
QStringList mappingFolders();

class Listener : public QObject {
    Q_OBJECT
public:
    explicit Listener(QObject *parent = nullptr);
    ~Listener() override;
    void start();
    void stop();
    QStringList controllers() const;     // the connected controllers with a Rekordbox map
    std::set<Kind> controls() const;     // which browser controls their maps have
    QString status() const;

signals:
    // Browse: ``value`` steps (+ clockwise); Zoom: steps; SamplerZoom: the knob 0-127; others: a press.
    void controller(wave::midi::Kind kind, int value, int deck);

private:
    void run();
    void loadMaps();
    void receive(quintptr port, const QByteArray &data);
    bool attach(quintptr port, const QString &name);

    std::vector<std::pair<QString, Table>> maps_;
    mutable std::mutex lock_;
    std::map<quintptr, std::pair<QString, Table>> devices_;
    QString status_;
    QThread *thread_ = nullptr;
    std::atomic<bool> stop_{false};
#ifdef Q_OS_MACOS
public:
    void macPackets(const void *list, quintptr source);
#endif
};

}  // namespace wave::midi

Q_DECLARE_METATYPE(wave::midi::Kind)
