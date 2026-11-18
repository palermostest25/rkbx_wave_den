#include "ui/browser.h"

#include "app/gig.h"
#include "core/clock.h"
#include "ui/widgets.h"

#include <QAbstractListModel>
#include <QDateTime>
#include <QDir>
#include <QFileInfo>
#include <QFutureWatcher>
#include <QGridLayout>
#include <QHeaderView>
#include <QLabel>
#include <QLineEdit>
#include <QListView>
#include <QPainter>
#include <QPainterPath>
#include <QScroller>
#include <QStackedWidget>
#include <QStyledItemDelegate>
#include <QTableView>
#include <QTimer>
#include <QVBoxLayout>
#include <QtConcurrent>

#include <list>

namespace wave {

constexpr int kPage = 200;
constexpr double kDoubleTap = .5;  // seconds between the two taps that open a playlist
const QColor kBlue(kAccent);

namespace {

QFont arial(double size, bool bold = false) {
    QFont f(QStringLiteral("Arial"));
    f.setPixelSize(std::max(9, int(std::lround(size))));
    f.setBold(bold);
    return f;
}

QString minutes(double seconds) {
    const int s = int(std::max(0.0, seconds));
    return QStringLiteral("%1:%2").arg(s / 60, 2, 10, QChar('0')).arg(s % 60, 2, 10, QChar('0'));
}

QString folded(const QString &path) { return QDir::fromNativeSeparators(path).toCaseFolded(); }

// Drag to scroll with momentum, for a finger or a mouse alike. A press becomes a tap only if the finger
// stays put, so a scroll never opens or picks what it started on.
void kinetic(QAbstractItemView *view) {
    QScroller::grabGesture(view->viewport(), QScroller::LeftMouseButtonGesture);
    QScroller *scroller = QScroller::scroller(view->viewport());
    QScrollerProperties p = scroller->scrollerProperties();
    p.setScrollMetric(QScrollerProperties::DragStartDistance, .003);
    p.setScrollMetric(QScrollerProperties::MousePressEventDelay, .1);
    p.setScrollMetric(QScrollerProperties::VerticalOvershootPolicy, QScrollerProperties::OvershootAlwaysOff);
    p.setScrollMetric(QScrollerProperties::HorizontalOvershootPolicy, QScrollerProperties::OvershootAlwaysOff);
    scroller->setScrollerProperties(p);
}

void sleeve(QPainter &p, const QRectF &r, const QImage &image) {
    if (!image.isNull()) { p.drawImage(r, image); return; }
    p.fillRect(r, QColor("#1c1f23"));
    p.setPen(QPen(QColor("#30353b"), 1));
    p.setBrush(Qt::NoBrush);
    p.drawEllipse(r.center(), r.width() * .32, r.width() * .32);
    p.drawEllipse(r.center(), r.width() * .07, r.width() * .07);
}

}  // namespace

// Each source's library, opened once on the worker and reopened when its file changes.
struct CatalogCache {
    struct Open { qint64 modified = 0; std::unique_ptr<Library> library; };
    std::map<QString, Open> open;
    Libraries libraries;
    Library *get(const QString &source, QString *error) {
        const QString file = source.isEmpty() ? masterDatabase() : source;
        const qint64 modified = QFileInfo(file).lastModified().toMSecsSinceEpoch();
        Open &o = open[source];
        if (!o.library || o.modified != modified) {
            o.library = Library::open(source, error);
            o.modified = modified;
        }
        return o.library.get();
    }
};

// A small cache of decoded sleeves, shared by rows and the load bar.
class Artwork {
public:
    QImage get(const QString &path) {
        if (path.isEmpty()) return {};
        for (auto it = images_.begin(); it != images_.end(); ++it)
            if (it->first == path) { images_.splice(images_.begin(), images_, it); return images_.front().second; }
        QImage image(path);
        if (!image.isNull()) image = image.scaled(96, 96, Qt::KeepAspectRatioByExpanding, Qt::SmoothTransformation);
        images_.emplace_front(path, image);
        while (images_.size() > 400) images_.pop_back();
        return image;
    }
private:
    std::list<std::pair<QString, QImage>> images_;
};

class TrackModel : public QAbstractTableModel {
public:
    using QAbstractTableModel::QAbstractTableModel;
    std::vector<CatalogTrack> tracks;
    int total = 0;
    std::map<QString, std::vector<int>> loaded;  // file (folded) -> deck numbers it is on
    std::function<void()> moreWanted;
    int rowCount(const QModelIndex &p = {}) const override { return p.isValid() ? 0 : int(tracks.size()); }
    int columnCount(const QModelIndex &p = {}) const override { return p.isValid() ? 0 : 5; }
    QVariant data(const QModelIndex &i, int role) const override {
        if (!i.isValid() || i.row() >= int(tracks.size())) return {};
        const CatalogTrack &t = tracks[size_t(i.row())];
        if (role == Qt::DisplayRole) {
            switch (i.column()) {
            case 1: return t.title.isEmpty() ? QStringLiteral("Untitled") : t.title;
            case 2: return t.bpm > 0 ? QString::number(t.bpm, 'f', 2) : QStringLiteral("—");
            case 3: return t.key.isEmpty() ? QStringLiteral("—") : t.key;
            case 4: return minutes(t.duration);
            default: return QString();
            }
        }
        if (role == Qt::ToolTipRole || role == Qt::AccessibleTextRole) return t.title + QStringLiteral(" — ") + t.artist;
        return {};
    }
    QVariant headerData(int section, Qt::Orientation o, int role) const override {
        static const char *names[] = {"", "TITLE", "BPM", "KEY", "TIME"};
        return o == Qt::Horizontal && role == Qt::DisplayRole ? QString::fromLatin1(names[section]) : QVariant();
    }
    void replace(std::vector<CatalogTrack> rows, int count) { beginResetModel(); tracks = std::move(rows); total = count; endResetModel(); }
    void append(const std::vector<CatalogTrack> &rows) {
        if (rows.empty()) return;
        beginInsertRows({}, int(tracks.size()), int(tracks.size() + rows.size()) - 1);
        tracks.insert(tracks.end(), rows.begin(), rows.end());
        endInsertRows();
    }
    bool canFetchMore(const QModelIndex &p) const override { return !p.isValid() && int(tracks.size()) < total; }
    void fetchMore(const QModelIndex &) override { if (moreWanted) moreWanted(); }
    std::vector<int> decksFor(const CatalogTrack &t) const {
        const auto it = t.path.isEmpty() ? loaded.end() : loaded.find(folded(t.path));
        return it == loaded.end() ? std::vector<int>{} : it->second;
    }
};

class TrackDelegate : public QStyledItemDelegate {
public:
    TrackDelegate(std::shared_ptr<Artwork> artwork, TrackModel *model, QObject *parent)
        : QStyledItemDelegate(parent), artwork_(std::move(artwork)), model_(model) {}
    void paint(QPainter *p, const QStyleOptionViewItem &option, const QModelIndex &index) const override {
        if (index.row() >= int(model_->tracks.size())) return;
        const CatalogTrack &t = model_->tracks[size_t(index.row())];
        const QRectF r(option.rect);
        p->save();
        p->setRenderHint(QPainter::Antialiasing);
        p->setRenderHint(QPainter::SmoothPixmapTransform);
        const bool selected = option.state & QStyle::State_Selected;
        p->fillRect(r, selected ? kBlue : QColor(index.row() % 2 ? "#121212" : "#0c0c0c"));
        switch (index.column()) {
        case 0: {
            const double side = r.height() - 10;
            sleeve(*p, QRectF(r.left() + 8, r.top() + 5, side, side), artwork_->get(t.artwork));
            break;
        }
        case 1: {
            double right = r.right() - 8;
            auto decks = model_->decksFor(t);
            std::sort(decks.rbegin(), decks.rend());
            for (int deck : decks) {  // the decks this track is on now
                const QRectF box(right - 26, r.center().y() - 13, 26, 26);
                p->setPen(Qt::NoPen);
                p->setBrush(selected ? Qt::white : kBlue);
                p->drawRoundedRect(box, 3, 3);
                p->setPen(selected ? kBlue : Qt::white);
                p->setFont(arial(16, true));
                p->drawText(box, Qt::AlignCenter, QString::number(deck));
                right = box.left() - 6;
            }
            const QRectF text(r.left() + 10, r.top(), right - r.left() - 14, r.height());
            p->setFont(arial(r.height() * .32, true));
            p->setPen(Qt::white);
            p->drawText(QRectF(text.left(), text.top(), text.width(), text.height() * .5 - 1), Qt::AlignLeft | Qt::AlignBottom,
                        p->fontMetrics().elidedText(t.title.isEmpty() ? QStringLiteral("Untitled") : t.title, Qt::ElideRight, int(text.width())));
            p->setFont(arial(r.height() * .26));
            p->setPen(QColor(selected ? "#e8f1ff" : "#9aa3ad"));
            p->drawText(QRectF(text.left(), text.top() + text.height() * .5 + 4, text.width(), text.height() * .4), Qt::AlignLeft | Qt::AlignTop,
                        p->fontMetrics().elidedText(t.artist, Qt::ElideRight, int(text.width())));
            break;
        }
        case 3:
            if (!t.key.isEmpty()) {
                p->setFont(arial(r.height() * .3, true));
                const double w = p->fontMetrics().horizontalAdvance(t.key) + 18;
                const QRectF box(r.center().x() - w / 2, r.center().y() - r.height() * .22, w, r.height() * .44);
                p->setPen(Qt::NoPen);
                p->setBrush(QColor(selected ? "#103a68" : "#1f3a2a"));
                p->drawRoundedRect(box, 4, 4);
                p->setPen(selected ? Qt::white : QColor("#7ee2a8"));
                p->drawText(box, Qt::AlignCenter, t.key);
                break;
            }
            [[fallthrough]];
        default:
            p->setFont(arial(r.height() * .32, index.column() == 2));
            p->setPen(selected || index.column() == 2 ? Qt::white : QColor("#c7cdd4"));
            p->drawText(r, Qt::AlignCenter, model_->data(index, Qt::DisplayRole).toString());
        }
        p->restore();
    }
    QSize sizeHint(const QStyleOptionViewItem &, const QModelIndex &) const override { return {80, 64}; }
private:
    std::shared_ptr<Artwork> artwork_;
    TrackModel *model_;
};

class CategoryModel : public QAbstractListModel {
public:
    using QAbstractListModel::QAbstractListModel;
    std::vector<Category> items;
    int rowCount(const QModelIndex &p = {}) const override { return p.isValid() ? 0 : int(items.size()); }
    QVariant data(const QModelIndex &i, int role) const override {
        if (!i.isValid() || i.row() >= int(items.size())) return {};
        return role == Qt::DisplayRole || role == Qt::AccessibleTextRole ? QVariant(items[size_t(i.row())].name) : QVariant();
    }
    void replace(std::vector<Category> rows) { beginResetModel(); items = std::move(rows); endResetModel(); }
};

class CategoryDelegate : public QStyledItemDelegate {
public:
    CategoryDelegate(CategoryModel *model, QObject *parent) : QStyledItemDelegate(parent), model_(model) {}
    void paint(QPainter *p, const QStyleOptionViewItem &option, const QModelIndex &index) const override {
        if (index.row() >= int(model_->items.size())) return;
        const Category &item = model_->items[size_t(index.row())];
        const QRectF r(option.rect);
        const bool selected = option.state & QStyle::State_Selected;
        p->save();
        p->setRenderHint(QPainter::Antialiasing);
        p->fillRect(r, selected ? kBlue : QColor(index.row() % 2 ? "#121212" : "#0c0c0c"));
        const QRectF icon(r.left() + 18, r.center().y() - 14, 28, 28);
        p->setPen(QPen(selected ? Qt::white : QColor("#8a929b"), 2));
        p->setBrush(Qt::NoBrush);
        if (item.folder) {
            QPainterPath path;
            path.moveTo(icon.left(), icon.top() + 6);
            path.lineTo(icon.left() + 10, icon.top() + 6);
            path.lineTo(icon.left() + 13, icon.top() + 10);
            path.lineTo(icon.right(), icon.top() + 10);
            path.lineTo(icon.right(), icon.bottom() - 3);
            path.lineTo(icon.left(), icon.bottom() - 3);
            path.closeSubpath();
            p->drawPath(path);
        } else {
            for (int k = 0; k < 3; ++k) {
                const double y = icon.top() + 7 + k * 7;
                p->drawLine(QPointF(icon.left() + 7, y), QPointF(icon.right(), y));
                p->drawPoint(QPointF(icon.left() + 1, y));
            }
        }
        const double right = r.right() - (item.folder ? 34 : 18);
        const QString count = item.folder ? QString() : QLocale().toString(item.count);
        p->setFont(arial(r.height() * .3));
        const double cw = count.isEmpty() ? 0 : p->fontMetrics().horizontalAdvance(count) + 12;
        p->setPen(QColor(selected ? "#e8f1ff" : "#7d858e"));
        p->drawText(QRectF(right - cw, r.top(), cw, r.height()), Qt::AlignRight | Qt::AlignVCenter, count);
        p->setFont(arial(r.height() * .36));
        p->setPen(Qt::white);
        const double nameW = right - cw - icon.right() - 24;
        p->drawText(QRectF(icon.right() + 18, r.top(), nameW, r.height()), Qt::AlignLeft | Qt::AlignVCenter,
                    p->fontMetrics().elidedText(item.name, Qt::ElideRight, int(nameW - 6)));
        if (item.folder) {
            p->setPen(QPen(QColor(selected ? "#e8f1ff" : "#7d858e"), 2.5));
            const double x = r.right() - 22, y = r.center().y();
            p->drawLine(QPointF(x - 6, y - 8), QPointF(x + 2, y));
            p->drawLine(QPointF(x + 2, y), QPointF(x - 6, y + 8));
        }
        p->restore();
    }
    QSize sizeHint(const QStyleOptionViewItem &, const QModelIndex &) const override { return {80, 64}; }
private:
    CategoryModel *model_;
};

// A category tile in the left column: a drawn icon over a small bold label.
class NavTile : public QAbstractButton {
public:
    NavTile(const QString &kind, QWidget *parent = nullptr) : QAbstractButton(parent), kind_(kind) {
        setFixedSize(76, 70);
        setAccessibleName(QStringLiteral("Browse by %1").arg(kind.toLower()));
        setFocusPolicy(Qt::NoFocus);
        setCursor(Qt::PointingHandCursor);
    }
    void setOn(bool on) { if (on != on_) { on_ = on; update(); } }
protected:
    void paintEvent(QPaintEvent *) override {
        QPainter p(this);
        p.setRenderHint(QPainter::Antialiasing);
        const QRectF r = QRectF(rect()).adjusted(5, 3, -5, -3);
        if (on_ || isDown()) {
            p.setPen(Qt::NoPen);
            p.setBrush(on_ ? kBlue : QColor("#1d2733"));
            p.drawRoundedRect(r, 6, 6);
        }
        const QColor ink = on_ ? Qt::white : QColor("#b9bfc6");
        const QRectF icon(r.center().x() - 12, r.top() + 9, 24, 24);
        p.setPen(QPen(ink, 2, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));
        p.setBrush(Qt::NoBrush);
        const QPointF c = icon.center();
        if (kind_ == "PLAYLIST") {
            for (int i = 0; i < 3; ++i) {
                const double y = icon.top() + 5 + i * 7;
                p.drawLine(QPointF(icon.left() + 6, y), QPointF(icon.right(), y));
                p.drawPoint(QPointF(icon.left(), y));
            }
        } else if (kind_ == "TRACK") {
            p.drawEllipse(QPointF(icon.left() + 6, icon.bottom() - 5), 5, 4);
            p.drawLine(QPointF(icon.left() + 11, icon.bottom() - 5), QPointF(icon.left() + 11, icon.top() + 1));
            p.drawLine(QPointF(icon.left() + 11, icon.top() + 1), QPointF(icon.right() - 2, icon.top() + 5));
        } else if (kind_ == "ARTIST") {
            p.drawEllipse(QPointF(c.x(), icon.top() + 7), 6, 6);
            p.drawArc(QRectF(icon.left() + 2, icon.top() + 15, 20, 18), 0, 180 * 16);
        } else if (kind_ == "ALBUM") {
            p.drawEllipse(c, 11, 11);
            p.drawEllipse(c, 3, 3);
        } else if (kind_ == "KEY") {
            p.drawEllipse(QPointF(icon.left() + 7, c.y()), 6, 6);
            p.drawLine(QPointF(icon.left() + 13, c.y()), QPointF(icon.right(), c.y()));
            p.drawLine(QPointF(icon.right() - 4, c.y()), QPointF(icon.right() - 4, c.y() + 6));
            p.drawLine(QPointF(icon.right() - 9, c.y()), QPointF(icon.right() - 9, c.y() + 4));
        } else if (kind_ == "HISTORY") {
            p.drawEllipse(c, 11, 11);
            p.drawLine(c, QPointF(c.x(), icon.top() + 5));
            p.drawLine(c, QPointF(c.x() + 6, c.y() + 2));
        } else if (kind_ == "SEARCH") {
            p.drawEllipse(QPointF(icon.left() + 9, icon.top() + 9), 8, 8);
            p.drawLine(QPointF(icon.left() + 15, icon.top() + 15), QPointF(icon.right() - 1, icon.bottom() - 1));
        }
        p.setPen(ink);
        p.setFont(arial(10, true));
        p.drawText(QRectF(r.left(), icon.bottom() + 5, r.width(), 14), Qt::AlignCenter, kind_);
    }
private:
    QString kind_;
    bool on_ = false;
};

// The navy source square heading the left column: which library is being browsed.
class SourceTile : public QAbstractButton {
public:
    explicit SourceTile(QWidget *parent = nullptr) : QAbstractButton(parent) {
        setFixedSize(76, 88);
        setFocusPolicy(Qt::NoFocus);
        setAccessibleName(QStringLiteral("Choose a music source"));
        setCursor(Qt::PointingHandCursor);
    }
    void setSource(const QString &label, bool usb) { name_ = label; usb_ = usb; update(); }
protected:
    void paintEvent(QPaintEvent *) override {
        QPainter p(this);
        p.setRenderHint(QPainter::Antialiasing);
        const QRectF r = QRectF(rect()).adjusted(5, 5, -5, -5);
        QLinearGradient fill(r.topLeft(), r.bottomLeft());
        fill.setColorAt(0, QColor(isDown() ? "#24508f" : "#21477f"));
        fill.setColorAt(1, QColor("#16305a"));
        p.setPen(Qt::NoPen);
        p.setBrush(fill);
        p.drawRoundedRect(r, 6, 6);
        p.setPen(Qt::white);
        p.setFont(arial(16, true));
        p.drawText(QRectF(r.left(), r.top() + 14, r.width(), 24), Qt::AlignCenter, usb_ ? QStringLiteral("USB") : QStringLiteral("COLL"));
        p.setFont(arial(10));
        p.drawText(QRectF(r.left() + 4, r.bottom() - 24, r.width() - 8, 16), Qt::AlignCenter,
                   p.fontMetrics().elidedText(name_, Qt::ElideRight, int(r.width() - 8)));
    }
private:
    QString name_ = "Collection";
    bool usb_ = false;
};

// A library on the SOURCE screen with what is on it.
class SourceCard : public QAbstractButton {
public:
    SourceCard(const QString &label, const QString &path, QWidget *parent = nullptr) : QAbstractButton(parent), label_(label), path_(path) {
        setMinimumSize(300, 190);
        setMaximumSize(720, 230);
        setFocusPolicy(Qt::NoFocus);
        setAccessibleName(QStringLiteral("Open %1").arg(label));
        setCursor(Qt::PointingHandCursor);
    }
    std::optional<SourceStats> stats;
protected:
    void paintEvent(QPaintEvent *) override {
        QPainter p(this);
        p.setRenderHint(QPainter::Antialiasing);
        const QRectF r = QRectF(rect()).adjusted(6, 6, -6, -6);
        p.setPen(QPen(QColor("#2a2e33"), 1));
        p.setBrush(QColor(isDown() ? "#1d2733" : "#141619"));
        p.drawRoundedRect(r, 10, 10);
        const QRectF tile(r.left() + 18, r.top() + 18, 70, 70);
        p.setPen(Qt::NoPen);
        p.setBrush(QColor("#1b3a6a"));
        p.drawRoundedRect(tile, 6, 6);
        p.setPen(Qt::white);
        p.setFont(arial(18, true));
        p.drawText(tile, Qt::AlignCenter, path_.isEmpty() ? QStringLiteral("COLL") : QStringLiteral("USB"));
        p.setFont(arial(24, true));
        p.drawText(QRectF(tile.right() + 18, tile.top(), r.width() - 130, 38), Qt::AlignLeft | Qt::AlignVCenter,
                   p.fontMetrics().elidedText(label_, Qt::ElideRight, int(r.width() - 130)));
        p.setFont(arial(15));
        p.setPen(QColor("#9aa3ad"));
        p.drawText(QRectF(tile.right() + 18, tile.top() + 38, r.width() - 130, 26), Qt::AlignLeft,
                   path_.isEmpty() ? QStringLiteral("Rekordbox collection") : QStringLiteral("Rekordbox USB export"));
        std::vector<std::pair<QString, QString>> rows;
        if (stats) {
            rows = {{"Songs", QLocale().toString(stats->tracks)}, {"Playlists", QLocale().toString(stats->playlists)}};
            if (stats->total > 0) rows.emplace_back("Available", QStringLiteral("%1 of %2 GB").arg(stats->free / 1e9, 0, 'f', 1).arg(stats->total / 1e9, 0, 'f', 0));
        }
        double y = tile.bottom() + 16;
        for (const auto &[key, value] : rows) {
            p.setFont(arial(15));
            p.setPen(QColor("#7d858e"));
            p.drawText(QRectF(r.left() + 18, y, 120, 24), Qt::AlignLeft | Qt::AlignVCenter, key);
            p.setPen(Qt::white);
            p.drawText(QRectF(r.left() + 140, y, r.width() - 160, 24), Qt::AlignLeft | Qt::AlignVCenter, value);
            y += 26;
        }
    }
private:
    QString label_, path_;
};

// LOAD over the deck's number, as large as a thumb; dimmed while that deck can't take a track.
class DeckButton : public QAbstractButton {
public:
    DeckButton(int deck, QWidget *parent = nullptr) : QAbstractButton(parent), deck_(deck) {
        setFixedSize(104, 80);
        setAccessibleName(QStringLiteral("Load the selected track on deck %1").arg(deck + 1));
        setCursor(Qt::PointingHandCursor);
    }
    void setLocked(bool locked) { if (locked != locked_) { locked_ = locked; update(); } }
protected:
    void paintEvent(QPaintEvent *) override {
        QPainter p(this);
        p.setRenderHint(QPainter::Antialiasing);
        const QRectF r = QRectF(rect()).adjusted(1, 1, -1, -1);
        const bool live = isEnabled() && !locked_;
        p.setPen(QPen(QColor(live ? "#4a5058" : "#24272b"), 1));
        p.setBrush(isDown() && live ? kBlue : QColor(live ? "#1b1b1b" : "#101010"));
        p.drawRoundedRect(r, 8, 8);
        p.setPen(live ? Qt::white : QColor("#4a4f55"));
        p.setFont(arial(12, true));
        p.drawText(QRectF(r.left(), r.top() + 6, r.width(), 16), Qt::AlignCenter, QStringLiteral("LOAD"));
        p.setFont(arial(36, true));
        p.drawText(QRectF(r.left(), r.top() + 20, r.width(), r.height() - 22), Qt::AlignCenter, QString::number(deck_ + 1));
    }
private:
    int deck_;
    bool locked_ = false;
};

// The selected track and the decks it can go to.
class LoadBar : public QFrame {
public:
    LoadBar(std::shared_ptr<Artwork> artwork, QWidget *parent = nullptr) : QFrame(parent), artwork_(std::move(artwork)) {
        setObjectName("loadbar");
        setFixedHeight(104);
    }
    void setTrack(const std::optional<CatalogTrack> &t) { track_ = t; update(); }
    QWidget *decks = nullptr;
protected:
    void paintEvent(QPaintEvent *e) override {
        QFrame::paintEvent(e);
        QPainter p(this);
        p.setRenderHint(QPainter::Antialiasing);
        p.setRenderHint(QPainter::SmoothPixmapTransform);
        if (!track_) {
            p.setPen(QColor("#5b636d"));
            p.setFont(arial(17));
            p.drawText(QRectF(18, 0, infoWidth() - 18, height()), Qt::AlignLeft | Qt::AlignVCenter, QStringLiteral("Tap a track, then a deck"));
            return;
        }
        const double side = height() - 22, x = 14 + side + 16, width = std::max(40.0, infoWidth() - x);
        sleeve(p, QRectF(14, 11, side, side), artwork_->get(track_->artwork));
        p.setPen(Qt::white);
        p.setFont(arial(21, true));
        p.drawText(QRectF(x, 12, width, 30), Qt::AlignLeft | Qt::AlignVCenter, p.fontMetrics().elidedText(track_->title, Qt::ElideRight, int(width)));
        p.setPen(QColor("#9aa3ad"));
        p.setFont(arial(17));
        p.drawText(QRectF(x, 42, width, 24), Qt::AlignLeft | Qt::AlignVCenter, p.fontMetrics().elidedText(track_->artist, Qt::ElideRight, int(width)));
        p.setPen(QColor("#d6dade"));
        p.setFont(arial(15, true));
        QStringList detail;
        if (track_->bpm > 0) detail << QStringLiteral("%1 BPM").arg(track_->bpm, 0, 'f', 2);
        if (!track_->key.isEmpty()) detail << track_->key;
        detail << minutes(track_->duration);
        p.drawText(QRectF(x, 68, width, 22), Qt::AlignLeft | Qt::AlignVCenter, detail.join(QStringLiteral("   ")));
    }
private:
    double infoWidth() const { return decks ? decks->geometry().left() - 16 : width() / 2.0; }
    std::shared_ptr<Artwork> artwork_;
    std::optional<CatalogTrack> track_;
};

// Wave's own keyboard, for the search box.
class TouchKeyboard : public QWidget {
public:
    explicit TouchKeyboard(QLineEdit *target, QWidget *parent = nullptr) : QWidget(parent), target_(target) {
        setObjectName("keyboard");
        setAttribute(Qt::WA_StyledBackground);
        auto *layout = new QVBoxLayout(this);
        layout->setContentsMargins(10, 10, 10, 10);
        layout->setSpacing(6);
        for (const char *chars : {"1234567890", "qwertyuiop", "asdfghjkl", "zxcvbnm"}) {
            auto *row = new QHBoxLayout;
            row->setSpacing(6);
            for (const char *c = chars; *c; ++c) {
                const QChar ch(*c);
                QPushButton *k = key(QString(ch), [this, ch] { target_->insert(shift_ ? QString(ch.toUpper()) : QString(ch)); });
                letters_.emplace_back(k, ch);
                row->addWidget(k);
            }
            layout->addLayout(row);
        }
        auto *row = new QHBoxLayout;
        QPushButton *shift = key(QStringLiteral("SHIFT"), [this] {
            shift_ = !shift_;
            for (auto &[k, c] : letters_) k->setText(shift_ ? QString(c.toUpper()) : QString(c));
        });
        shift->setCheckable(true);
        row->addWidget(shift);
        for (const char *t : {"-", "'", "/", "."}) row->addWidget(key(QString::fromLatin1(t), [this, t] { target_->insert(QString::fromLatin1(t)); }));
        row->addWidget(key(QStringLiteral("SPACE"), [this] { target_->insert(QStringLiteral(" ")); }), 3);
        QPushButton *del = key(QStringLiteral("DELETE"), [this] { target_->backspace(); });
        del->setAutoRepeat(true);
        row->addWidget(del);
        row->addWidget(key(QStringLiteral("CLEAR"), [this] { target_->clear(); }));
        row->addWidget(key(QStringLiteral("DONE"), [this] { hide(); }));
        layout->addLayout(row);
        hide();
    }
private:
    QPushButton *key(const QString &text, std::function<void()> action) {
        auto *b = new QPushButton(text);
        b->setMinimumHeight(52);
        b->setFocusPolicy(Qt::NoFocus);
        connect(b, &QPushButton::clicked, this, action);
        return b;
    }
    QLineEdit *target_;
    bool shift_ = false;
    std::vector<std::pair<QPushButton *, QChar>> letters_;
};

// --- The page ------------------------------------------------------------------------------------

template <typename T> void BrowserPage::work(std::function<T(CatalogCache &)> job, std::function<void(T)> done, bool devices) {
    if (closed_) return;
    auto *watcher = new QFutureWatcher<T>(this);
    connect(watcher, &QFutureWatcherBase::finished, this, [watcher, done] {
        watcher->deleteLater();
        done(watcher->result());
    });
    auto cache = cache_;
    watcher->setFuture(QtConcurrent::run(devices ? &devices_ : &worker_, [cache, job] { return job(*cache); }));
}

BrowserPage::BrowserPage(QWidget *parent) : QWidget(parent), cache_(std::make_shared<CatalogCache>()), artwork_(std::make_shared<Artwork>()) {
    setObjectName("browser");
    setAttribute(Qt::WA_StyledBackground);
    worker_.setMaxThreadCount(1);
    devices_.setMaxThreadCount(1);  // finding sticks and their space: a slow drive never holds up the list
    auto *outer = new QHBoxLayout(this);
    outer->setContentsMargins(0, 0, 0, 0);
    outer->setSpacing(0);
    auto *nav = new QFrame;
    nav->setObjectName("browserNav");
    auto *column = new QVBoxLayout(nav);
    column->setContentsMargins(0, 2, 0, 2);
    column->setSpacing(2);
    sourceTile_ = new SourceTile;
    connect(sourceTile_, &QAbstractButton::clicked, this, &BrowserPage::showSources);
    column->addWidget(sourceTile_);
    for (const char *kind : {"PLAYLIST", "TRACK", "ARTIST", "ALBUM", "KEY", "HISTORY", "SEARCH"}) {
        auto *tile = new NavTile(QString::fromLatin1(kind));
        connect(tile, &QAbstractButton::clicked, this, [this, kind] { openCategory(QString::fromLatin1(kind)); });
        tiles_[QString::fromLatin1(kind)] = tile;
        column->addWidget(tile);
    }
    column->addStretch();
    outer->addWidget(nav);

    auto *main = new QVBoxLayout;
    main->setContentsMargins(0, 0, 0, 0);
    main->setSpacing(0);
    outer->addLayout(main, 1);
    auto *top = new QFrame;
    top->setObjectName("browserTop");
    auto *bar = new QHBoxLayout(top);
    bar->setContentsMargins(14, 8, 14, 8);
    bar->setSpacing(12);
    back_ = button(QStringLiteral("‹"));
    back_->setObjectName("back");
    back_->setAccessibleName(QStringLiteral("Back"));
    back_->setFixedSize(52, 52);
    connect(back_, &QPushButton::clicked, this, [this] { goBack(); });
    bar->addWidget(back_);
    // Where the browser is, as a row of crumbs: tap any of them to go back there.
    crumbs_ = new QWidget;
    crumbs_->setObjectName("crumbs");
    crumbRow_ = new QHBoxLayout(crumbs_);
    crumbRow_->setContentsMargins(0, 0, 0, 0);
    crumbRow_->setSpacing(2);
    bar->addWidget(crumbs_, 1);
    search_ = new QLineEdit;
    search_->setPlaceholderText(QStringLiteral("Search title or artist"));
    search_->setAccessibleName(QStringLiteral("Search collection"));
    search_->setClearButtonEnabled(true);
    search_->setMinimumHeight(52);
    search_->setMinimumWidth(260);
    bar->addWidget(search_, 1);
    QPushButton *keys = button(QStringLiteral("ABC"));
    keys->setObjectName("keys");
    keys->setAccessibleName(QStringLiteral("Show the on-screen keyboard"));
    keys->setFixedSize(60, 52);
    bar->addWidget(keys);
    QPushButton *refresh = button(QStringLiteral("↻"));
    refresh->setObjectName("keys");
    refresh->setAccessibleName(QStringLiteral("Look for USB sticks and reload"));
    refresh->setFixedSize(60, 52);
    bar->addWidget(refresh);
    main->addWidget(top);

    stack_ = new QStackedWidget;
    main->addWidget(stack_, 1);
    sourcesPage_ = new QWidget;
    sourcesPage_->setObjectName("sourcesPage");
    sourcesPage_->setAttribute(Qt::WA_StyledBackground);
    sourcesGrid_ = new QGridLayout(sourcesPage_);
    sourcesGrid_->setContentsMargins(24, 24, 24, 24);
    sourcesGrid_->setSpacing(16);
    stack_->addWidget(sourcesPage_);
    categories_ = new CategoryModel(this);
    list_ = new QListView;
    list_->setModel(categories_);
    list_->setItemDelegate(new CategoryDelegate(categories_, list_));
    list_->setUniformItemSizes(true);
    list_->setVerticalScrollMode(QAbstractItemView::ScrollPerPixel);
    list_->setAccessibleName(QStringLiteral("Browse list"));
    connect(list_, &QListView::clicked, this, [this](const QModelIndex &i) { tapped(i.row()); });
    kinetic(list_);
    stack_->addWidget(list_);
    model_ = new TrackModel(this);
    model_->moreWanted = [this] { fetchMore(); };
    table_ = new QTableView;
    table_->setAccessibleName(QStringLiteral("Tracks; tap a heading to sort"));
    table_->setModel(model_);
    table_->setItemDelegate(new TrackDelegate(artwork_, model_, table_));
    table_->setSelectionBehavior(QAbstractItemView::SelectRows);
    table_->setSelectionMode(QAbstractItemView::SingleSelection);
    table_->setEditTriggers(QAbstractItemView::NoEditTriggers);
    table_->setVerticalScrollMode(QAbstractItemView::ScrollPerPixel);
    table_->setHorizontalScrollMode(QAbstractItemView::ScrollPerPixel);
    table_->setShowGrid(false);
    table_->setWordWrap(false);
    table_->verticalHeader()->hide();
    table_->verticalHeader()->setDefaultSectionSize(64);
    QHeaderView *header = table_->horizontalHeader();
    header->setMinimumHeight(40);
    header->setSectionResizeMode(0, QHeaderView::Fixed);
    table_->setColumnWidth(0, 72);
    header->setSectionResizeMode(1, QHeaderView::Stretch);
    for (auto [col, w] : {std::pair{2, 110}, {3, 100}, {4, 96}}) {
        header->setSectionResizeMode(col, QHeaderView::Fixed);
        table_->setColumnWidth(col, w);
    }
    header->setSectionsClickable(true);
    header->setSortIndicator(1, Qt::AscendingOrder);
    header->setSortIndicatorShown(true);
    connect(header, &QHeaderView::sectionClicked, this, &BrowserPage::sortBy);
    kinetic(table_);
    connect(table_->selectionModel(), &QItemSelectionModel::selectionChanged, this, [this] { selectionChanged(); });
    stack_->addWidget(table_);

    keyboard_ = new TouchKeyboard(search_);
    main->addWidget(keyboard_);
    connect(keys, &QPushButton::clicked, this, &BrowserPage::toggleKeyboard);
    message_ = new QLabel(QStringLiteral("Opening collection…"));
    message_->setObjectName("message");
    main->addWidget(message_);
    loadBar_ = new LoadBar(artwork_);
    auto *loads = new QHBoxLayout(loadBar_);
    loads->setContentsMargins(14, 12, 14, 12);
    loads->setSpacing(10);
    loads->addStretch(1);
    auto *decks = new QWidget;
    decks->setStyleSheet(QStringLiteral("background:transparent;"));
    deckRow_ = new QHBoxLayout(decks);
    deckRow_->setContentsMargins(0, 0, 0, 0);
    deckRow_->setSpacing(10);
    loads->addWidget(decks);
    loadBar_->decks = decks;
    for (int idx = 0; idx < 4; ++idx) {
        auto *b = new DeckButton(idx);
        connect(b, &QAbstractButton::clicked, this, [this, idx] { load(idx); });
        deckButtons_[idx] = b;
        b->hide();
    }
    main->addWidget(loadBar_);

    debounce_ = new QTimer(this);
    debounce_->setSingleShot(true);
    debounce_->setInterval(180);
    connect(debounce_, &QTimer::timeout, this, &BrowserPage::startQuery);
    connect(search_, &QLineEdit::textChanged, this, &BrowserPage::searchChanged);
    connect(refresh, &QPushButton::clicked, this, &BrowserPage::showSources);
    openCategory(QStringLiteral("TRACK"));
}

BrowserPage::~BrowserPage() { shutdown(); }

void BrowserPage::shutdown() {
    if (closed_) return;
    closed_ = true;
    // Release Qt's shared gesture targets before their viewports are destroyed.
    QScroller::ungrabGesture(table_->viewport());
    QScroller::ungrabGesture(list_->viewport());
    debounce_->stop();
    worker_.clear();
    devices_.clear();
    worker_.waitForDone(3000);
    devices_.waitForDone(3000);
}

void BrowserPage::setHeading() {
    while (QLayoutItem *item = crumbRow_->takeAt(0)) {
        if (QWidget *w = item->widget()) { w->hide(); w->deleteLater(); }
        delete item;
    }
    QStringList names{sourceLabel_.toUpper()};
    for (const auto &[title, view] : trail_) names << title;
    names << title_;
    crumbs_->setAccessibleName(names.join(QStringLiteral("  ›  ")));
    // The source, then every level above, then where the browser is now (not a button).
    std::vector<std::pair<QString, int>> levels{{sourceLabel_.toUpper(), -1}};
    for (size_t n = 0; n < trail_.size(); ++n) levels.emplace_back(trail_[n].first, int(n));
    if (levels.size() > 4) levels = {levels[0], {QStringLiteral("…"), int(trail_.size()) - 3}, levels[levels.size() - 2], levels.back()};
    auto shortText = [](const QString &t) { return t.size() <= 22 ? t : t.left(20) + QStringLiteral("…"); };
    for (const auto &[text, level] : levels) {
        QPushButton *crumb = button(shortText(text));
        crumb->setToolTip(text);
        crumb->setObjectName("crumb");
        crumb->setFocusPolicy(Qt::NoFocus);
        connect(crumb, &QPushButton::clicked, this, [this, level = level] { backTo(level); });
        crumbRow_->addWidget(crumb);
        auto *arrow = new QLabel(QStringLiteral("›"));
        arrow->setObjectName("crumbArrow");
        crumbRow_->addWidget(arrow);
    }
    auto *here = new QLabel(shortText(title_));
    here->setObjectName("crumbHere");
    crumbRow_->addWidget(here);
    crumbRow_->addStretch(1);
    back_->setEnabled(!trail_.empty() || view_.kind != "sources");
    for (auto &[kind, tile] : tiles_) tile->setOn(kind == kind_ && view_.kind != "sources");
}

void BrowserPage::navigate(const View &view, const QString &title) {
    view_ = view;
    title_ = title;
    if (view.kind != "tracks") table_->clearSelection();  // nothing stays picked from a list no longer shown
    setHeading();
    if (view.kind == "sources") {
        stack_->setCurrentWidget(sourcesPage_);
        work<std::vector<std::pair<QString, QString>>>([](CatalogCache &) { return sources(); }, [this](auto available) {
            while (QLayoutItem *item = sourcesGrid_->takeAt(0)) {
                if (item->widget()) item->widget()->deleteLater();
                delete item;
            }
            for (size_t n = 0; n < available.size(); ++n) {
                const auto [label, path] = available[n];
                auto *card = new SourceCard(label, path);
                connect(card, &QAbstractButton::clicked, this, [this, label = label, path = path] { chooseSource(label, path); });
                sourcesGrid_->addWidget(card, int(n / 2), int(n % 2), Qt::AlignLeft | Qt::AlignTop);
                QPointer<SourceCard> guard(card);
                work<std::optional<SourceStats>>([path = path](CatalogCache &c) -> std::optional<SourceStats> {
                    QString error;
                    Library *lib = c.get(path, &error);
                    return lib ? std::optional<SourceStats>(lib->stats()) : std::nullopt;
                }, [guard](std::optional<SourceStats> s) { if (guard) { guard->stats = s; guard->update(); } });
            }
            sourcesGrid_->setRowStretch(int(available.size() / 2) + 1, 1);
            if (view_.kind == "sources")
                message_->setText(available.size() > 1 ? QStringLiteral("Tap a source to browse it.")
                                                       : QStringLiteral("No USB stick found. Plug one in and tap ↻, or browse the collection."));
            bool present = sourcePath_.isEmpty();
            for (const auto &[label, path] : available) present = present || path == sourcePath_;
            if (!present) chooseSource(QStringLiteral("Collection"), {});  // the stick being browsed was removed
        }, true);
    } else if (view.kind == "list") {
        stack_->setCurrentWidget(list_);
        categories_->replace({});
        message_->setText(QStringLiteral("Opening…"));
        const QString source = sourcePath_, category = view.category, parent = view.parent;
        work<std::pair<std::vector<Category>, QString>>([source, category, parent](CatalogCache &c) {
            QString error;
            Library *lib = c.get(source, &error);
            return lib ? std::make_pair(lib->categories(category, parent), QString()) : std::make_pair(std::vector<Category>{}, error);
        }, [this, view](auto result) {
            if (!(view == view_)) return;
            if (!result.second.isEmpty()) {
                message_->setText(QStringLiteral("Library unavailable: %1. Start Rekordbox or reconnect the USB, then tap ↻.").arg(result.second));
                return;
            }
            categories_->replace(result.first);
            message_->setText(result.first.empty() ? QStringLiteral("Nothing here")
                                                   : QStringLiteral("%1 %2").arg(QLocale().toString(int(result.first.size())), title_.toLower()));
        });
    } else {
        stack_->setCurrentWidget(table_);
        const bool ordered = view.scope && (view.scope->first == "PLAYLIST" || view.scope->first == "HISTORY");
        sort_ = {ordered ? QStringLiteral("Order") : QStringLiteral("Title"), false};
        // A playlist plays in its own order until a heading is tapped.
        table_->horizontalHeader()->setSortIndicatorShown(!ordered);
        table_->horizontalHeader()->setSortIndicator(1, Qt::AscendingOrder);
        request();
    }
}

void BrowserPage::openCategory(const QString &kind) {
    kind_ = kind;
    trail_.clear();
    if (kind != "SEARCH" && !search_->text().isEmpty()) {
        const bool b = search_->blockSignals(true);  // a new category starts without the last search
        search_->clear();
        search_->blockSignals(b);
    }
    if (kind == "TRACK" || kind == "SEARCH") {
        navigate(View{"tracks", {}, {}, std::nullopt}, kind);
        if (kind == "SEARCH") focusSearch();
    } else {
        navigate(View{"list", kind, {}, std::nullopt}, kind);
    }
}

void BrowserPage::tapped(int row) {
    // Opens it, or with double tap set (the default), highlights it and a second tap on it opens it, so a
    // finger can scroll and pick without opening anything by accident.
    const double t = now();
    const auto first = lastTap_;
    lastTap_ = std::make_pair(row, t);
    if (openOn_ == "single" || (first && first->first == row && t - first->second < kDoubleTap)) {
        lastTap_.reset();
        openItem(row);
    } else {
        list_->setCurrentIndex(categories_->index(row));
    }
}

void BrowserPage::openItem(int row) {
    if (row < 0 || row >= int(categories_->items.size())) return;
    const Category item = categories_->items[size_t(row)];
    if (!search_->text().isEmpty()) {
        const bool b = search_->blockSignals(true);  // a playlist opens whole, not filtered by an old search
        search_->clear();
        search_->blockSignals(b);
    }
    trail_.emplace_back(title_, view_);
    if (item.folder) navigate(View{"list", view_.category, item.id, std::nullopt}, item.name);
    else navigate(View{"tracks", {}, {}, std::make_pair(view_.category, item.id)}, item.name);
}

void BrowserPage::backTo(int level) {
    if (level < 0) { showSources(); return; }
    if (level < int(trail_.size())) {
        const auto [title, view] = trail_[size_t(level)];
        trail_.erase(trail_.begin() + level, trail_.end());
        navigate(view, title);
    }
}

bool BrowserPage::goBack() {
    if (!trail_.empty()) {
        const auto [title, view] = trail_.back();
        trail_.pop_back();
        navigate(view, title);
        return true;
    }
    if (view_.kind == "sources") {
        openCategory(kind_);
        return true;
    }
    return false;
}

void BrowserPage::showSources() {
    trail_.clear();
    navigate(View{"sources", {}, {}, std::nullopt}, QStringLiteral("SOURCE"));
}

void BrowserPage::chooseSource(const QString &label, const QString &path) {
    sourceLabel_ = label;
    sourcePath_ = path;
    sourceTile_->setSource(label, !path.isEmpty());
    model_->replace({}, 0);  // the old source's rows would load by the wrong library's ids
    selectionChanged();
    openCategory(QStringLiteral("PLAYLIST"));
}

bool BrowserPage::highlightItem(const QString &id) {
    for (size_t row = 0; row < categories_->items.size(); ++row)
        if (categories_->items[row].id == id) {
            const QModelIndex index = categories_->index(int(row));
            list_->setCurrentIndex(index);
            list_->scrollTo(index, QAbstractItemView::EnsureVisible);
            return true;
        }
    return false;
}

void BrowserPage::searchChanged() {
    if (view_.kind != "tracks") {
        kind_ = QStringLiteral("SEARCH");
        trail_.clear();
        view_ = View{"tracks", {}, {}, std::nullopt};
        title_ = QStringLiteral("SEARCH");
        stack_->setCurrentWidget(table_);
        setHeading();
    }
    request();
}

void BrowserPage::sortBy(int column) {
    static const char *fields[] = {nullptr, "Title", "BPM", "Key", "Time"};
    if (column < 1 || column > 4 || view_.kind != "tracks") return;
    const QString field = QString::fromLatin1(fields[column]);
    sort_ = {field, sort_.first == field ? !sort_.second : false};
    table_->horizontalHeader()->setSortIndicatorShown(true);
    table_->horizontalHeader()->setSortIndicator(column, sort_.second ? Qt::DescendingOrder : Qt::AscendingOrder);
    request();
}

void BrowserPage::request() {
    ++revision_;
    appending_ = false;
    table_->clearSelection();  // never leave a row from an older query selected while the new one is pending
    debounce_->start();
}

void BrowserPage::fetchMore() {
    if (querying_ || closed_ || debounce_->isActive()) return;
    appending_ = true;
    startQuery();
}

void BrowserPage::startQuery() {
    if (closed_ || querying_) return;
    querying_ = true;
    running_ = revision_;
    const int offset = appending_ ? int(model_->tracks.size()) : 0;
    if (!appending_) message_->setText(QStringLiteral("Searching…"));
    const QString source = sourcePath_, text = search_->text(), field = sort_.first;
    const bool descending = sort_.second;
    const auto scope = view_.scope;
    using Result = std::tuple<int, std::vector<CatalogTrack>, QString>;
    work<Result>([=](CatalogCache &c) -> Result {
        QString error;
        Library *lib = c.get(source, &error);
        if (!lib) return {0, {}, error.isEmpty() ? QStringLiteral("Rekordbox's library was not found") : error};
        auto [total, rows] = lib->browse(text, field, descending, offset, kPage, scope);
        return {total, rows, QString()};
    }, [this](Result r) {
        querying_ = false;
        if (running_ != revision_) { startQuery(); return; }
        if (view_.kind != "tracks") return;
        auto &[total, rows, error] = r;
        if (!error.isEmpty()) {
            model_->replace({}, 0);
            message_->setText(QStringLiteral("Library unavailable: %1. Start Rekordbox or reconnect the USB, then tap ↻.").arg(error));
            return;
        }
        if (appending_) model_->append(rows);
        else {
            model_->replace(rows, total);
            selectionChanged();  // a reset drops the selection without signalling it
            table_->scrollToTop();
        }
        appending_ = false;
        message_->setText(total ? QStringLiteral("%1 tracks").arg(QLocale().toString(total)) : QStringLiteral("No matching tracks"));
    });
}

int BrowserPage::step(int delta) {
    QWidget *shown = stack_->currentWidget();
    int count = 0, current = -1;
    if (shown == table_) {
        count = model_->rowCount();
        const auto rows = table_->selectionModel()->selectedRows();
        current = rows.isEmpty() ? -1 : rows[0].row();
    } else if (shown == list_) {
        count = categories_->rowCount();
        current = list_->currentIndex().isValid() ? list_->currentIndex().row() : -1;
    } else {
        return -1;
    }
    if (!count) return -1;
    const int target = std::clamp((current >= 0 ? current : (delta > 0 ? -1 : count)) + delta, 0, count - 1);
    if (shown == table_) {
        if (target >= count - 3 && model_->canFetchMore({})) model_->fetchMore({});  // more of a long list, as the knob nears its end
        table_->selectRow(target);
        table_->scrollTo(model_->index(target, 1), QAbstractItemView::EnsureVisible);
    } else {
        list_->setCurrentIndex(categories_->index(target));
        list_->scrollTo(categories_->index(target), QAbstractItemView::EnsureVisible);
    }
    return target;
}

bool BrowserPage::enter() {
    if (stack_->currentWidget() == list_ && list_->currentIndex().isValid()) {
        openItem(list_->currentIndex().row());
        return true;
    }
    return false;
}

bool BrowserPage::showsList() const { return stack_->currentWidget() == list_; }

void BrowserPage::focusForKnob() { (stack_->currentWidget() == list_ ? static_cast<QWidget *>(list_) : table_)->setFocus(); }

void BrowserPage::focusSearch() {
    search_->setFocus();
    if (keyboardMode_ == "windows") touch::showWindowsKeyboard();
    else keyboard_->show();
}

bool BrowserPage::keyboardShown() const { return keyboard_->isVisible(); }
void BrowserPage::hideKeyboard() { keyboard_->hide(); }

void BrowserPage::toggleKeyboard() {
    if (keyboardMode_ == "windows") {
        search_->setFocus();
        touch::showWindowsKeyboard();
        return;
    }
    keyboard_->setVisible(!keyboard_->isVisible());
}

std::optional<CatalogTrack> BrowserPage::selectedTrack() const {
    // Only while tracks are shown: a track highlighted before can never be loaded from a view that doesn't show it.
    if (stack_->currentWidget() != table_) return std::nullopt;
    const auto rows = table_->selectionModel()->selectedRows();
    if (rows.isEmpty() || rows[0].row() >= int(model_->tracks.size())) return std::nullopt;
    return model_->tracks[size_t(rows[0].row())];
}

void BrowserPage::selectionChanged() {
    loadBar_->setTrack(selectedTrack());
    syncDecks();
}

void BrowserPage::load(int deck) {
    const auto track = selectedTrack();
    if (!track) message_->setText(QStringLiteral("Tap a track first."));
    else if (!locked_[deck].isEmpty()) message_->setText(locked_[deck]);
    else emit loadRequested(deck, *track, sourcePath_);
}

void BrowserPage::setDecks(const std::vector<int> &order, const std::map<int, QString> &locked, const QString &unavailable,
                           const std::map<int, QString> &loaded) {
    if (order != deckOrder_) {
        deckOrder_ = order;
        for (auto &[idx, b] : deckButtons_) { deckRow_->removeWidget(b); b->hide(); }
        for (int idx : order) { deckRow_->addWidget(deckButtons_[idx]); deckButtons_[idx]->show(); }
    }
    locked_.clear();
    for (const auto &[idx, why] : locked) locked_[idx] = unavailable.isEmpty() ? why : unavailable;
    if (unavailable != unavailable_) {
        unavailable_ = unavailable;
        if (!unavailable.isEmpty()) message_->setText(unavailable);
    }
    std::map<QString, std::vector<int>> index;
    for (const auto &[deck, path] : loaded) if (!path.isEmpty()) index[folded(path)].push_back(deck + 1);
    if (index != model_->loaded) {
        model_->loaded = index;
        table_->viewport()->update();
    }
    syncDecks();
}

void BrowserPage::syncDecks() {
    // A locked deck stays pressable: on a touch screen, pressing it is how its reason is seen.
    const bool selected = selectedTrack().has_value();
    for (auto &[idx, b] : deckButtons_) {
        b->setEnabled(selected);
        b->setToolTip(locked_[idx]);
        b->setLocked(!locked_[idx].isEmpty());
    }
}

void BrowserPage::showLoadStatus(const QString &text) { message_->setText(text); }

QString BrowserPage::sourceFor(const QString &file) const {
    const QString f = folded(file);
    for (const QString &root : deviceRoots_) {
        QString r = folded(root);
        while (r.endsWith('/')) r.chop(1);
        if (!r.isEmpty() && f.startsWith(r + '/')) return QDir(root).filePath("PIONEER/rekordbox/exportLibrary.db");
    }
    return {};
}

std::pair<CatalogTrack, QString> BrowserPage::forLoading(CatalogTrack track) const {
    // A stick's export stores paths from its own root.
    const QString source = sourceFor(track.path);
    if (!source.isEmpty()) {
        const QString root = QDir(QFileInfo(source).absolutePath() + "/../..").absolutePath();
        track.path = '/' + QDir(root).relativeFilePath(QDir::fromNativeSeparators(track.path));
    }
    return {track, source};
}

void BrowserPage::locate(const QString &source, const std::vector<uint32_t> &ids, int rows, std::function<void(std::optional<Located>)> done) {
    work<std::optional<Located>>([source, ids, rows](CatalogCache &c) -> std::optional<Located> {
        QString error;
        Library *lib = c.get(source, &error);
        return lib ? lib->locate(ids, rows) : std::nullopt;
    }, std::move(done));
}

void BrowserPage::tracksForPaths(const QStringList &paths, std::function<void(std::map<QString, CatalogTrack>)> done) {
    work<std::map<QString, CatalogTrack>>([paths](CatalogCache &c) {
        std::map<QString, CatalogTrack> found;
        for (const QString &path : paths) {
            const auto t = path.isEmpty() ? std::nullopt : c.libraries.resolvePath(path);
            found[path] = t ? CatalogTrack{t->id, t->title, t->artist, t->bpm, t->key, t->lengthSec, path, t->artwork}
                            : CatalogTrack{{}, QFileInfo(path).completeBaseName(), {}, 0, {}, 0, path, {}};
        }
        return found;
    }, std::move(done));
}

}  // namespace wave
