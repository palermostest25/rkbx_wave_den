// Everything the deck screen shows, kept up to date: Rekordbox's decks (through the reader), each loaded
// track's analysis and cues, the playback timelines, touch previews and the zoom. The window reads it
// to draw; nothing here draws.
#pragma once

#include "app/reader.h"
#include "core/anlz.h"
#include "core/library.h"
#include "core/settings.h"
#include "core/timeline.h"
#include "host/host.h"

#include <QFutureWatcher>
#include <QObject>
#include <QThreadPool>
#include <QTimer>

#include <array>
#include <map>
#include <memory>

namespace wave {

struct DeckView {
    std::shared_ptr<const Analysis> analysis;
    std::vector<Cue> cues;
    TrackInfo meta;                // as the library has the loaded track
    QString requested;             // the analysis folder asked for
    QString error;                 // why there is nothing to show, if so
    double currentBpm = 0, originalBpm = 0;
    PlaybackTimeline timeline{false};
    std::optional<bridge::Deck> status;  // master, sync, key lock... while bridged
    uint64_t generation = 0;       // a new number per track, for caches
};

// The handle Rekordbox gives the stick mounted at ``root``, 0 when Rekordbox hasn't mounted it.
int64_t deviceHandle(const QString &root, const std::vector<std::pair<int64_t, QString>> &devices);

class Session : public QObject {
    Q_OBJECT
public:
    static constexpr double kPreviewHold = 3.0, kPreviewReturn = .35;
    static constexpr double kKnobNear = 4.0, kKnobFar = 128.0;  // beats across a lane at the sampler knob's ends

    explicit Session(const QString &settingsPath, bool demo = false, QObject *parent = nullptr);
    ~Session() override;

    Settings settings;
    QString settingsPath;
    QString settingsError;         // the saved settings could not be read, and why
    bool save();

    const DeckView &deck(int i) const { return decks_[size_t(i)]; }
    std::vector<int> order() const { return settings.order(); }
    std::optional<double> position(int idx) const;
    std::optional<double> previewPosition(int idx) const;
    bool playing(int idx) const;
    bool animating() const;        // something moves on screen: frames needed
    QString status() const { return status_; }
    bool connected() const { return frame_ && frame_->connected; }
    bool bridged() const { return frame_ && frame_->bridged; }
    std::shared_ptr<const ReaderFrame> lastFrame() const { return frame_; }
    host::Host *host() const { return host_; }
    bool demo() const { return demo_; }

    double zoomBeats() const;      // beats across a lane now: the knob's while in use, else the setting
    double knobZoom(int value, bool reverse);
    void endKnobZoom() { knobBeats_.reset(); }

    void previewHold(int idx, double seconds);
    void previewRelease(int idx);

    // Ask Rekordbox to load a track: from the collection, or from the stick whose library is ``source``.
    QString loadUnavailable() const;  // why touch loading can't be used now, "" when it can
    bool loadTrack(int idx, const CatalogTrack &track, const QString &source = {});
    bool loadedByWave(int idx, const QString &path) const;
    bool loading() const { return loading_.has_value(); }
    void toggleMasterTempo(int idx);
    // Rekordbox's own browser window (macOS: shown at x, y; Windows: its split screen toggled). ``answered``: Rekordbox's reply.
    void showRekordboxBrowser(bool on, int x, int y, std::function<void(bool, QString)> answered = {});
    void connectRekordbox();

signals:
    void changed();                       // something visible changed
    void analysisChanged();
    void statusChanged(const QString &status);
    void loadStatus(const QString &message);
    void frameArrived(std::shared_ptr<const ReaderFrame> frame);

private:
    void receive(std::shared_ptr<const ReaderFrame> f);
    void openTrack(int idx, const QString &folder);
    void clearDeck(int idx);
    void pollCues();
    void pollHost();
    void pollLoading();
    void setStatus(const QString &s);
    void request(std::function<std::pair<bool, QString>(bridge::Client &)> ask, std::function<void(bool, QString)> answered);
    host::Seen seen();

    bool demo_ = false;
    std::array<DeckView, 4> decks_;
    std::shared_ptr<const ReaderFrame> frame_;
    Reader *reader_ = nullptr;
    host::Host *host_ = nullptr;
    QString status_;
    std::map<int, std::pair<double, std::optional<double>>> previews_;  // deck -> (seconds, held until)
    std::optional<double> knobBeats_;
    QThreadPool analysisPool_, requestPool_;
    std::shared_ptr<Libraries> cueLibraries_;  // used only on the analysis pool's one thread
    bool cuePolling_ = false;
    QString masterPath_, cueSignature_;
    QStringList cueFolders_;
    double masterCheckedAt_ = 0;
    QTimer cueTimer_, hostTimer_;
    uint64_t nextGeneration_ = 1;
    struct Loading { int deck; QString title, path; double deadline; bool answered = false, accepted = false; };
    std::optional<Loading> loading_;
    std::map<int, std::pair<QString, double>> waveLoads_;  // deck -> (file Wave asked for, until)
    std::map<uint32_t, std::pair<std::optional<double>, double>> ages_;  // pid -> (age, when measured)
    std::optional<bool> bridgeModules_;
    double started_ = 0;
};

}  // namespace wave
