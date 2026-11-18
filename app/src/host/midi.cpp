#include "host/midi.h"

#include "host/rekordbox.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QLoggingCategory>
#include <QRegularExpression>

#include <chrono>
#include <thread>

#ifdef Q_OS_WIN
#include <windows.h>
#include <mmsystem.h>
#endif
#ifdef Q_OS_MACOS
#include <CoreFoundation/CoreFoundation.h>
#include <CoreMIDI/CoreMIDI.h>
#endif

Q_LOGGING_CATEGORY(lcMidi, "wave.midi")

namespace wave::midi {
namespace {

QStringList csvRow(const QString &line) {
    QStringList out;
    QString field;
    bool quoted = false;
    for (int i = 0; i < line.size(); ++i) {
        const QChar c = line[i];
        if (quoted) {
            if (c == '"' && i + 1 < line.size() && line[i + 1] == '"') { field += '"'; ++i; }
            else if (c == '"') quoted = false;
            else field += c;
        } else if (c == '"') quoted = true;
        else if (c == ',') { out << field; field.clear(); }
        else field += c;
    }
    out << field;
    return out;
}

QString deviceKey(QString text) {
    text = text.toCaseFolded();
    text.remove(QRegularExpression("pioneer( dj)?|midi|\\(\\d+\\)"));
    return text.remove(QRegularExpression("[^a-z0-9]"));
}

}  // namespace

bool shared() {
#ifdef Q_OS_MACOS
    return true;
#elif defined(Q_OS_WIN)
    HKEY key = nullptr;
    if (RegOpenKeyExW(HKEY_LOCAL_MACHINE, L"SYSTEM\\CurrentControlSet\\Services\\MidiSrv", 0, KEY_READ, &key) != ERROR_SUCCESS) return false;
    RegCloseKey(key);
    return true;
#else
    return false;
#endif
}

std::optional<Action> browseAction(const QString &function, int status) {
    // Rekordbox's maps name these a little differently between controllers, so names are matched loosely.
    QString key = function.toCaseFolded();
    key.remove(QRegularExpression("[^a-z0-9]"));
    const bool note = (status & 0xF0) == 0x90, control = (status & 0xF0) == 0xB0;
    if (key == QLatin1String("zoom") && control) return Action{Kind::Zoom};
    if (key.contains("shift") || key.contains("zoom") || key.contains("preview")) return std::nullopt;
    if (control && key.contains("sampler") && (key.contains("vol") || key.contains("level") || key.contains("gain")))
        return Action{Kind::SamplerZoom};
    const bool browser = key.contains("brows") || key.contains("rotary") || key.contains("selector");
    if (browser && control) return Action{Kind::Browse};
    static const QStringList presses = {"press", "push", "click", "focus", "enter", "select"};
    bool pressed = false;
    for (const QString &w : presses) pressed = pressed || key.contains(w);
    if ((browser && note && pressed) || ((key == "browse" || key == "browser") && note)) return Action{Kind::Press};
    static const QStringList backs = {"back", "browseback", "browserback", "browsebackward", "backbutton"};
    if (note && backs.contains(key)) return Action{Kind::Back};
    if (note && key.contains("view")) return Action{Kind::View};  // VIEW, BrowserView, ViewChange…
    const auto load = QRegularExpression("^load(?:track|deck|todeck)?(\\d?)$").match(key);
    if (note && load.hasMatch()) return Action{Kind::Load, load.captured(1).isEmpty() ? -1 : load.captured(1).toInt() - 1};
    return std::nullopt;
}

std::pair<QString, Table> loadMap(const QString &text, const QString &fileName) {
    QString name = fileName.section('.', 0, 0);
    Table table;
    const QRegularExpression midi("^[9Bb][0-9A-Fa-f]{3}$");
    for (QString line : text.split('\n')) {
        line.remove('\r');
        if (line.startsWith(QChar(0xFEFF))) line.remove(0, 1);
        const QStringList row = csvRow(line);
        if (row.size() > 2 && row[0] == QLatin1String("@file")) name = row[2];
        if (row.size() < 8 || !midi.match(row[3]).hasMatch()) continue;
        bool ok = false;
        const int status = row[3].left(2).toInt(&ok, 16), data1 = row[3].mid(2).toInt(nullptr, 16);
        // "Zoom" is Rekordbox's own waveform zoom; the rest are the browser's controls.
        const auto action = browseAction(row[0], status);
        if (!action) continue;
        // Deck columns hold the MIDI channel offset each deck sends on.
        bool any = false;
        for (int deck = 0; deck < 4; ++deck) {
            bool numeric = false;
            const int offset = row[4 + deck].trimmed().toInt(&numeric);
            if (!numeric || offset >= 16) continue;
            any = true;
            Action a = *action;
            if (a.kind == Kind::Load) a.deck = deck;
            table.try_emplace({status + offset, data1}, a);
        }
        if (!any) table.try_emplace({status, data1}, *action);  // a control for all decks; LoadDeck3 names its deck
    }
    return {name, table};
}

std::optional<std::pair<QString, Table>> findMap(const QString &device, const std::vector<std::pair<QString, Table>> &maps) {
    // The map whose controller name is the longest match inside the device name.
    const QString wanted = deviceKey(device);
    std::optional<std::pair<QString, Table>> best;
    int bestLength = 0;
    for (const auto &[name, table] : maps) {
        const QString key = deviceKey(name);
        if (!key.isEmpty() && wanted.contains(key) && key.size() > bestLength) { best = {name, table}; bestLength = int(key.size()); }
    }
    return best;
}

std::vector<std::array<int, 3>> messages(const QByteArray &data) {
    std::vector<std::array<int, 3>> out;
    int status = 0;
    std::vector<int> pending;
    bool sysex = false;
    for (const char raw : data) {
        const int byte = uchar(raw);
        if (byte >= 0xF8) continue;
        if (byte >= 0x80) {
            sysex = byte == 0xF0;
            status = byte < 0xF0 ? byte : 0;
            pending.clear();
            continue;
        }
        if (sysex || !status) continue;
        pending.push_back(byte);
        if (int(pending.size()) == (status >= 0xC0 && status < 0xE0 ? 1 : 2)) {
            out.push_back({status, pending.front(), pending.back()});
            pending.clear();  // running status keeps ``status``
        }
    }
    return out;
}

QStringList mappingFolders() {
    // The DJ's own MIDI-learn maps first, then those shipped with Rekordbox.
    QStringList folders;
    auto add = [&](const QString &folder) { if (QFileInfo(folder).isDir() && !folders.contains(folder)) folders << folder; };
#ifdef Q_OS_MACOS
    add(QDir::home().filePath("Library/Pioneer/rekordbox/MidiMappings"));
    add("/Applications/rekordbox 7/rekordbox.app/Contents/Resources/MidiMappings");
    for (const QString &app : QDir("/Applications").entryList({"rekordbox*"}, QDir::Dirs, QDir::Name | QDir::Reversed))
        add("/Applications/" + app + "/rekordbox.app/Contents/Resources/MidiMappings");
#else
    add(QDir(qEnvironmentVariable("APPDATA")).filePath("Pioneer/rekordbox/MidiMappings"));
    const auto installs = host::installed();
    for (auto it = installs.rbegin(); it != installs.rend(); ++it) {
        const QString base = QFileInfo(it->executable).absolutePath();
        add(base + "/MidiMappings");
        for (const QString &sub : QDir(base).entryList(QDir::Dirs | QDir::NoDotAndDotDot)) add(base + "/" + sub + "/MidiMappings");
    }
#endif
    return folders;
}

Listener::Listener(QObject *parent) : QObject(parent) { qRegisterMetaType<wave::midi::Kind>(); }
Listener::~Listener() { stop(); }

void Listener::start() {
    if (thread_) return;
    stop_ = false;
    thread_ = QThread::create([this] { run(); });
    thread_->setObjectName("controller-midi");
    thread_->start();
}

void Listener::stop() {
    if (!thread_) return;
    stop_ = true;
    thread_->wait(3000);
    delete thread_;
    thread_ = nullptr;
}

QStringList Listener::controllers() const {
    std::lock_guard l(lock_);
    QStringList names;
    for (const auto &[port, device] : devices_) if (!names.contains(device.first)) names << device.first;
    names.sort();
    return names;
}

std::set<Kind> Listener::controls() const {
    std::lock_guard l(lock_);
    std::set<Kind> kinds;
    for (const auto &[port, device] : devices_) for (const auto &[key, action] : device.second) kinds.insert(action.kind);
    return kinds;
}

QString Listener::status() const { std::lock_guard l(lock_); return status_; }

void Listener::loadMaps() {
    std::vector<std::pair<QString, Table>> maps;
    QStringList seen;
    for (const QString &folder : mappingFolders())
        for (const QString &file : QDir(folder).entryList({"*.midi.csv"}, QDir::Files, QDir::Name)) {
            QFile f(QDir(folder).filePath(file));
            if (!f.open(QIODevice::ReadOnly)) continue;
            auto map = loadMap(QString::fromUtf8(f.readAll()), file);
            if (!map.second.empty() && !seen.contains(map.first)) { seen << map.first; maps.push_back(std::move(map)); }
        }
    maps_ = std::move(maps);
    std::lock_guard l(lock_);
    status_ = maps_.empty() ? QStringLiteral("Rekordbox's MIDI maps were not found") : QString();
}

bool Listener::attach(quintptr port, const QString &name) {
    const auto found = findMap(name, maps_);
    if (!found) return false;
    std::lock_guard l(lock_);
    devices_[port] = *found;
    QStringList kinds;
    static const char *names[] = {"browse", "press", "back", "view", "load", "zoom", "sampler zoom"};
    std::set<Kind> have;
    for (const auto &[key, action] : found->second) have.insert(action.kind);
    for (Kind k : have) kinds << names[int(k)];
    qCInfo(lcMidi) << "Controller" << found->first << "on" << name << ": controls" << kinds.join(", ");
    return true;
}

void Listener::addDevice(quintptr port, const QString &name, const Table &table) {
    std::lock_guard l(lock_);
    devices_[port] = {name, table};
}

void Listener::receive(quintptr port, const QByteArray &data) {
    Table table;
    {
        std::lock_guard l(lock_);
        const auto it = devices_.find(port);
        if (it == devices_.end()) return;
        table = it->second.second;
    }
    for (const auto &[status, data1, value] : messages(data)) {
        const auto it = table.find({status, data1});
        if (it == table.end()) continue;
        const Action &a = it->second;
        if (a.kind == Kind::Browse || a.kind == Kind::Zoom) {
            // An endless encoder: 1 = one step clockwise, 127 = one step back.
            const int steps = value < 64 ? value : value - 128;
            if (steps) emit controller(a.kind, steps, -1);
        } else if (a.kind == Kind::SamplerZoom) {
            emit controller(a.kind, value, -1);
        } else if (value) {  // presses only: note-on velocity or button CC value above zero
            emit controller(a.kind, 0, a.deck);
        }
    }
}


void Listener::run() {
    double retry = 0;
    auto now = [] { return std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count(); };
#ifdef Q_OS_WIN
    struct Port { HMIDIIN handle; std::unique_ptr<std::pair<Listener *, quintptr>> who; };
    std::vector<Port> ports;
    std::optional<QStringList> names;
    struct Self { static void CALLBACK cb(HMIDIIN, UINT message, DWORD_PTR instance, DWORD_PTR param, DWORD_PTR) {
        if (message != MIM_DATA) return;
        auto *who = reinterpret_cast<std::pair<Listener *, quintptr> *>(instance);
        const char bytes[3] = {char(param & 0xFF), char(param >> 8 & 0x7F), char(param >> 16 & 0x7F)};
        who->first->receive(who->second, QByteArray(bytes, 3));
    } };
    auto closeAll = [&] {
        for (Port &p : ports) { midiInStop(p.handle); midiInClose(p.handle); }
        ports.clear();
        std::lock_guard l(lock_);
        devices_.clear();
    };
    while (!stop_) {
        if (maps_.empty() && now() >= retry) {
            retry = now() + 5;
            loadMaps();
            if (!maps_.empty()) names.reset();  // reopen with maps to match against
        }
        QStringList current;
        for (UINT i = 0; i < midiInGetNumDevs(); ++i) {
            MIDIINCAPSW caps{};
            current << (midiInGetDevCapsW(i, &caps, sizeof caps) == MMSYSERR_NOERROR ? QString::fromWCharArray(caps.szPname) : QString());
        }
        if (!names || *names != current) {
            closeAll();  // port numbers shift when devices come and go: reopen all
            names = current;
            QStringList busy;
            for (int i = 0; i < current.size(); ++i) {
                if (current[i].isEmpty() || !findMap(current[i], maps_)) continue;
                Port p{nullptr, std::make_unique<std::pair<Listener *, quintptr>>(this, quintptr(i + 1))};
                if (midiInOpen(&p.handle, UINT(i), reinterpret_cast<DWORD_PTR>(&Self::cb), reinterpret_cast<DWORD_PTR>(p.who.get()),
                               CALLBACK_FUNCTION) != MMSYSERR_NOERROR) {
                    busy << current[i];  // classic Windows MIDI allows one app per port
                    continue;
                }
                attach(quintptr(i + 1), current[i]);
                midiInStart(p.handle);
                ports.push_back(std::move(p));
            }
            {
                std::lock_guard l(lock_);
                if (!busy.isEmpty()) status_ = busy.join(", ") + QStringLiteral(" MIDI is held by Rekordbox");
            }
            qCInfo(lcMidi) << "MIDI ports" << current << "; reading" << controllers() << "; held elsewhere" << busy;
        }
        for (int i = 0; i < 100 && !stop_; ++i) std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
    closeAll();
#elif defined(Q_OS_MACOS)
    MIDIClientRef client = 0;
    MIDIPortRef port = 0;
    if (MIDIClientCreate(CFSTR("Rekordbox Wave"), nullptr, nullptr, &client) != noErr ||
        MIDIInputPortCreate(client, CFSTR("Rekordbox Wave"), [](const MIDIPacketList *list, void *self, void *source) {
            static_cast<Listener *>(self)->macPackets(list, reinterpret_cast<quintptr>(source));
        }, this, &port) != noErr) {
        std::lock_guard l(lock_);
        status_ = QStringLiteral("MIDI unavailable");
        return;
    }
    std::set<MIDIEndpointRef> connected;
    while (!stop_) {
        if (maps_.empty() && now() >= retry) {
            retry = now() + 5;
            loadMaps();
            std::lock_guard l(lock_);
            for (auto it = connected.begin(); it != connected.end();)  // look again at devices seen without a map
                it = devices_.count(quintptr(*it)) ? std::next(it) : connected.erase(it);
        }
        std::set<MIDIEndpointRef> present;
        for (ItemCount i = 0; i < MIDIGetNumberOfSources(); ++i) {
            const MIDIEndpointRef source = MIDIGetSource(i);
            present.insert(source);
            if (connected.count(source)) continue;
            connected.insert(source);
            CFStringRef name = nullptr;
            QString label;
            if (MIDIObjectGetStringProperty(source, kMIDIPropertyDisplayName, &name) == noErr && name) {
                label = QString::fromCFString(name);
                CFRelease(name);
            }
            if (attach(quintptr(source), label)) MIDIPortConnectSource(port, source, reinterpret_cast<void *>(quintptr(source)));
        }
        {
            std::lock_guard l(lock_);
            for (MIDIEndpointRef gone : connected) if (!present.count(gone)) devices_.erase(quintptr(gone));
        }
        for (auto it = connected.begin(); it != connected.end();) it = present.count(*it) ? std::next(it) : connected.erase(it);
        // The run loop delivers CoreMIDI's device notifications, so new sources appear.
        CFRunLoopRunInMode(kCFRunLoopDefaultMode, 1.0, false);
        for (int i = 0; i < 25 && !stop_; ++i) std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
    MIDIClientDispose(client);
#else
    Q_UNUSED(retry);
    Q_UNUSED(now);
#endif
}

#ifdef Q_OS_MACOS
void Listener::macPackets(const void *data, quintptr source) {
    const auto *list = static_cast<const MIDIPacketList *>(data);
    const MIDIPacket *packet = &list->packet[0];
    for (UInt32 i = 0; i < list->numPackets; ++i) {
        receive(source, QByteArray(reinterpret_cast<const char *>(packet->data), packet->length));
        packet = MIDIPacketNext(packet);
    }
}
#endif

}  // namespace wave::midi
