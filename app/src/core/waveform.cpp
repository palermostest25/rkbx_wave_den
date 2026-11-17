#include "core/waveform.h"

#include <algorithm>
#include <cmath>
#include <functional>

namespace wave {
namespace {

// numpy's convolve(values, ones(window) / window, mode='same'), as the Python renderer smooths.
std::vector<float> smooth(const std::vector<float> &values, int window) {
    const int n = int(values.size());
    if (window <= 1 || n == 0) return values;
    const int shift = (window - 1) / 2;
    std::vector<float> out(n);
    for (int i = 0; i < n; ++i) {
        const int k = i + shift, from = std::max(0, k - window + 1), to = std::min(n - 1, k);
        double sum = 0;
        for (int j = from; j <= to; ++j) sum += values[j];
        out[i] = float(sum / window);
    }
    return out;
}

// numpy.maximum.reduceat over each pixel's first column: the loudest column a pixel covers.
std::vector<float> peaks(const float *values, int n, const std::vector<int> &start) {
    const int w = int(start.size());
    std::vector<float> out(w);
    for (int i = 0; i < w; ++i) {
        const int from = start[i], to = i + 1 < w ? start[i + 1] : n;
        float best = values[from];
        for (int j = from + 1; j < to; ++j) best = std::max(best, values[j]);
        out[i] = best;
    }
    return out;
}

}  // namespace

QImage renderBands(const float *low, const float *mid, const float *high, int n, int width, int height, int margin,
                   const Palette &palette) {
    QImage image(width, height, QImage::Format_RGB32);
    image.fill(Qt::black);
    if (n <= 0 || width <= 0) return image;
    const int halfH = (height - 2 * margin) / 2, centre = height / 2;
    // Each pixel column starts at floor(i * n / width), as numpy.linspace(0, n, width + 1) puts its edges.
    std::vector<int> start(width);
    for (int i = 0; i < width; ++i)
        start[i] = std::clamp(int(std::floor(double(i) * n / width)), 0, std::max(0, n - 1));
    auto clipped = [n](const float *values) {
        std::vector<float> out(values, values + n);
        for (float &v : out) v = std::clamp(v, 0.0f, 1.0f);
        return out;
    };
    const auto l = clipped(low), m = clipped(mid), h = clipped(high);
    const auto lc = peaks(l.data(), n, start), mc = peaks(m.data(), n, start), hc = peaks(h.data(), n, start);
    const QRgb lowC = palette.low.rgb(), midC = palette.mid.rgb(), highC = palette.high.rgb(),
               overlapC = palette.overlap.rgb(), bg = qRgb(0, 0, 0);
    for (int x = 0; x < width; ++x) {
        const int lowH = int(lc[x] * float(halfH)), midH = int(mc[x] * float(halfH)), rawHigh = int(hc[x] * float(halfH));
        const bool sounding = lc[x] > 0 || mc[x] > 0 || hc[x] > 0;
        const int highH = sounding ? std::max(rawHigh, 1) : rawHigh;  // a thin highs line wherever there is sound
        const int overlapHigh = std::max(rawHigh, 1);
        const int both = std::min(lowH, midH);
        for (int y = 0; y < height; ++y) {
            const int dist = std::abs(y - centre);
            QRgb c = bg;
            if (lowH > 0 && dist <= lowH) c = lowC;
            if (midH > 0 && dist <= midH) c = midC;
            if (highH > 0 && dist <= highH) c = highC;
            if (dist <= both && dist > overlapHigh) c = overlapC;  // low under mid: brown; highs stay above
            reinterpret_cast<QRgb *>(image.scanLine(y))[x] = c;
        }
    }
    return image;
}

QImage renderTile(const Analysis &analysis, int tile, const Palette &palette, const Levels &levels, int reduction) {
    const int start = tile * kTileColumns, end = std::min((tile + 1) * kTileColumns, analysis.columns());
    if (end <= start) return {};
    auto band = [&](const std::vector<float> &values, float gain) {
        std::vector<float> out(values.begin() + start, values.begin() + end);
        for (float &v : out) v *= gain;
        return smooth(out, std::min(std::max(1, levels.smoothing), int(out.size())));
    };
    const auto l = band(analysis.low, levels.low), m = band(analysis.mid, levels.mid), h = band(analysis.high, levels.high);
    const int width = (end - start + reduction - 1) / std::max(1, reduction);
    return renderBands(l.data(), m.data(), h.data(), end - start, width, kTileHeight, 3, palette);
}

size_t TileCache::Hash::operator()(const Key &k) const {
    size_t h = std::hash<const void *>()(k.analysis);
    for (size_t v : {size_t(k.tile), size_t(k.reduction), size_t(k.palette.low.rgb()), size_t(k.palette.mid.rgb()),
                     size_t(k.palette.high.rgb()), size_t(k.levels.smoothing)})
        h = h * 1000003u ^ v;
    for (float v : {k.levels.low, k.levels.mid, k.levels.high}) h = h * 1000003u ^ std::hash<float>()(v);
    return h;
}

bool TileCache::cached(const Analysis *analysis, int tile, const Palette &palette, const Levels &levels, int reduction) const {
    return images_.count({analysis, tile, reduction, palette, levels}) > 0;
}

const QImage &TileCache::get(const std::shared_ptr<const Analysis> &analysis, int tile, const Palette &palette,
                             const Levels &levels, int reduction) {
    const Key key{analysis.get(), tile, reduction, palette, levels};
    if (auto it = images_.find(key); it != images_.end()) {
        order_.splice(order_.end(), order_, it->second.place);  // most recently used
        return it->second.image;
    }
    order_.push_back(key);
    // The entry holds the analysis, so its address can never be reused by another track while cached.
    auto &entry = images_[key] = {analysis, renderTile(*analysis, tile, palette, levels, reduction), std::prev(order_.end())};
    while (int(images_.size()) > capacity_) {
        images_.erase(order_.front());
        order_.pop_front();
    }
    return entry.image;
}

}  // namespace wave
