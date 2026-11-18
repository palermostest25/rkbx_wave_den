// Rekordbox's own deck state, read from Wave's bridge inside Rekordbox on a thread of its own, about a
// hundred times a second while Rekordbox reports something new. Each loaded file is looked up in
// Rekordbox's library (the collection, or the USB stick it plays from) for its analysis and its title.
// Frames go to the UI thread only when something changed, so a quiet Rekordbox costs nothing.
#pragma once

#include "core/bridge.h"
#include "host/host.h"

#include <QObject>
#include <QThread>

#include <atomic>
#include <map>
#include <memory>

namespace wave {

struct TrackInfo {
    QString path;                 // the audio file Rekordbox's engine has loaded, "" for an empty deck
    QString analysisFolder;       // its ANLZ folder, "" when it isn't in a library Wave can read
    QString title, artist, key, artwork, error;
};

struct ReaderFrame {
    host::State state = host::State::Absent;
    QString status;
    bool connected = false, bridged = false, stale = false, checked = false;
    uint32_t pid = 0;
    std::vector<bridge::Deck> decks;                    // while bridged
    std::vector<std::pair<int64_t, QString>> devices;   // sticks Rekordbox has mounted
    std::optional<bridge::Browser> browser;
    double when = 0;                                    // when the deck values were true (wave::now())
    bool decksChanged = false;
    std::map<int, TrackInfo> tracks;                    // decks whose file changed in this frame
};

QString streamingService(const QString &path);  // "Spotify" for spotify:track:…, "" for a file

class Reader : public QObject {
    Q_OBJECT
public:
    static constexpr const char *kRelaunch = "Quit Rekordbox and press CONNECT in Wave for exact loops and touch loading";
    static constexpr const char *kBusy = "REKORDBOX · busy";
    static constexpr const char *kStarting = "Rekordbox is starting";

    explicit Reader(QObject *parent = nullptr);
    ~Reader() override;
    void start();
    void stop();

signals:
    void frame(std::shared_ptr<const ReaderFrame> frame);

private:
    void run();
    QThread *thread_ = nullptr;
    std::atomic<bool> stop_{false};
};

}  // namespace wave

Q_DECLARE_METATYPE(std::shared_ptr<const wave::ReaderFrame>)
