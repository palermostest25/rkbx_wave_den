// The touch browser, in the style of a deck's own: a source tile and categories down the left, a back bar
// with tappable breadcrumbs and search across the top, drill-down lists and tracks in the middle, and the
// load bar below. Queries run on a worker of their own, so the waveforms never wait for the library.
#pragma once

#include "core/library.h"

#include <QFrame>
#include <QPointer>
#include <QThreadPool>
#include <QWidget>

#include <functional>
#include <map>
#include <memory>
#include <optional>

class QGridLayout;
class QHBoxLayout;
class QLabel;
class QLineEdit;
class QListView;
class QPushButton;
class QStackedWidget;
class QTableView;
class QTimer;

namespace wave {

class TrackModel;
class CategoryModel;
class Artwork;
class NavTile;
class SourceTile;
class DeckButton;
class LoadBar;
class TouchKeyboard;
struct CatalogCache;

class BrowserPage : public QWidget {
    Q_OBJECT
public:
    explicit BrowserPage(QWidget *parent = nullptr);
    ~BrowserPage() override;

    // The controller's knob, push and BACK driving this browser.
    int step(int delta);                     // moves the highlight in what is shown; the row
    bool enter();                            // opens the highlighted playlist or folder
    bool goBack();                           // one level up; false at a category's root
    bool highlightItem(const QString &id);   // highlight one item of the list shown now
    bool showsList() const;                  // playlists or folders are shown (not tracks)
    void focusForKnob();                     // opened from the controller: no keyboard pops up
    void focusSearch();
    bool keyboardShown() const;
    void hideKeyboard();

    std::optional<CatalogTrack> selectedTrack() const;  // only while tracks are shown
    QString sourcePath() const { return sourcePath_; }
    QString sourceFor(const QString &file) const;     // the stick library a mounted file belongs to, "" collection
    std::pair<CatalogTrack, QString> forLoading(CatalogTrack track) const;
    void setDeviceRoots(const QStringList &roots) { deviceRoots_ = roots; }

    void setOpenOn(const QString &mode) { openOn_ = mode; }      // "single" or "double" tap opens a playlist
    void setKeyboard(const QString &which) { keyboardMode_ = which; }  // "wave" or "windows"
    void setDecks(const std::vector<int> &order, const std::map<int, QString> &locked, const QString &unavailable,
                  const std::map<int, QString> &loaded);
    void showLoadStatus(const QString &text);

    // Library work on the browser's worker; ``done`` runs on the UI thread.
    void locate(const QString &source, const std::vector<uint32_t> &ids, int rows, std::function<void(std::optional<Located>)> done);
    void tracksForPaths(const QStringList &paths, std::function<void(std::map<QString, CatalogTrack>)> done);
    void shutdown();

signals:
    void loadRequested(int deck, const wave::CatalogTrack &track, const QString &source);

private:
    struct View { QString kind = "tracks"; QString category, parent; std::optional<std::pair<QString, QString>> scope;
                  bool operator==(const View &) const = default; };
    void navigate(const View &view, const QString &title);
    void openCategory(const QString &kind);
    void tapped(int row);
    void openItem(int row);
    void backTo(int level);
    void showSources();
    void chooseSource(const QString &label, const QString &path);
    void setHeading();
    void searchChanged();
    void sortBy(int column);
    void request();
    void startQuery();
    void fetchMore();
    void selectionChanged();
    void load(int deck);
    void syncDecks();
    void toggleKeyboard();
    template <typename T> void work(std::function<T(CatalogCache &)> job, std::function<void(T)> done, bool devices = false);

    std::shared_ptr<CatalogCache> cache_;
    QThreadPool worker_, devices_;
    std::shared_ptr<Artwork> artwork_;
    QString sourceLabel_ = "Collection", sourcePath_, kind_ = "TRACK", title_ = "TRACK", openOn_ = "double", keyboardMode_ = "wave";
    View view_;
    std::vector<std::pair<QString, View>> trail_;
    std::pair<QString, bool> sort_{"Title", false};
    int revision_ = 0, running_ = -1;
    bool querying_ = false, appending_ = false, closed_ = false;
    std::optional<std::pair<int, double>> lastTap_;
    std::vector<int> deckOrder_;
    std::map<int, QString> locked_;
    QString unavailable_;
    QStringList deviceRoots_;

    SourceTile *sourceTile_;
    std::map<QString, NavTile *> tiles_;
    QPushButton *back_;
    QWidget *crumbs_;
    QHBoxLayout *crumbRow_;
    QLineEdit *search_;
    QStackedWidget *stack_;
    QWidget *sourcesPage_;
    QGridLayout *sourcesGrid_;
    CategoryModel *categories_;
    QListView *list_;
    TrackModel *model_;
    QTableView *table_;
    TouchKeyboard *keyboard_;
    QLabel *message_;
    LoadBar *loadBar_;
    QHBoxLayout *deckRow_;
    std::map<int, DeckButton *> deckButtons_;
    QTimer *debounce_;
};

}  // namespace wave
