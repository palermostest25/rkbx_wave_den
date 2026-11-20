// The controller's browser controls, decoded with Rekordbox's own MIDI maps: the browse knob turning and
// pressed, BACK, VIEW, each deck's LOAD, waveform zoom (SHIFT + BROWSE) and the sampler volume knob as a zoom.
// Nothing is ever sent to the controller.
//
// On Windows, Rekordbox holds the controller, and Wave hears it through Wave's bridge inside Rekordbox, which
// also keeps the browser controls from Rekordbox while Wave drives its own browser with them (setKeeping). Where
// the bridge isn't there, Wave opens the controller itself only where Windows shares it (Windows MIDI Services),
// never taking it from Rekordbox. On macOS CoreMIDI shares it.
#pragma once

#include <QObject>
#include <QString>
#include <QStringList>
#include <QThread>

#include <array>
#include <atomic>
#include <map>
#include <mutex>
#include <optional>
#include <set>
#include <vector>

namespace wave::midi {

enum class Kind { Browse, Press, Back, View, Load, Zoom, SamplerZoom, LoopIn, LoopOut, LoopExit };  // loop keys: Wave only watches
enum class Keep { Nothing, View, Browser };  // what the bridge keeps from Rekordbox for Wave
struct Action { Kind kind; int deck = -1; };  // deck: for LOAD, -1 when the map doesn't say
using Table = std::map<std::pair<int, int>, Action>;  // (status, data1) -> action

bool shared();                                         // the controller can be read beside Rekordbox here
std::optional<Action> browseAction(const QString &function, int status, const QString &control = {});
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
    // Windows: may Wave open the controller now (Rekordbox is running with it)? macOS shares it always.
    void setAllowed(bool on) { allowed_ = on; }
    // Windows: keep the browse knob, its push, BACK, VIEW and LOAD from Rekordbox, for Wave's browser.
    void setKeeping(Keep what) { keeping_ = int(what); }
    void setKeeping(bool on) { setKeeping(on ? Keep::Browser : Keep::Nothing); }
    // Wave's browser has the browse controls: asked, and the bridge does it.
    bool keeping() const { return keeping_ == int(Keep::Browser) && throughRekordbox_; }
    bool throughRekordbox() const { return throughRekordbox_; }     // heard through Wave's bridge in Rekordbox
    static std::vector<std::pair<int, int>> keys(const Table &table, Keep what = Keep::Browser);  // what keeping keeps
    void assumeThroughRekordbox(bool on) { throughRekordbox_ = on; }    // tests: as the bridge would report

signals:
    // Browse: ``value`` steps (+ clockwise); Zoom: steps; SamplerZoom: the knob 0-127; others: a press.
    void controller(wave::midi::Kind kind, int value, int deck);

public:
    // What the platform's MIDI callbacks call; public for tests.
    void receive(quintptr port, const QByteArray &data);
    void addDevice(quintptr port, const QString &name, const Table &table);
    void receiveMessage(const Table &table, int status, int data1, int value);
    Table merged() const;  // every connected controller's controls, for messages heard through Rekordbox

private:
    void run();
    void loadMaps();
    bool attach(quintptr port, const QString &name);

    std::vector<std::pair<QString, Table>> maps_;
    mutable std::mutex lock_;
    std::map<quintptr, std::pair<QString, Table>> devices_;
    QString status_;
    QThread *thread_ = nullptr;
    std::atomic<bool> stop_{false};
    std::atomic<bool> allowed_{false}, throughRekordbox_{false};
    std::atomic<int> keeping_{0};
#ifdef Q_OS_MACOS
public:
    void macPackets(const void *list, quintptr source);
#endif
};

}  // namespace wave::midi

Q_DECLARE_METATYPE(wave::midi::Kind)
