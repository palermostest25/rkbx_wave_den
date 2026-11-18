// The AZ-style deck screen: one waveform lane per deck, scrolling past a fixed playhead with the beat
// grid, cues and loops, and the info bar below with each deck's track, tempo, time and overview.
// Frames are cheap: tiles are rendered once, scaled once to the lane, then only blitted.
#pragma once

#include "core/waveform.h"

#include <QHash>
#include <QPixmap>
#include <QWidget>

#include <list>
#include <map>

namespace wave {

class Session;
struct DeckView;

class Canvas : public QWidget {
    Q_OBJECT
public:
    explicit Canvas(Session *session, QWidget *parent = nullptr);
    void invalidate();                // colours or levels changed: draw the tiles again
    bool needsFrames() const { return needsFrames_; }  // a marquee or a warning is moving
    void frame(bool withInfo);        // the next frame: the lanes, and the info bar too if asked
    QRect lanesRect() const;

signals:
    void settingsRequested();
    void zoomBy(double factor);       // multiply the visible beats

protected:
    void paintEvent(QPaintEvent *) override;
    void mousePressEvent(QMouseEvent *) override;
    void mouseMoveEvent(QMouseEvent *) override;
    void mouseReleaseEvent(QMouseEvent *) override;
    void mouseDoubleClickEvent(QMouseEvent *) override;
    void wheelEvent(QWheelEvent *) override;
    bool event(QEvent *) override;
    void resizeEvent(QResizeEvent *) override;

private:
    double infoHeight(int decks) const;
    void drawDeck(QPainter &p, int idx, const QRectF &bounds);
    void drawInfo(QPainter &p, const QRectF &bounds, const std::vector<int> &order);
    void drawPanel(QPainter &p, int idx, const QRectF &r);
    bool previewAt(const QPointF &at, std::optional<int> deck = std::nullopt);
    const QPixmap &scaledTile(const DeckView &deck, int tile, int reduction, int width, int height);
    const QPixmap &overview(const DeckView &deck, const QSize &size);
    QImage cover(const QString &path);
    Palette palette() const;
    Levels levels() const;

    Session *session_;
    TileCache tiles_;
    struct Scaled { uint64_t generation; int tile, reduction, width, height; bool operator==(const Scaled &) const = default; };
    std::list<std::pair<Scaled, QPixmap>> scaled_;     // most recent at the front
    struct Ahead { std::shared_ptr<const Analysis> analysis; int tile, reduction; };
    std::vector<Ahead> upcoming_;
    std::map<uint64_t, std::pair<QSize, QPixmap>> overviews_;
    std::list<std::pair<QString, QImage>> covers_;
    std::map<int, QRectF> taps_, clocks_;               // deck -> its MT chip / big time readout
    std::map<int, std::pair<QRectF, double>> overviewsAt_;  // deck -> (overview, track length)
    std::optional<int> previewing_;
    bool needsFrames_ = false;
};

QString clockText(double seconds, bool tenths = true);
QString beatLabel(double beats);

}  // namespace wave
