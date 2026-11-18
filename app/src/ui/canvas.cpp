#include "ui/canvas.h"

#include "app/session.h"
#include "core/clock.h"

#include <QFileInfo>
#include <QMouseEvent>
#include <QNativeGestureEvent>
#include <QPainter>
#include <QPainterPath>

#include <cmath>

namespace wave {
namespace {

const QColor kDeckBlue("#1f8fff");
const QColor kLoopOrange("#ffa21a");
const QColor kWarnRed("#ff3b30");

QFont font(double size, bool bold = false) {
    QFont f(QStringLiteral("Arial"));
    f.setPixelSize(std::max(9, int(std::lround(size))));
    f.setBold(bold);
    f.setHintingPreference(QFont::PreferNoHinting);
    return f;
}

void rounded(QPainter &p, const QRectF &rect, double radius) {
    // A smooth rounded box, without antialiasing the waveform lines drawn around it.
    p.setRenderHint(QPainter::Antialiasing, true);
    p.drawRoundedRect(rect, radius, radius);
    p.setRenderHint(QPainter::Antialiasing, false);
}

void text(QPainter &p, const QRectF &rect, const QString &s, double size, const QColor &colour,
          Qt::Alignment align = Qt::AlignLeft, bool bold = false) {
    p.setFont(font(size, bold));
    p.setPen(colour);
    p.drawText(rect, align | Qt::AlignVCenter, p.fontMetrics().elidedText(s, Qt::ElideRight, std::max(1, int(rect.width()))));
}

// Text that fits sits still; longer text scrolls through, pausing at its start, as the AZ does.
void marquee(QPainter &p, const QRectF &rect, const QString &s, double size, const QColor &colour, bool bold, bool *moving) {
    p.setFont(font(size, bold));
    p.setPen(colour);
    const double width = p.fontMetrics().horizontalAdvance(s);
    if (width <= rect.width()) {
        p.drawText(rect, Qt::AlignLeft | Qt::AlignVCenter, s);
        return;
    }
    *moving = true;
    const double span = width + size * 3, pause = 2.0, speed = 40.0;
    const double t = std::fmod(now(), pause + span / speed), offset = std::max(0.0, t - pause) * speed;
    p.save();
    p.setClipRect(rect);
    for (double x : {rect.left() - offset, rect.left() - offset + span})
        p.drawText(QRectF(x, rect.top(), width + 2, rect.height()), Qt::AlignLeft | Qt::AlignVCenter, s);
    p.restore();
}

QRectF chip(QPainter &p, double x, double y, double h, const QString &s, const QColor &fill, const QColor &ink, bool alignRight = false) {
    p.setFont(font(h * .62, true));
    const double w = p.fontMetrics().horizontalAdvance(s) + h * .7;
    const QRectF box(alignRight ? x - w : x, y, w, h);
    p.setPen(Qt::NoPen);
    p.setBrush(fill);
    p.setRenderHint(QPainter::Antialiasing, true);
    p.drawRoundedRect(box, 3, 3);
    p.setRenderHint(QPainter::Antialiasing, false);
    p.setPen(ink);
    p.drawText(box, Qt::AlignCenter, s);
    return box;
}

// Place nearby cue labels without moving their exact-time stems: two rows, nearest free spot.
std::vector<std::optional<std::pair<double, int>>> flagPositions(const std::vector<double> &anchors, double width, double maximum) {
    std::vector<double> occupied[2];
    std::vector<std::optional<std::pair<double, int>>> result;
    for (double preferred : anchors) {
        std::optional<std::tuple<double, double, int>> best;
        for (int row = 0; row < 2; ++row) {
            std::vector<double> xs{std::max(0.0, std::min(preferred, maximum - width)), 0};
            for (double x : occupied[row]) xs.push_back(x + width + 2);
            for (double x : xs) {
                if (x + width > maximum) continue;
                bool free = true;
                for (double old : occupied[row]) if (!(x + width + 1 <= old || x >= old + width + 1)) { free = false; break; }
                if (!free) continue;
                const auto choice = std::make_tuple(std::abs(x - preferred) + row * width / 2, x, row);
                if (!best || choice < *best) best = choice;
            }
        }
        if (!best) { result.emplace_back(); continue; }
        occupied[std::get<2>(*best)].push_back(std::get<1>(*best));
        result.emplace_back(std::make_pair(std::get<1>(*best), std::get<2>(*best)));
    }
    return result;
}

std::optional<QString> countdown(double position, const std::vector<Cue> &cues, double bpm) {
    // Bars.beats to the next cue ahead, as Rekordbox counts down to it.
    std::optional<double> next;
    for (const Cue &c : cues) if (c.timeSec > position + .05 && (!next || c.timeSec < *next)) next = c.timeSec;
    if (!next || bpm <= 0) return std::nullopt;
    const int beats = int(std::ceil((*next - position) * bpm / 60 - 1e-6));
    return QStringLiteral("%1.%2").arg(beats / 4).arg(beats % 4);
}

const char *tempoRange(int range) {
    switch (range) {
    case 6: return "±6";
    case 10: return "±10";
    case 16: return "±16";
    case 100: return "WIDE";
    default: return nullptr;
    }
}

}  // namespace

QString clockText(double seconds, bool tenths) {
    seconds = std::max(0.0, seconds);
    const long whole = tenths ? long(seconds * 10) : long(seconds) * 10;
    QString s = QStringLiteral("%1:%2").arg(whole / 600, 2, 10, QChar('0')).arg(whole / 10 % 60, 2, 10, QChar('0'));
    return tenths ? s + QStringLiteral(".%1").arg(whole % 10) : s;
}

QString beatLabel(double beats) {
    // Loop length as Rekordbox shows it: 1/4, 1/2, 1, 2 … or 3.5 for a free manual loop.
    if (beats < .97) return QStringLiteral("1/%1").arg(std::lround(1 / beats));
    return std::abs(beats - std::round(beats)) < .05 ? QString::number(std::lround(beats)) : QString::number(beats, 'f', 1);
}

Canvas::Canvas(Session *session, QWidget *parent) : QWidget(parent), session_(session) {
    setMinimumSize(560, 280);
    setAttribute(Qt::WA_OpaquePaintEvent);
    setAccessibleName(QStringLiteral("Stacked deck waveforms"));
    setFocusPolicy(Qt::StrongFocus);
}

Palette Canvas::palette() const {
    const auto c = session_->settings.colours();
    return {c[0], c[1], c[2], c[3]};
}

Levels Canvas::levels() const {
    const auto g = session_->settings.gains();
    return {g[0], g[1], g[2], session_->settings.smoothing()};
}

void Canvas::invalidate() {
    tiles_.clear();
    scaled_.clear();
    overviews_.clear();
    update();
}

void Canvas::resizeEvent(QResizeEvent *) { scaled_.clear(); }

double Canvas::infoHeight(int decks) const {
    if (!session_->settings.flag("deck_info")) return 0;
    const double wanted = std::clamp(height() * .3, 150.0, 340.0) * session_->settings.uiScale() * session_->settings.number("info_size");
    // The waveforms come first: never squeeze a lane below a readable height.
    return std::max(0.0, std::min(wanted, height() - 70.0 * decks));
}

const QPixmap &Canvas::scaledTile(const DeckView &deck, int tile, int reduction, int width, int height) {
    const Scaled key{deck.generation, tile, reduction, width, height};
    for (auto it = scaled_.begin(); it != scaled_.end(); ++it)
        if (it->first == key) {
            scaled_.splice(scaled_.begin(), scaled_, it);
            return scaled_.front().second;
        }
    const QImage &image = tiles_.get(deck.analysis, tile, palette(), levels(), reduction);
    QPixmap pixmap = image.isNull() ? QPixmap() : QPixmap::fromImage(image.scaled(width, height, Qt::IgnoreAspectRatio, Qt::SmoothTransformation));
    scaled_.emplace_front(key, std::move(pixmap));
    while (scaled_.size() > 40) scaled_.pop_back();
    return scaled_.front().second;
}

void Canvas::paintEvent(QPaintEvent *) {
    QPainter p(this);
    needsFrames_ = false;
    p.fillRect(rect(), Qt::black);
    const auto order = session_->order();
    const double info = infoHeight(int(order.size()));
    const double lane = (height() - info) / order.size();
    upcoming_.clear();
    for (size_t row = 0; row < order.size(); ++row) drawDeck(p, order[row], QRectF(0, row * lane, width(), lane));
    if (info > 0) drawInfo(p, QRectF(0, height() - info, width(), info), order);
    p.end();
    // Render one tile ahead per frame (a couple of milliseconds), so four decks never fill a frame at once.
    for (const Ahead &a : upcoming_)
        if (a.tile >= 0 && a.tile * kTileColumns < a.analysis->columns() && !tiles_.cached(a.analysis.get(), a.tile, palette(), levels(), a.reduction)) {
            tiles_.get(a.analysis, a.tile, palette(), levels(), a.reduction);
            break;
        }
}

void Canvas::drawDeck(QPainter &p, int idx, const QRectF &bounds) {
    const Settings &s = session_->settings;
    const DeckView &deck = session_->deck(idx);
    const double scale = s.uiScale(), laneH = bounds.height();
    const double fontSize = std::clamp(laneH * .075, 14.0, 23.0) * scale;
    const QFont base = font(fontSize);
    p.setFont(base);
    const double rail = std::max(54.0, fontSize * 2.7);
    p.fillRect(QRectF(0, bounds.top(), rail - 7, bounds.height()), QColor("#101010"));
    // Lit while the deck plays, like the deck numbers on the AZ's enlarged waveforms.
    const bool playing = session_->playing(idx);
    const QRectF badge(7, bounds.top() + 14, rail - 21, bounds.height() - 28);
    p.setBrush(playing ? QBrush(kDeckBlue) : Qt::NoBrush);
    p.setPen(QPen(playing ? kDeckBlue : QColor("#5b636d"), 1));
    rounded(p, badge, 6);
    p.setFont(font(fontSize * 1.5, true));
    p.setPen(playing ? Qt::white : QColor("#d6dade"));
    p.drawText(badge, Qt::AlignCenter, QString::number(idx + 1));
    p.setFont(base);
    if (bounds.top() > 0) {
        p.setPen(QColor("#252525"));
        p.drawLine(QPointF(0, bounds.top()), QPointF(bounds.right(), bounds.top()));
    }
    const double flagH = std::max(19.0, fontSize + 4);
    const QRectF signal(rail, bounds.top() + flagH + 3, std::max(1.0, bounds.width() - rail - 8), std::max(1.0, bounds.height() - 2 * flagH - 14));
    const auto live = session_->position(idx);
    const auto preview = live ? session_->previewPosition(idx) : std::nullopt;
    const auto position = preview ? preview : live;
    if (!deck.analysis || !position) {
        QString waiting = deck.error;
        if (waiting.isEmpty()) waiting = session_->connected() || session_->demo() ? QStringLiteral("Waiting for a track in Rekordbox") : session_->status();
        if (waiting.isEmpty()) waiting = QStringLiteral("Waiting for a track in Rekordbox");
        p.setPen(QColor("#7d858f"));
        p.drawText(signal.adjusted(24, 0, -24, 0), Qt::AlignCenter | Qt::TextWordWrap, waiting);
        return;
    }
    const Analysis &analysis = *deck.analysis;
    const double bpm = deck.originalBpm > 0 ? deck.originalBpm : deck.currentBpm > 0 ? deck.currentBpm : 120.0;
    const double visible = session_->zoomBeats() * 60 / bpm;
    const double start = *position - visible * s.number("playhead"), end = start + visible;
    const double pps = signal.width() / visible;
    auto xAt = [&](double seconds) { return signal.left() + (seconds - start) * pps; };
    p.save();
    p.setClipRect(signal);
    const double duration = analysis.duration;
    const int bins = analysis.columns();
    if (duration > 0 && bins) {
        const double binRate = bins / duration;
        const int first = std::max(0, int(std::floor(start * binRate / kTileColumns)));
        const int last = std::min(int(std::ceil(double(bins) / kTileColumns)) - 1, int(std::floor(end * binRate / kTileColumns)));
        const int reduction = 1 << std::max(0, int(std::floor(std::log2(binRate / std::max(pps, .001)))));
        upcoming_.push_back({deck.analysis, last + 1, reduction});
        if (first > 0) upcoming_.push_back({deck.analysis, first - 1, reduction});
        const int tileH = int(std::lround(signal.height()));
        const int fullW = int(std::ceil(kTileColumns / binRate * pps)) + 1;  // a pixel of overlap, never a gap
        for (int tile = first; tile <= last; ++tile) {
            const int columns = std::min(kTileColumns, bins - tile * kTileColumns);
            const int w = columns == kTileColumns ? fullW : int(std::ceil(columns / binRate * pps));
            const QPixmap &pixmap = scaledTile(deck, tile, reduction, std::max(1, w), std::max(1, tileH));
            if (!pixmap.isNull()) p.drawPixmap(QPointF(std::round(xAt(tile * kTileColumns / binRate)), std::round(signal.top())), pixmap);
        }
    }
    const auto &timeline = deck.timeline;
    const auto loop = session_->demo() ? std::optional<Loop>() : timeline.activeLoop();
    if (s.flag("loops")) {
        // Saved loops tint their span in the cue colour, as on the decks.
        for (const Cue &c : deck.cues) {
            if (!c.endSec || *c.endSec < start || c.timeSec > end) continue;
            QColor tint = c.color;
            tint.setAlpha(55);
            p.fillRect(QRectF(xAt(c.timeSec), signal.top(), xAt(*c.endSec) - xAt(c.timeSec), signal.height()), tint);
        }
        const QFont pillFont = font(fontSize * .72, true);
        auto pill = [&](double x, const QString &label) {
            p.setFont(pillFont);
            const double w = p.fontMetrics().horizontalAdvance(label) + 12;
            const QRectF box(std::min(std::max(x, signal.left() + 2), signal.right() - w - 2), signal.top() + 4, w, pillFont.pixelSize() + 6);
            p.setPen(Qt::NoPen);
            p.setBrush(kLoopOrange);
            rounded(p, box, 4);
            p.setPen(Qt::black);
            p.drawText(box, Qt::AlignCenter, label);
            p.setFont(base);
        };
        if (loop) {
            const double left = xAt(loop->first), right = xAt(loop->second);
            const QRectF r(left, signal.top() + 1, right - left, signal.height() - 2);
            p.fillRect(r, QColor(255, 143, 0, 62));
            p.setBrush(Qt::NoBrush);
            p.setPen(QPen(kLoopOrange, 1.5));
            p.drawRect(r);
            p.setPen(QPen(kLoopOrange, 3));
            p.drawLine(QPointF(left, r.top()), QPointF(left, r.bottom()));
            p.drawLine(QPointF(right, r.top()), QPointF(right, r.bottom()));
            pill(left + 4, beatLabel((loop->second - loop->first) * bpm / 60));
        } else if (timeline.pendingIn && *timeline.pendingIn <= end) {
            const double left = xAt(*timeline.pendingIn);
            if (*position > *timeline.pendingIn)
                p.fillRect(QRectF(left, signal.top() + 1, xAt(*position) - left, signal.height() - 2), QColor(255, 143, 0, 34));
            p.setPen(QPen(kLoopOrange, 3));
            p.drawLine(QPointF(left, signal.top()), QPointF(left, signal.bottom()));
            pill(left + 4, QStringLiteral("IN"));
        }
    }
    p.restore();
    // Grid ticks sit outside the waveform, as on the AZ's enlarged display.
    if (s.flag("grid")) {
        const QPen down(QColor("#f52620"), 1), beat(QColor("#b7bcc1"), 1);
        const auto &grid = analysis.grid;
        // The grid is in time order: start from the first beat in view.
        auto it = std::lower_bound(grid.begin(), grid.end(), start * 1000, [](const Beat &b, double ms) { return b.timeMs < ms; });
        for (; it != grid.end() && it->timeMs / 1000 <= end; ++it) {
            const int number = int(it - grid.begin());
            const double x = xAt(it->timeMs / 1000);
            const bool downbeat = it->number == 1;
            p.setPen(downbeat ? down : beat);
            const double tick = downbeat ? 13 : 8;
            p.drawLine(QPointF(x, signal.top() - tick - 2), QPointF(x, signal.top() - 2));
            p.drawLine(QPointF(x, signal.bottom() + 2), QPointF(x, signal.bottom() + tick + 2));
            if (downbeat && number / 4 % 4 == 3 && s.flag("bar_numbers")) {
                p.setPen(QColor("#efefef"));
                p.drawText(QRectF(x - 15, signal.bottom() + 11, 40, flagH), Qt::AlignCenter, QString::number(number / 4 + 1));
            }
        }
    }
    if (s.flag("cues")) {
        std::vector<const Cue *> cues;
        std::vector<double> anchors;
        for (const Cue &c : deck.cues) if (start <= c.timeSec && c.timeSec <= end) { cues.push_back(&c); anchors.push_back(xAt(c.timeSec) - signal.left()); }
        const double fw = std::max(24.0, fontSize * 1.35);
        const auto places = flagPositions(anchors, fw, signal.width());
        for (size_t i = 0; i < cues.size(); ++i) {
            const Cue &c = *cues[i];
            const double x = xAt(c.timeSec);
            p.setPen(QPen(c.color, 2));  // the stem is always at the real cue position
            p.drawLine(QPointF(x, signal.top()), QPointF(x, signal.bottom()));
            if (!places[i]) continue;
            const double fx = places[i]->first + signal.left();
            const double top = signal.top() - flagH - 3 + places[i]->second * flagH;
            p.setPen(Qt::NoPen);
            p.setBrush(c.color);
            const QPointF flag[] = {{fx, top}, {fx + fw, top}, {fx + fw, top + flagH - 5}, {fx + 5, top + flagH - 5}, {fx, top + flagH}};
            p.drawPolygon(flag, 5);
            p.setPen(c.color.lightness() > 100 ? Qt::black : Qt::white);
            const QString label = c.memory ? QStringLiteral("M") : c.number >= 1 && c.number <= 16 ? QString(QChar('A' + c.number - 1)) : QStringLiteral("C");
            p.drawText(QRectF(fx, top - 1, fw, flagH - 3), Qt::AlignCenter, label);
        }
        if (s.flag("loops"))
            for (const Cue &c : deck.cues) {
                if (!c.endSec || *c.endSec < start || c.timeSec > end) continue;
                const double left = std::max(signal.left(), xAt(c.timeSec)), right = std::min(signal.right(), xAt(*c.endSec));
                const double y = signal.bottom() + 4;
                p.setPen(QPen(c.color, 3));
                p.drawLine(QPointF(left, y), QPointF(right, y));
                p.drawLine(QPointF(left, y - 4), QPointF(left, y + 5));
                p.drawLine(QPointF(right, y - 4), QPointF(right, y + 5));
            }
    }
    // A white playhead, distinct from red first-beat ticks; previewing, a blue one marks the look.
    const double x = std::round(xAt(*position));
    p.setPen(QPen(QColor("#060606"), 5));
    p.drawLine(QPointF(x, bounds.top() + 2), QPointF(x, bounds.bottom() - 4));
    p.setPen(QPen(preview ? kDeckBlue : QColor("#f8f8f8"), 2.2));
    p.drawLine(QPointF(x, bounds.top() + 2), QPointF(x, bounds.bottom() - 4));
    if (preview) {
        if (start <= *live && *live <= end) {  // the deck's real position, when it is in view
            p.setPen(QPen(QColor("#f8f8f8"), 2.2));
            p.drawLine(QPointF(xAt(*live), bounds.top() + 2), QPointF(xAt(*live), bounds.bottom() - 4));
        }
        const QRectF tag(signal.left() + 8, bounds.top() + 6, fontSize * 9.5, flagH + 2);
        p.setPen(Qt::NoPen);
        p.setBrush(kDeckBlue);
        rounded(p, tag, 4);
        p.setPen(Qt::white);
        p.drawText(tag, Qt::AlignCenter, QStringLiteral("PREVIEW  %1").arg(clockText(*preview, false)));
    }
}

const QPixmap &Canvas::overview(const DeckView &deck, const QSize &size) {
    auto &entry = overviews_[deck.generation];
    if (entry.first == size && !entry.second.isNull()) return entry.second;
    // Averaged, as an overview should read: the loudest column per pixel would paint it all white.
    const Analysis &a = *deck.analysis;
    const int width = std::max(1, std::min(1600, a.columns()));
    std::vector<float> low(width), mid(width), high(width);
    const auto g = session_->settings.gains();
    for (int i = 0; i < width; ++i) {
        const int from = int(std::floor(double(i) * a.columns() / width)), to = std::max(from + 1, int(std::floor(double(i + 1) * a.columns() / width)));
        double l = 0, m = 0, h = 0;
        for (int j = from; j < to && j < a.columns(); ++j) { l += a.low[j]; m += a.mid[j]; h += a.high[j]; }
        const int n = std::max(1, std::min(to, a.columns()) - from);
        low[i] = float(l / n) * g[0]; mid[i] = float(m / n) * g[1]; high[i] = float(h / n) * g[2];
    }
    const QImage image = renderBands(low.data(), mid.data(), high.data(), width, width, 96, 1, palette());
    entry = {size, QPixmap::fromImage(image.scaled(size, Qt::IgnoreAspectRatio, Qt::SmoothTransformation))};
    while (overviews_.size() > 8) overviews_.erase(overviews_.begin());
    return entry.second;
}

QImage Canvas::cover(const QString &path) {
    for (auto it = covers_.begin(); it != covers_.end(); ++it)
        if (it->first == path) return it->second;
    covers_.emplace_front(path, QImage(path));
    while (covers_.size() > 16) covers_.pop_back();
    return covers_.front().second;
}

void Canvas::drawInfo(QPainter &p, const QRectF &bounds, const std::vector<int> &order) {
    taps_.clear();
    clocks_.clear();
    overviewsAt_.clear();
    p.fillRect(bounds, QColor("#0b0c0e"));
    p.setPen(QColor("#2a2e33"));
    p.drawLine(bounds.topLeft(), bounds.topRight());
    const double w = bounds.width() / order.size();
    for (size_t column = 0; column < order.size(); ++column)
        drawPanel(p, order[column], QRectF(bounds.left() + column * w, bounds.top(), w, bounds.height()).adjusted(4, 7, -4, -6));
}

void Canvas::drawPanel(QPainter &p, int idx, const QRectF &r) {
    const Settings &s = session_->settings;
    const DeckView &deck = session_->deck(idx);
    const bool playing = session_->playing(idx);
    const auto pos = session_->position(idx);
    const bool loaded = deck.analysis && pos;
    const double position = pos.value_or(0);
    p.setPen(QPen(QColor("#24282d"), 1));
    p.setBrush(QColor("#131518"));
    rounded(p, r, 10);
    const double pad = std::max(5.0, r.height() * .035);
    const QRectF inner = r.adjusted(pad, pad, -pad, -pad);
    const double gap = std::max(4.0, pad * .8), h = inner.height();
    const double line = h * .13, big = h * .27, info = h * .13;
    const double top = inner.top();
    bool moving = false;
    // Deck number, lit while playing, beside the title and artist.
    const QRectF badge(inner.left(), top, 2 * line, 2 * line);
    p.setPen(QPen(playing ? kDeckBlue : QColor("#5b636d"), 1));
    p.setBrush(playing ? QBrush(kDeckBlue) : Qt::NoBrush);
    rounded(p, badge, 6);
    text(p, badge, QString::number(idx + 1), line * 1.3, playing ? Qt::white : QColor("#d6dade"), Qt::AlignCenter, true);
    const double textX = badge.right() + gap;
    double textW = inner.right() - textX;
    if (!loaded) {
        text(p, QRectF(textX, top, textW, line), QStringLiteral("No track loaded"), line * .7, QColor("#7d858e"));
        if (!deck.error.isEmpty()) text(p, QRectF(textX, top + line, textW, line), deck.error, line * .55, QColor("#9aa3ad"));
    } else {
        // MASTER, SYNC and key lock, dim until Rekordbox reports them on.
        if (deck.status) {
            const double chipH = line * .78;
            double right = inner.right();
            const struct { const char *label; bool on; QColor fill, ink; } chips[] = {
                {"MT", deck.status->masterTempo, QColor("#d63a3a"), Qt::white},
                {"SYNC", deck.status->sync, kDeckBlue, Qt::white},
                {"MASTER", deck.status->master, kLoopOrange, Qt::black}};
            for (const auto &c : chips) {
                const QRectF box = chip(p, right, top + (line - chipH) / 2, chipH, QString::fromLatin1(c.label),
                                        c.on ? c.fill : QColor("#1c1f23"), c.on ? c.ink : QColor("#4f565e"), true);
                if (QLatin1String(c.label) == QLatin1String("MT")) taps_[idx] = box;
                right = box.left() - gap * .6;
            }
            textW = right - gap - textX;
        }
        QString title = deck.meta.title;
        if (title.isEmpty()) title = QFileInfo(deck.meta.path).completeBaseName();
        marquee(p, QRectF(textX, top, textW, line), title.isEmpty() ? QStringLiteral("Unknown") : title, line * .72, QColor("#f1f3f5"), true, &moving);
        marquee(p, QRectF(textX, top + line, inner.right() - textX, line), deck.meta.artist, line * .6, QColor("#9aa3ad"), false, &moving);
    }
    const double row = top + 2 * line + gap;
    // Artwork (or an empty sleeve) with the play state on it.
    const QRectF art(inner.left(), row, big, big);
    const QImage image = loaded && !deck.meta.artwork.isEmpty() && s.flag("artwork") ? cover(deck.meta.artwork) : QImage();
    if (!image.isNull()) {
        p.setRenderHint(QPainter::SmoothPixmapTransform, true);
        p.drawImage(art, image);
        p.setRenderHint(QPainter::SmoothPixmapTransform, false);
    } else {
        p.fillRect(art, QColor("#1c1f23"));
        p.setPen(QPen(QColor("#2c3137"), 1));
        p.setBrush(Qt::NoBrush);
        p.setRenderHint(QPainter::Antialiasing, true);
        p.drawEllipse(art.center(), big * .3, big * .3);
        p.drawEllipse(art.center(), big * .06, big * .06);
        p.setRenderHint(QPainter::Antialiasing, false);
    }
    if (loaded) {
        const double g = big * .34;
        const QRectF glyph(art.left() + 3, art.bottom() - g - 3, g, g);
        p.setPen(Qt::NoPen);
        p.setBrush(QColor(0, 0, 0, 170));
        p.drawRect(glyph);
        p.setBrush(playing ? QColor("#34c759") : QColor("#c9ced4"));
        p.setRenderHint(QPainter::Antialiasing, true);
        if (playing) {
            const QPointF tri[] = {{glyph.left() + g * .32, glyph.top() + g * .22}, {glyph.left() + g * .32, glyph.bottom() - g * .22},
                                   {glyph.right() - g * .22, glyph.center().y()}};
            p.drawPolygon(tri, 3);
        } else {
            for (double x : {.28, .56}) p.drawRect(QRectF(glyph.left() + g * x, glyph.top() + g * .24, g * .16, g * .52));
        }
        p.setRenderHint(QPainter::Antialiasing, false);
    }
    const double duration = loaded ? deck.analysis->duration : 0;
    const double remaining = loaded ? std::max(0.0, duration - position) : 0;
    const bool warn = loaded && duration > 0 && remaining < s.number("end_warning");
    const bool blink = warn && playing && std::fmod(now(), 1.0) < .5;
    const double details = row + big + gap * .5;
    if (loaded) {
        const double original = deck.originalBpm, current = deck.currentBpm > 0 ? deck.currentBpm : deck.originalBpm;
        const double bpmW = std::min(inner.width() * .36, big * 2.6), label = big * .26;
        const double timeX = art.right() + gap * 1.5;
        const bool elapsed = s.text("time_mode") == QLatin1String("elapsed");
        text(p, QRectF(timeX, row, inner.width(), label), elapsed ? QStringLiteral("ELAPSED") : QStringLiteral("REMAIN"), label * .8, QColor("#7d858e"));
        clocks_[idx] = QRectF(timeX, row, inner.right() - bpmW - timeX, big);
        text(p, QRectF(timeX, row + label, inner.right() - bpmW - timeX, big - label), elapsed ? clockText(position) : '-' + clockText(remaining),
             std::min((big - label) * .9, (inner.right() - bpmW - timeX) / 4.3), warn ? kWarnRed : Qt::white, Qt::AlignLeft, true);
        text(p, QRectF(inner.right() - bpmW, row, bpmW, label), QStringLiteral("BPM"), label * .8, QColor("#7d858e"), Qt::AlignRight);
        text(p, QRectF(inner.right() - bpmW, row + label, bpmW, big - label), current > 0 ? QString::number(current, 'f', 1) : QStringLiteral("--"),
             std::min((big - label) * .9, bpmW / 3.2), Qt::white, Qt::AlignRight, true);
        // Elapsed, key and tempo at the left; loop and countdown to the next cue at the right.
        double x = inner.left();
        text(p, QRectF(x, details, info * 4.6, info), elapsed ? '-' + clockText(remaining) : clockText(position), info * .62, QColor("#9aa3ad"));
        x += info * 4.6;
        const double chipH = info * .82, y = details + (info - chipH) / 2;
        const int shift = deck.status ? deck.status->keyShift : 0;
        if (!deck.meta.key.isEmpty())
            x = chip(p, x, y, chipH, deck.meta.key + (shift ? QStringLiteral(" %1%2").arg(shift > 0 ? "+" : "").arg(shift) : QString()),
                     QColor("#1f3a2a"), QColor("#7ee2a8")).right() + gap;
        if (original > 0 && current > 0) {
            const double pct = (current / original - 1) * 100;
            const QString pctText = QStringLiteral("%1%2%").arg(pct >= 0 ? "+" : "").arg(pct, 0, 'f', 2);
            text(p, QRectF(x, details, info * 4, info), pctText, info * .62, std::abs(pct) >= .005 ? QColor("#f1f3f5") : QColor("#7d858e"));
            p.setFont(font(info * .62));
            x += p.fontMetrics().horizontalAdvance(pctText) + gap;
        }
        if (deck.status) if (const char *range = tempoRange(deck.status->tempoRange))
            x = chip(p, x, y, chipH, QString::fromUtf8(range), QColor("#22262c"), QColor("#c9ced4")).right() + gap;
        double right = inner.right();
        const auto loop = deck.timeline.activeLoop();
        if (loop && original > 0)
            right = chip(p, right, y, chipH, "LOOP " + beatLabel((loop->second - loop->first) * original / 60), kLoopOrange, Qt::black, true).left() - gap;
        if (const auto bars = countdown(position, deck.cues, original))
            text(p, QRectF(right - info * 5, details, info * 5, info), *bars + QStringLiteral(" Bars"), info * .62, QColor("#d6dade"), Qt::AlignRight);
    }
    const QRectF ov(inner.left(), details + info + gap * .5, inner.width(), inner.bottom() - details - info - gap * .5);
    // Whole-track overview: played part dimmed, cues above and below, the playhead white.
    p.fillRect(ov, QColor("#050506"));
    if (loaded && duration > 0 && ov.width() >= 1 && ov.height() >= 1) {
        overviewsAt_[idx] = {ov, duration};
        p.drawPixmap(ov.topLeft(), overview(deck, ov.size().toSize()));
        auto xAt = [&](double t) { return ov.left() + std::clamp(t / duration, 0.0, 1.0) * ov.width(); };
        p.fillRect(QRectF(ov.left(), ov.top(), xAt(position) - ov.left(), ov.height()), QColor(0, 0, 0, 135));
        for (const Cue &c : deck.cues)
            if (c.endSec) {
                QColor tint = c.color;
                tint.setAlpha(70);
                p.fillRect(QRectF(xAt(c.timeSec), ov.top(), xAt(*c.endSec) - xAt(c.timeSec), ov.height()), tint);
            }
        if (const auto loop = deck.timeline.activeLoop())
            p.fillRect(QRectF(xAt(loop->first), ov.top(), std::max(2.0, xAt(loop->second) - xAt(loop->first)), ov.height()), QColor(255, 143, 0, 110));
        p.setPen(QPen(QColor("#4a5058"), 1));  // minute ticks along the bottom edge
        for (int minute = 1; minute <= int(duration / 60); ++minute) {
            const double x = xAt(minute * 60);
            p.drawLine(QPointF(x, ov.bottom() - 4), QPointF(x, ov.bottom()));
        }
        const double mark = std::max(5.0, ov.height() * .16);
        p.setRenderHint(QPainter::Antialiasing, true);
        for (const Cue &c : deck.cues) {
            const double x = xAt(c.timeSec);
            p.setPen(Qt::NoPen);
            p.setBrush(c.color);
            if (c.memory) {
                const QPointF tri[] = {{x - mark * .6, ov.bottom()}, {x + mark * .6, ov.bottom()}, {x, ov.bottom() - mark}};
                p.drawPolygon(tri, 3);
            } else {
                const QPointF tri[] = {{x - mark * .6, ov.top()}, {x + mark * .6, ov.top()}, {x, ov.top() + mark}};
                p.drawPolygon(tri, 3);
            }
        }
        p.setRenderHint(QPainter::Antialiasing, false);
        const double x = xAt(position);
        p.setPen(QPen(QColor("#060606"), 4));
        p.drawLine(QPointF(x, ov.top()), QPointF(x, ov.bottom()));
        p.setPen(QPen(Qt::white, 2));
        p.drawLine(QPointF(x, ov.top()), QPointF(x, ov.bottom()));
        if (const auto looking = session_->previewPosition(idx)) {  // where the lane above is showing
            p.setPen(QPen(kDeckBlue, 2, Qt::DashLine));
            p.drawLine(QPointF(xAt(*looking), ov.top()), QPointF(xAt(*looking), ov.bottom()));
        }
    }
    p.setBrush(Qt::NoBrush);
    p.setPen(QPen(blink ? kWarnRed : QColor("#2a2e33"), blink ? 2 : 1));
    p.drawRect(ov);
    // A long title scrolling or the end warning blinking needs frames even while nothing plays.
    if (moving || (warn && playing)) needsFrames_ = true;
}

bool Canvas::previewAt(const QPointF &at, std::optional<int> deck) {
    // A touch on a deck's whole-track overview: show that part of the track in its lane.
    for (const auto &[idx, entry] : overviewsAt_) {
        const auto &[box, duration] = entry;
        if ((!deck && box.adjusted(0, -6, 0, 6).contains(at)) || deck == idx) {
            const double fraction = std::clamp((at.x() - box.left()) / std::max(1.0, box.width()), 0.0, 1.0);
            session_->previewHold(idx, fraction * duration);
            previewing_ = idx;
            update();
            return true;
        }
    }
    return false;
}

void Canvas::mousePressEvent(QMouseEvent *e) {
    const QPointF at = e->position();
    for (const auto &[idx, box] : taps_)
        if (box.adjusted(-8, -8, 8, 8).contains(at)) {  // a fingertip, not a cursor
            session_->toggleMasterTempo(idx);
            return;
        }
    if (previewAt(at)) return;
    for (const auto &[idx, box] : clocks_)
        if (box.contains(at)) {
            session_->settings.setView("time_mode", session_->settings.text("time_mode") == QLatin1String("elapsed") ? "remain" : "elapsed");
            session_->save();
            update();
            return;
        }
    QWidget::mousePressEvent(e);
}

void Canvas::mouseMoveEvent(QMouseEvent *e) {
    if (e->buttons() && previewing_) previewAt(e->position(), previewing_);
    QWidget::mouseMoveEvent(e);
}

void Canvas::mouseReleaseEvent(QMouseEvent *e) {
    if (previewing_) {
        session_->previewRelease(*previewing_);
        previewing_.reset();
    }
    QWidget::mouseReleaseEvent(e);
}

void Canvas::mouseDoubleClickEvent(QMouseEvent *e) {
    if (previewing_) return;
    for (const auto &[idx, entry] : overviewsAt_)
        if (entry.first.contains(e->position())) return;  // a quick second tap on the overview is another look
    emit settingsRequested();
}

void Canvas::wheelEvent(QWheelEvent *e) {
    // Two-finger scroll or a mouse wheel: up zooms in, a notch is a quarter of a doubling.
    if (const int delta = e->angleDelta().y()) emit zoomBy(std::pow(2.0, -delta / 480.0));
    e->accept();
}

bool Canvas::event(QEvent *e) {
    // Trackpad pinch (macOS): spreading the fingers zooms in.
    if (e->type() == QEvent::NativeGesture) {
        auto *g = static_cast<QNativeGestureEvent *>(e);
        if (g->gestureType() == Qt::ZoomNativeGesture) {
            emit zoomBy(1 / std::max(.2, 1 + g->value()));
            return true;
        }
    }
    return QWidget::event(e);
}

}  // namespace wave
