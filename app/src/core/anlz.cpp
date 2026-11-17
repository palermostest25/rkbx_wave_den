#include "core/anlz.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>

#include <algorithm>
#include <cmath>
#include <map>
#include <tuple>

namespace wave {
namespace {

quint32 u32(const QByteArray &b, int at) {
    if (at < 0 || at + 4 > b.size()) return 0;
    const auto *p = reinterpret_cast<const uchar *>(b.constData() + at);
    return quint32(p[0]) << 24 | quint32(p[1]) << 16 | quint32(p[2]) << 8 | p[3];
}
quint16 u16(const QByteArray &b, int at) {
    if (at < 0 || at + 2 > b.size()) return 0;
    const auto *p = reinterpret_cast<const uchar *>(b.constData() + at);
    return quint16(p[0] << 8 | p[1]);
}
quint8 u8(const QByteArray &b, int at) {
    return at >= 0 && at < b.size() ? quint8(b[at]) : 0;
}

QStringList filesOf(const QString &folder, const char *suffix) {
    QDir dir(folder);
    QStringList found = dir.entryList({QStringLiteral("*.") + suffix}, QDir::Files, QDir::Name);
    for (QString &name : found) name = dir.filePath(name);
    return found;
}

std::vector<Tag> tagsOf(const QString &path) {
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) return {};
    return readTags(file.readAll());
}

// PWV7: low, mid and high heights (0..~100) at 150 a second. PWV5: 16-bit colour detail, whose height
// (bits 2-6) is shown in all three bands rather than inventing bands it doesn't have.
bool waveformOf(const std::vector<Tag> &tags, Analysis &out, const std::array<float, 3> &gains) {
    if (auto tag = findTag(tags, "PWV7")) {
        const int count = int(u32(tag->header, 16));
        if (u32(tag->header, 12) == 3 && count > 0 && tag->body.size() >= 3 * count) {
            out.low.resize(count); out.mid.resize(count); out.high.resize(count);
            const auto *p = reinterpret_cast<const uchar *>(tag->body.constData());
            for (int i = 0; i < count; ++i) {
                // Per-band gains before the clip, so a boosted band keeps its peaks.
                out.low[i] = std::clamp(p[3 * i] / 100.0f * gains[0], 0.0f, 1.0f);
                out.mid[i] = std::clamp(p[3 * i + 1] / 100.0f * gains[1], 0.0f, 1.0f);
                out.high[i] = std::clamp(p[3 * i + 2] / 100.0f * gains[2], 0.0f, 1.0f);
            }
            out.threeBand = true;
            return true;
        }
    }
    if (auto tag = findTag(tags, "PWV5")) {
        const int count = int(u32(tag->header, 16));
        if (count > 0 && tag->body.size() >= 2 * count) {
            out.low.resize(count);
            for (int i = 0; i < count; ++i)
                out.low[i] = std::clamp(((u16(tag->body, 2 * i) & 0x7c) >> 2) / 31.0f, 0.0f, 1.0f);
            out.mid = out.high = out.low;
            out.threeBand = false;
            return true;
        }
    }
    return false;
}

}  // namespace

std::vector<Tag> readTags(const QByteArray &file) {
    std::vector<Tag> tags;
    if (file.size() < 12 || !file.startsWith("PMAI")) return tags;
    qint64 offset = u32(file, 4);
    while (offset + 12 <= file.size()) {
        const quint32 header = u32(file, int(offset) + 4), length = u32(file, int(offset) + 8);
        if (length < 12 || header < 12 || header > length || offset + length > file.size()) break;
        Tag tag;
        tag.type = file.mid(int(offset), 4);
        tag.header = file.mid(int(offset), int(header));
        tag.body = file.mid(int(offset + header), int(length - header));
        tags.push_back(std::move(tag));
        offset += length;
    }
    return tags;
}

std::optional<Tag> findTag(const std::vector<Tag> &tags, const char *type) {
    for (const Tag &tag : tags)
        if (tag.type == type) return tag;
    return std::nullopt;
}

std::vector<Beat> beatGrid(const Tag &pqtz) {
    // Header: count at 20. Entries: beat (u16), tempo x100 (u16), time in ms (u32).
    std::vector<Beat> grid;
    const int count = int(u32(pqtz.header, 20));
    if (count <= 0 || pqtz.body.size() < 8 * count) return grid;
    grid.reserve(count);
    for (int i = 0; i < count; ++i) {
        const double tempo = u16(pqtz.body, 8 * i + 2) / 100.0;
        grid.push_back({double(u32(pqtz.body, 8 * i + 4)), int(u16(pqtz.body, 8 * i)), tempo > 0 ? tempo : 0.0});
    }
    return grid;
}

QColor cueColour(bool memory, int number) {
    if (memory) return QColor(255, 190, 0);  // amber
    static const QColor hotCues[] = {{230, 40, 40}, {50, 200, 215}, {45, 125, 45}, {160, 60, 205},
                                   {70, 230, 70}, {255, 145, 20}, {35, 70, 205}, {240, 220, 45}};
    return number >= 1 && number <= 8 ? hotCues[number - 1] : QColor(0, 200, 130);
}

namespace {
std::optional<double> loopEnd(quint32 start, quint32 end) {
    if (end > start && end < 0xffffffffu) return end / 1000.0;
    return std::nullopt;
}
}  // namespace

std::vector<Cue> cueList(const Tag &tag) {
    std::vector<Cue> cues;
    const bool rich = tag.type == "PCO2";
    if (!rich && tag.type != "PCOB") return cues;
    const int count = rich ? u16(tag.header, 16) : u16(tag.header, 18);
    // Entries (PCPT / PCP2) follow the header, each its own length long.
    int at = 0;
    for (int i = 0; i < count && at + 12 <= tag.body.size(); ++i) {
        const quint32 length = u32(tag.body, at + 8);
        if (length < 12 || at + int(length) > tag.body.size()) break;
        const QByteArray entry = tag.body.mid(at, int(length));
        at += int(length);
        const int number = int(u32(entry, 12));
        const quint32 time = rich ? u32(entry, 20) : u32(entry, 32);
        if (time == 0xffffffffu) continue;
        Cue cue;
        cue.timeSec = time / 1000.0;
        cue.memory = number == 0;
        cue.number = number;
        const bool loop = (rich ? u8(entry, 16) : u8(entry, 28)) == 2;
        if (loop) cue.endSec = loopEnd(time, rich ? u32(entry, 24) : u32(entry, 36));
        cue.color = cueColour(cue.memory, number);
        if (rich) {  // PCP2 carries Rekordbox's own colour after its comment
            const int comment = int(u32(entry, 40));
            const int rgb = 44 + comment + 1;
            if (comment >= 0 && rgb + 3 <= entry.size() && (u8(entry, rgb) || u8(entry, rgb + 1) || u8(entry, rgb + 2)))
                cue.color = QColor(u8(entry, rgb), u8(entry, rgb + 1), u8(entry, rgb + 2));
        }
        cues.push_back(cue);
    }
    return cues;
}

QString songPath(const Tag &ppth) {
    const int length = int(u32(ppth.header, 12));
    if (length < 2 || ppth.body.size() < length) return {};
    QString path;
    const int bytes = length - 2;  // UTF-16BE, then a two-byte terminator
    for (int i = 0; i + 1 < bytes; i += 2) {
        const QChar c(u16(ppth.body, i));
        if (c.isNull()) break;
        path.append(c);
    }
    return path;
}

std::array<float, 3> bandGains(const std::optional<Tag> &pwvc) {
    // PWVC: the gain (percent) Rekordbox's analyser gave each band. Dividing it back out puts every track
    // on one scale. 100/100/100 means none was applied, and those files store hotter heights: measured
    // against their own PWV5 over a 4,300-track library, these factors bring them to the same scale.
    if (!pwvc || pwvc->body.size() < 6) return {1.0f, 1.0f, 1.0f};
    const int at = int(pwvc->body.size()) - 6;
    const quint16 g[3] = {u16(pwvc->body, at), u16(pwvc->body, at + 2), u16(pwvc->body, at + 4)};
    if (g[0] == 100 && g[1] == 100 && g[2] == 100) return {0.99f, 0.69f, 0.16f};
    if (g[0] && g[1] && g[2]) return {100.0f / g[0], 100.0f / g[1], 100.0f / g[2]};
    return {1.0f, 1.0f, 1.0f};
}

double originalBpm(const std::vector<Beat> &grid) {
    for (const Beat &beat : grid)
        if (beat.bpm > 0) return beat.bpm;
    double total = 0;
    int intervals = 0;
    for (size_t i = 1; i < grid.size(); ++i) {
        const double delta = grid[i].timeMs - grid[i - 1].timeMs;
        if (delta > 0) { total += delta; ++intervals; }
    }
    return intervals && total > 0 ? 60000.0 / (total / intervals) : 0.0;
}

Analysis loadFolder(const QString &folder, QString *error) {
    Analysis out;
    if (!QFileInfo(folder).isDir()) {
        if (error) *error = QStringLiteral("Analysis folder not found: %1").arg(folder);
        return out;
    }
    const QStringList twoEx = filesOf(folder, "2EX"), ext = filesOf(folder, "EXT"), dat = filesOf(folder, "DAT");
    // The waveform: .2EX first (PWV7, with its gains), then each .EXT (PWV5).
    QStringList waveformFiles = twoEx.isEmpty() ? QStringList() : QStringList{twoEx.first()};
    waveformFiles += ext;
    for (const QString &path : waveformFiles) {
        const auto tags = tagsOf(path);
        const auto gains = path.endsWith(QLatin1String(".2EX"), Qt::CaseInsensitive)
                               ? bandGains(findTag(tags, "PWVC")) : std::array<float, 3>{1, 1, 1};
        if (auto ppth = findTag(tags, "PPTH"); ppth && out.songPath.isEmpty()) out.songPath = songPath(*ppth);
        if (waveformOf(tags, out, gains)) break;
    }
    if (out.empty()) {
        if (error) *error = QStringLiteral("No detailed waveform in this folder");
        return out;
    }
    out.duration = out.columns() / 150.0;
    // The beat grid: .DAT is authoritative, then .EXT, then .2EX.
    for (const QStringList &files : {dat, ext, twoEx}) {
        if (files.isEmpty()) continue;
        if (auto pqtz = findTag(tagsOf(files.first()), "PQTZ")) out.grid = beatGrid(*pqtz);
        break;
    }
    out.originalBpm = originalBpm(out.grid);
    // Cues: every file; PCO2's richer entries (colours) win over PCOB's for the same cue.
    std::map<std::tuple<qint64, bool, int>, std::pair<bool, Cue>> found;
    for (const QStringList &files : {dat, ext, twoEx}) {
        for (const QString &path : files) {
            for (const Tag &tag : tagsOf(path)) {
                const bool rich = tag.type == "PCO2";
                for (const Cue &cue : cueList(tag)) {
                    const auto key = std::make_tuple(qint64(std::llround(cue.timeSec * 1000)), cue.memory, cue.number);
                    auto it = found.find(key);
                    if (it != found.end() && it->second.first && !rich) continue;
                    found[key] = {rich, cue};
                }
            }
        }
    }
    for (const auto &[key, item] : found) out.cues.push_back(item.second);
    std::sort(out.cues.begin(), out.cues.end(),
              [](const Cue &a, const Cue &b) { return std::tie(a.timeSec, a.number) < std::tie(b.timeSec, b.number); });
    return out;
}

}  // namespace wave
