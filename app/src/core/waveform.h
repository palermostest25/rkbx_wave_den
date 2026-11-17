// Waveform pictures: a track drawn in tiles of 1024 columns (about 6.8 s), kept in a bounded cache, as
// Rekordbox draws its three bands: mirrored about the centre, low under mid under high, brown where low
// and mid overlap, and a thin highs line wherever there is sound.
#pragma once

#include "core/anlz.h"

#include <QColor>
#include <QImage>

#include <list>
#include <memory>
#include <unordered_map>

namespace wave {

struct Palette {
    QColor low{0, 84, 226}, mid{255, 166, 0}, high{246, 236, 217}, overlap{180, 102, 7};  // Rekordbox's
    bool operator==(const Palette &) const = default;
};

struct Levels {
    float low = 1, mid = 1, high = 1;  // the DJ's per-band gain
    int smoothing = 1;                 // columns averaged, 1 for none
    bool operator==(const Levels &) const = default;
};

constexpr int kTileColumns = 1024;
constexpr int kTileHeight = 256;

// One tile of a track: columns [tile * 1024, +1024), drawn ``reduction`` columns to a pixel (peaks kept).
QImage renderTile(const Analysis &analysis, int tile, const Palette &palette, const Levels &levels, int reduction = 1);

// The bands of ``n`` columns drawn ``width`` pixels wide (Wave's renderer, symmetric mode, peak reduction).
QImage renderBands(const float *low, const float *mid, const float *high, int n, int width, int height, int margin,
                   const Palette &palette);

// Tiles of the tracks on screen; the oldest go first once it holds ``capacity``.
class TileCache {
public:
    explicit TileCache(int capacity = 64) : capacity_(capacity) {}
    const QImage &get(const std::shared_ptr<const Analysis> &analysis, int tile, const Palette &palette,
                      const Levels &levels, int reduction = 1);
    bool cached(const Analysis *analysis, int tile, const Palette &palette, const Levels &levels, int reduction = 1) const;
    void clear() { order_.clear(); images_.clear(); }
    int size() const { return int(images_.size()); }

private:
    struct Key {
        const Analysis *analysis; int tile, reduction; Palette palette; Levels levels;
        bool operator==(const Key &) const = default;
    };
    struct Hash { size_t operator()(const Key &k) const; };
    struct Entry { std::shared_ptr<const Analysis> analysis; QImage image; std::list<Key>::iterator place; };
    int capacity_;
    std::list<Key> order_;  // most recent at the back
    std::unordered_map<Key, Entry, Hash> images_;
};

}  // namespace wave
