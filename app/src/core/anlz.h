// Rekordbox's analysis files (ANLZ*.DAT / .EXT / .2EX): the waveform, beat grid, cues and file path.
// Layouts from Deep Symmetry's analysis and pyrekordbox's definitions; every read is bounds-checked, so a
// damaged or unexpected file gives no data rather than a crash.
#pragma once

#include <QByteArray>
#include <QColor>
#include <QString>

#include <array>
#include <optional>
#include <vector>

namespace wave {

struct Beat {
    double timeMs = 0;   // from the start of the track
    int number = 0;      // 1-4 within its bar
    double bpm = 0;      // 0 when unknown
};

struct Cue {
    double timeSec = 0;
    QColor color;
    bool memory = false;           // a memory cue, else a hot cue
    int number = 0;                // hot cue slot, 1 = A
    std::optional<double> endSec;  // a loop's end
};

// One track's analysis, as Wave draws it: three bands of 0..1 heights at 150 columns a second.
struct Analysis {
    std::vector<float> low, mid, high;
    double duration = 0;           // seconds
    bool threeBand = true;         // PWV7; otherwise PWV5 heights shown in all three bands
    QString songPath;              // PPTH: the audio file, as Rekordbox recorded it
    std::vector<Beat> grid;
    std::vector<Cue> cues;         // from the files (USB exports); the collection's come from its database
    double originalBpm = 0;

    bool empty() const { return low.empty(); }
    int columns() const { return int(low.size()); }
};

struct Tag {
    QByteArray type;    // four letters: PWV7, PQTZ...
    QByteArray header;  // the whole tag header, the 12 common bytes included
    QByteArray body;    // what follows the header
};

// The tags of one ANLZ file; stops at the first damaged one.
std::vector<Tag> readTags(const QByteArray &file);
std::optional<Tag> findTag(const std::vector<Tag> &tags, const char *type);

// Pieces, for tests and for the cue/grid readers.
std::vector<Beat> beatGrid(const Tag &pqtz);
std::vector<Cue> cueList(const Tag &tag);              // PCOB or PCO2
QString songPath(const Tag &ppth);
std::array<float, 3> bandGains(const std::optional<Tag> &pwvc);
double originalBpm(const std::vector<Beat> &grid);
QColor cueColour(bool memory, int number);

// Everything Wave needs from a track's analysis folder. Empty when it holds no detailed waveform.
Analysis loadFolder(const QString &folder, QString *error = nullptr);

}  // namespace wave
