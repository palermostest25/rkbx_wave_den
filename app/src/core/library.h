// Rekordbox's libraries, strictly read-only: the collection (master.db) and each USB stick's export
// (PIONEER/rekordbox/exportLibrary.db), both SQLCipher databases. What the deck screen needs (a loaded
// file's track, its cues) and what the touch browser lists (sources, playlists, tracks, search).
#pragma once

#include "core/anlz.h"

#include <QString>
#include <QStringList>

#include <memory>
#include <optional>
#include <vector>

struct sqlite3;

namespace wave {

struct LibraryTrack {
    QString id, title, artist, key;
    double lengthSec = 0, bpm = 0;
    QString audioPath, analysisFolder, artwork;
};

struct CatalogTrack {
    QString id, title, artist;
    double bpm = 0;
    QString key;
    double duration = 0;
    QString path, artwork;
};

struct Category {
    QString id, name;
    int count = 0;
    bool folder = false;
};

struct Located {
    QString kind;   // PLAYLIST, HISTORY, or TRACK for the whole collection
    QString id, name;
    std::vector<std::pair<QString, QString>> trail;  // (id, name) of the folders above, outermost first
};

struct SourceStats { int tracks = 0, playlists = 0; qint64 total = 0, free = 0; };

// One open library. ``source`` is "" for the collection, else a stick's exportLibrary.db.
class Library {
public:
    static std::unique_ptr<Library> open(const QString &source, QString *error = nullptr);
    static std::unique_ptr<Library> openFile(const QString &path, bool device, QString *error = nullptr);
    ~Library();

    bool device() const { return device_; }
    QString root() const;  // where its analysis and artwork paths start
    std::optional<LibraryTrack> resolvePath(const QString &audioPath);
    std::vector<Cue> cues(const QString &analysisFolder);  // master.db's cues for a collection track

    // The browser's lists. ``kind``: PLAYLIST, HISTORY, ARTIST, ALBUM, KEY.
    std::vector<Category> categories(const QString &kind, const QString &parent = {});
    // A page of tracks: (total, rows). ``scope`` = (kind, list id), ``sort`` Title/Artist/BPM/Key/Time/Order.
    std::pair<int, std::vector<CatalogTrack>> browse(const QString &search, const QString &sort, bool descending,
                                                     int offset, int limit, const std::optional<std::pair<QString, QString>> &scope = {});
    std::optional<Located> locate(const std::vector<uint32_t> &ids, int rows);
    SourceStats stats();

private:
    Library() = default;
    bool has(const QString &table, const QStringList &columns);
    sqlite3 *db_ = nullptr;
    bool device_ = false;
    QString path_;
};

QString masterDatabase();                   // where Rekordbox keeps master.db
QStringList stickLibraries();               // exportLibrary.db on every mounted local drive
QString volumeName(const QString &root);    // a stick's name as the computer shows it
std::vector<std::pair<QString, QString>> sources();  // ("Collection", ""), then (name, exportLibrary.db) per stick
// A collection track's analysis folder sits in Rekordbox's share folder (its cues are in master.db);
// a stick's sits on the stick (its cues are in its ANLZ files).
bool inCollection(const QString &analysisFolder);
QByteArray databaseKey(bool device);        // the key Rekordbox encrypts master.db (or a stick) with

}  // namespace wave
