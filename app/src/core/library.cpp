#include "core/library.h"

#include "core/clock.h"

#include <QDir>
#include <QFile>
#include <QDateTime>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QStorageInfo>
#include <QUrl>

#include <sqlite3.h>

#include <algorithm>
#include <set>
#include <tuple>

#ifdef Q_OS_WIN
#include <windows.h>
#endif

namespace wave {
namespace {

// Rekordbox's database keys, as pyrekordbox (MIT) publishes them, kept XOR-ed with its BLOB_KEY.
constexpr unsigned char kMasterKey[] = {0x02, 0x05, 0x05, 0x00, 0x50, 0x0c, 0x5e, 0x0a, 0x57, 0x50, 0x0c, 0x0b, 0x06, 0x54, 0x00, 0x02, 0x03, 0x53, 0x51, 0x07, 0x0c, 0x5e, 0x00, 0x5a, 0x0c, 0x00, 0x03, 0x57, 0x0e, 0x50, 0x52, 0x05, 0x05, 0x57, 0x00, 0x52, 0x0d, 0x5d, 0x51, 0x5c, 0x07, 0x52, 0x01, 0x57, 0x51, 0x54, 0x02, 0x09, 0x07, 0x02, 0x04, 0x54, 0x55, 0x09, 0x00, 0x5e, 0x00, 0x50, 0x02, 0x03, 0x0f, 0x57, 0x5a, 0x06};
constexpr unsigned char kDeviceKey[] = {0x44, 0x0d, 0x50, 0x02, 0x50, 0x56, 0x14, 0x0c, 0x5f, 0x5b, 0x00, 0x04, 0x0f, 0x50, 0x53, 0x50, 0x44, 0x03, 0x54, 0x17, 0x4e, 0x5a, 0x0d, 0x53, 0x04, 0x06, 0x58, 0x05, 0x46, 0x1a, 0x17, 0x5c, 0x54, 0x06, 0x43, 0x14, 0x56, 0x5a, 0x1e, 0x00, 0x04, 0x56, 0x5e, 0x5e, 0x00, 0x57, 0x15, 0x54, 0x4c, 0x03, 0x03, 0x0f, 0x01, 0x57, 0x5e, 0x5e, 0x5a, 0x11, 0x45, 0x41, 0x4e, 0x12, 0x0f, 0x42};

template <size_t N> QByteArray reveal(const unsigned char (&key)[N]) {
    static const char blob[] = "657f48f84c437cc1";
    QByteArray out(static_cast<int>(N), Qt::Uninitialized);
    for (size_t i = 0; i < N; ++i) out[int(i)] = char(key[i] ^ uchar(blob[i % 16]));
    return out;
}

// A prepared statement, bound and stepped; finalised when it goes.
class Query {
public:
    Query(sqlite3 *db, const QString &sql) {
        const QByteArray text = sql.toUtf8();
        if (sqlite3_prepare_v2(db, text.constData(), int(text.size()), &stmt_, nullptr) != SQLITE_OK) stmt_ = nullptr;
    }
    ~Query() { sqlite3_finalize(stmt_); }
    bool ok() const { return stmt_ != nullptr; }
    Query &bind(const QVariant &value) {
        ++index_;
        if (!stmt_) return *this;
        if (value.typeId() == QMetaType::Int || value.typeId() == QMetaType::LongLong)
            sqlite3_bind_int64(stmt_, index_, value.toLongLong());
        else {
            const QByteArray text = value.toString().toUtf8();
            sqlite3_bind_text(stmt_, index_, text.constData(), int(text.size()), SQLITE_TRANSIENT);
        }
        return *this;
    }
    bool next() { return stmt_ && sqlite3_step(stmt_) == SQLITE_ROW; }
    QString text(int c) const {
        const auto *p = reinterpret_cast<const char *>(sqlite3_column_text(stmt_, c));
        return p ? QString::fromUtf8(p) : QString();
    }
    double real(int c) const { return sqlite3_column_double(stmt_, c); }
    qint64 integer(int c) const { return sqlite3_column_int64(stmt_, c); }

private:
    sqlite3_stmt *stmt_ = nullptr;
    int index_ = 0;
};

const QString kEncrypted = QStringLiteral("'$A%:v%:%'");  // Rekordbox keeps streaming metadata encrypted
// A stick's length is seconds as Rekordbox 7.2 exports it, milliseconds in older exports.
const QString kDeviceSeconds = QStringLiteral("CASE WHEN coalesce(c.length,0)>36000 THEN c.length/1000.0 ELSE coalesce(c.length,0) END");

QString pioneerFolder() {
#ifdef Q_OS_WIN
    return QDir(qEnvironmentVariable("APPDATA", QDir::homePath())).filePath("Pioneer");
#else
    return QDir::home().filePath("Library/Application Support/Pioneer");
#endif
}

QString joined(const QString &root, QString relative) {
    relative.replace('\\', '/');
    while (relative.startsWith('/')) relative.remove(0, 1);
    return QDir(root).filePath(relative);
}

}  // namespace

QString masterDatabase() {
    const QString override = qEnvironmentVariable("RKBX_WAVE_MASTER_DB");  // tests, and a library kept elsewhere
    if (!override.isEmpty()) return override;
    // Rekordbox names it in its agent's options.json; master.db beside it otherwise.
    QFile options(QDir(pioneerFolder()).filePath("rekordboxAgent/storage/options.json"));
    if (options.open(QIODevice::ReadOnly)) {
        for (const QJsonValue &pair : QJsonDocument::fromJson(options.readAll()).object().value("options").toArray()) {
            const QJsonArray kv = pair.toArray();
            if (kv.size() == 2 && kv[0].toString() == QLatin1String("db-path") && QFileInfo::exists(kv[1].toString()))
                return kv[1].toString();
        }
    }
    return QDir(pioneerFolder()).filePath("rekordbox/master.db");
}

bool inCollection(const QString &analysisFolder) {
    const QString share = QFileInfo(QFileInfo(masterDatabase()).absolutePath() + "/share").canonicalFilePath();
    const QString folder = QFileInfo(analysisFolder).canonicalFilePath();
    return !share.isEmpty() && folder.startsWith(share + '/', Qt::CaseInsensitive);
}

QStringList stickLibraries() {
    // Local drives only: a network share can take seconds to answer, or never.
    static const std::set<QString> network = {"smbfs", "nfs", "afpfs", "webdav", "cifs", "ftp", "macfuse", "osxfuse", "fuse"};
    QStringList found;
    for (const QStorageInfo &volume : QStorageInfo::mountedVolumes()) {
        if (!volume.isValid() || !volume.isReady()) continue;
        const QString root = volume.rootPath();
#ifdef Q_OS_WIN
        if (GetDriveTypeW(reinterpret_cast<LPCWSTR>(QDir::toNativeSeparators(root).utf16())) == DRIVE_REMOTE) continue;
#else
        if (network.count(QString::fromUtf8(volume.fileSystemType()).toLower()) || root == "/" || root.startsWith("/System/")) continue;
#ifdef Q_OS_MACOS
        if (QFileInfo(root).absolutePath() != QLatin1String("/Volumes")) continue;  // other mounts can stall
#endif
#endif
        const QString path = QDir(root).filePath("PIONEER/rekordbox/exportLibrary.db");
        if (QFileInfo(path).isFile()) found << path;
    }
    return found;
}

QString volumeName(const QString &root) {
    const QStorageInfo volume(root);
    if (!volume.name().isEmpty()) return volume.name();
    const QString folder = QDir(root).dirName();
    return folder.isEmpty() || folder.endsWith(':') ? QDir::toNativeSeparators(root).remove('\\') : folder;
}

std::vector<std::pair<QString, QString>> sources() {
    std::vector<std::pair<QString, QString>> out{{QStringLiteral("Collection"), QString()}};
    for (const QString &path : stickLibraries())
        out.emplace_back(volumeName(QFileInfo(path).dir().absoluteFilePath("../..")), path);
    return out;
}

QByteArray databaseKey(bool device) { return device ? reveal(kDeviceKey) : reveal(kMasterKey); }

std::unique_ptr<Library> Library::open(const QString &source, QString *error) {
    return openFile(source.isEmpty() ? masterDatabase() : source, !source.isEmpty(), error);
}

std::unique_ptr<Library> Library::openFile(const QString &path, bool device, QString *error) {
    std::unique_ptr<Library> library(new Library);
    library->device_ = device;
    library->path_ = path;
    if (!QFileInfo(library->path_).isFile()) {
        if (error) *error = QStringLiteral("Rekordbox's library was not found");
        return nullptr;
    }
    // mode=ro is enforced by SQLite itself, rather than relying on care.
    const QByteArray uri = QUrl::fromLocalFile(QFileInfo(library->path_).absoluteFilePath()).toString(QUrl::FullyEncoded).toUtf8() + "?mode=ro";
    if (sqlite3_open_v2(uri.constData(), &library->db_, SQLITE_OPEN_READONLY | SQLITE_OPEN_URI, nullptr) != SQLITE_OK) {
        if (error) *error = QString::fromUtf8(sqlite3_errmsg(library->db_));
        return nullptr;
    }
    sqlite3_busy_timeout(library->db_, 250);
    const QByteArray key = databaseKey(library->device_);
    sqlite3_key(library->db_, key.constData(), int(key.size()));
    sqlite3_exec(library->db_, "PRAGMA query_only=ON", nullptr, nullptr, nullptr);
    if (sqlite3_exec(library->db_, "SELECT count(*) FROM sqlite_master", nullptr, nullptr, nullptr) != SQLITE_OK) {
        if (error) *error = QStringLiteral("Rekordbox's library could not be read: %1").arg(QString::fromUtf8(sqlite3_errmsg(library->db_)));
        return nullptr;
    }
    return library;
}

Library::~Library() { sqlite3_close(db_); }

QString Library::root() const {
    const QFileInfo db(path_);
    return device_ ? QDir(db.absolutePath() + "/../..").absolutePath() : db.absolutePath() + "/share";
}

bool Library::has(const QString &table, const QStringList &columns) {
    Query q(db_, QStringLiteral("SELECT name FROM pragma_table_info(?)"));
    q.bind(table);
    QStringList found;
    while (q.next()) found << q.text(0);
    if (found.isEmpty()) return false;
    for (const QString &c : columns) if (!found.contains(c)) return false;
    return true;
}

std::optional<LibraryTrack> Library::resolvePath(const QString &audioPath) {
    QString stored = audioPath;
    if (device_) {  // a stick stores paths from its own root
        const QString root = QDir(this->root()).absolutePath();
        const QString file = QDir::fromNativeSeparators(QFileInfo(audioPath).absoluteFilePath());
        if (!file.startsWith(root + '/', Qt::CaseInsensitive)) return std::nullopt;
        stored = file.mid(root.size());
    }
    const QString key = device_ && has("key", {"key_id", "name"}) && has("content", {"key_id"})
                            ? QStringLiteral("coalesce((SELECT k.name FROM \"key\" k WHERE k.key_id=c.key_id),'')") : QStringLiteral("''");
    const QString sql = device_
        ? QStringLiteral("SELECT c.content_id,c.title,coalesce(a.name,''),%1,c.bpmx100,c.path,c.analysisDataFilePath,"
                         "(SELECT count(*) FROM cue q WHERE q.content_id=c.content_id),0,'',%2,'' "
                         "FROM content c LEFT JOIN artist a ON a.artist_id=c.artist_id_artist WHERE c.path=? LIMIT 100").arg(kDeviceSeconds, key)
        : QStringLiteral("SELECT c.ID,c.Title,coalesce(a.Name,''),c.Length,c.BPM,c.FolderPath,c.AnalysisDataPath,"
                         "(SELECT count(*) FROM djmdCue q WHERE q.ContentID=c.ID),coalesce(c.DJPlayCount,0),coalesce(c.updated_at,''),"
                         "coalesce((SELECT k.ScaleName FROM djmdKey k WHERE k.ID=c.KeyID),''),coalesce(c.ImagePath,'') "
                         "FROM djmdContent c LEFT JOIN djmdArtist a ON a.ID=c.ArtistID "
                         "WHERE c.FolderPath=? AND coalesce(c.rb_local_deleted,0)=0 LIMIT 100");
    Query q(db_, sql);
    q.bind(stored);
    std::optional<LibraryTrack> best;
    std::tuple<bool, qint64, qint64, QString> bestRank{};
    while (q.next()) {
        const QString analysis = q.text(6);
        if (analysis.isEmpty()) continue;
        LibraryTrack t;
        t.id = q.text(0); t.title = q.text(1); t.artist = q.text(2);
        t.lengthSec = q.real(3); t.bpm = q.real(4) / 100; t.audioPath = q.text(5);
        t.analysisFolder = QFileInfo(joined(root(), analysis)).absolutePath();
        t.key = q.text(10);
        if (!q.text(11).isEmpty()) t.artwork = joined(root(), q.text(11));
        // Copies of one file: the one the DJ prepared, analysed, with most cues, most played, latest.
        const auto rank = std::make_tuple(QFileInfo(t.analysisFolder).isDir(), q.integer(7), q.integer(8), q.text(9));
        if (!best || rank > bestRank) { best = t; bestRank = rank; }
    }
    return best;
}

std::vector<Cue> Library::cues(const QString &analysisFolder) {
    std::vector<Cue> out;
    if (device_) return out;
    // The track's id, found once by its analysis folder (a scan of the collection); then its cues by id.
    const QString name = QFileInfo(analysisFolder).fileName();
    auto it = cueIds_.find(name);
    if (it == cueIds_.end()) {
        Query ids(db_, QStringLiteral("SELECT ID FROM djmdContent WHERE AnalysisDataPath LIKE ?"));
        ids.bind(QStringLiteral("%/") + name + QStringLiteral("/%"));
        QStringList found;
        while (ids.next()) found << ids.text(0);
        it = cueIds_.emplace(name, found).first;
    }
    if (it->second.isEmpty()) return out;
    QStringList marks;
    for (int i = 0; i < it->second.size(); ++i) marks << QStringLiteral("?");
    Query q(db_, QStringLiteral("SELECT InMsec,OutMsec,Kind FROM djmdCue WHERE ContentID IN (%1) AND coalesce(rb_local_deleted,0)=0")
                     .arg(marks.join(',')));
    for (const QString &id : it->second) q.bind(id);
    while (q.next()) {
        const qint64 in = q.integer(0), outMs = q.integer(1);
        int number = int(q.integer(2));
        number -= number > 4;  // master.db skips Kind 4: hot cues A-H are 1, 2, 3, 5, 6, 7, 8, 9
        Cue cue;
        cue.timeSec = in / 1000.0;
        cue.memory = number == 0;
        cue.number = number;
        cue.color = cueColour(cue.memory, number);
        if (outMs > in && outMs < 0xffffffffLL) cue.endSec = outMs / 1000.0;
        out.push_back(cue);
    }
    std::sort(out.begin(), out.end(), [](const Cue &a, const Cue &b) { return std::tie(a.timeSec, a.number) < std::tie(b.timeSec, b.number); });
    return out;
}

namespace {
struct Spec { QString table, id, name, parent, order, attribute; };
struct Members { QString table, list, content, order; };
}  // namespace

std::vector<Category> Library::categories(const QString &kind, const QString &parent) {
    std::vector<Category> out;
    if (kind == "PLAYLIST" || kind == "HISTORY") {
        const bool playlist = kind == "PLAYLIST";
        const Spec t = device_ ? Spec{playlist ? "playlist" : "history", playlist ? "playlist_id" : "history_id", "name",
                                      playlist ? "playlist_id_parent" : "history_id_parent", "sequenceNo", "attribute"}
                               : Spec{playlist ? "djmdPlaylist" : "djmdHistory", "ID", "Name", "ParentID", "Seq", "Attribute"};
        const Members m = device_ ? Members{playlist ? "playlist_content" : "history_content", t.id, "content_id", "sequenceNo"}
                                  : Members{playlist ? "djmdSongPlaylist" : "djmdSongHistory", playlist ? "PlaylistID" : "HistoryID", "ContentID", "TrackNo"};
        if (!has(t.table, {t.id, t.name, t.parent, t.order, t.attribute}) || !has(m.table, {m.list, m.content})) return out;
        const QString root = device_ ? QStringLiteral("coalesce(p.%1,0)=0").arg(t.parent)
                                     : QStringLiteral("coalesce(p.%1,'root') IN ('root','0','')").arg(t.parent);
        Query q(db_, QStringLiteral("SELECT p.%1,p.%2,coalesce(p.%5,0),(SELECT count(*) FROM %6 m WHERE m.%7=p.%1) "
                                    "FROM %8 p WHERE %3 AND coalesce(p.%5,0) IN (0,1) ORDER BY p.%4")
                         .arg(t.id, t.name, parent.isEmpty() ? root : QStringLiteral("p.%1=?").arg(t.parent), t.order,
                              t.attribute, m.table, m.list, t.table));
        if (!parent.isEmpty()) q.bind(parent);
        while (q.next())
            out.push_back({q.text(0), q.text(1).isEmpty() ? QStringLiteral("Untitled") : q.text(1), int(q.integer(3)), q.integer(2) == 1});
        return out;
    }
    // Every artist, album or key with tracks.
    struct Flat { QString table, id, name, column; };
    const Flat f = device_ ? (kind == "ARTIST" ? Flat{"artist", "artist_id", "name", "artist_id_artist"}
                              : kind == "ALBUM" ? Flat{"album", "album_id", "name", "album_id"} : Flat{"\"key\"", "key_id", "name", "key_id"})
                           : (kind == "ARTIST" ? Flat{"djmdArtist", "ID", "Name", "ArtistID"}
                              : kind == "ALBUM" ? Flat{"djmdAlbum", "ID", "Name", "AlbumID"} : Flat{"djmdKey", "ID", "ScaleName", "KeyID"});
    const QString contentTable = device_ ? "content" : "djmdContent";
    if (!has(QString(f.table).remove('"'), {f.id, f.name}) || !has(contentTable, {f.column})) return out;
    const QString condition = device_ ? QStringLiteral("1=1")
                                      : QStringLiteral("coalesce(c.rb_local_deleted,0)=0 AND c.Title NOT LIKE %1").arg(kEncrypted);
    Query q(db_, QStringLiteral("SELECT x.%1,x.%2,count(*) FROM %3 c JOIN %4 x ON x.%1=c.%5 WHERE %6 AND trim(coalesce(x.%2,''))<>'' "
                                "AND x.%2 NOT LIKE %7 GROUP BY x.%1 ORDER BY x.%2 COLLATE NOCASE")
                     .arg(f.id, f.name, contentTable, f.table, f.column, condition, kEncrypted));
    while (q.next()) out.push_back({q.text(0), q.text(1), int(q.integer(2)), false});
    return out;
}

std::pair<int, std::vector<CatalogTrack>> Library::browse(const QString &search, const QString &sort, bool descending, int offset,
                                                          int limit, const std::optional<std::pair<QString, QString>> &scope) {
    std::vector<CatalogTrack> rows;
    const bool listOrder = sort == QLatin1String("Order");
    if (listOrder && !(scope && (scope->first == "PLAYLIST" || scope->first == "HISTORY"))) return {0, rows};
    if (offset < 0 || limit < 1 || limit > 500) return {0, rows};
    QString fields, tables, condition, identity;
    QMap<QString, QString> sorts;
    if (device_) {
        const bool keyed = has("key", {"key_id", "name"}) && has("content", {"key_id"});
        const bool imaged = has("image", {"image_id", "path"}) && has("content", {"image_id"});
        fields = QStringLiteral("c.content_id,c.title,coalesce(a.name,''),coalesce(c.bpmx100,0)/100.0,coalesce(%1,''),%2,c.path,coalesce(%3,'')")
                     .arg(keyed ? "k.name" : "''", kDeviceSeconds, imaged ? "i.path" : "''");
        tables = QStringLiteral("content c LEFT JOIN artist a ON a.artist_id=c.artist_id_artist");
        if (keyed) tables += QStringLiteral(" LEFT JOIN \"key\" k ON k.key_id=c.key_id");
        if (imaged) tables += QStringLiteral(" LEFT JOIN image i ON i.image_id=c.image_id");
        condition = QStringLiteral("1=1");
        sorts = {{"Title", "c.title"}, {"Artist", "a.name"}, {"BPM", "c.bpmx100"}, {"Key", keyed ? "k.name" : "c.title"}, {"Time", "c.length"}};
        identity = QStringLiteral("c.content_id");
    } else {
        fields = QStringLiteral("c.ID,c.Title,coalesce(a.Name,''),coalesce(c.BPM,0)/100.0,coalesce(k.ScaleName,''),coalesce(c.Length,0),"
                                "c.FolderPath,coalesce(%1,'')").arg(has("djmdContent", {"ImagePath"}) ? "c.ImagePath" : "''");
        tables = QStringLiteral("djmdContent c LEFT JOIN djmdArtist a ON a.ID=c.ArtistID LEFT JOIN djmdKey k ON k.ID=c.KeyID");
        condition = QStringLiteral("coalesce(c.rb_local_deleted,0)=0 AND c.Title NOT LIKE %1").arg(kEncrypted);
        sorts = {{"Title", "c.Title"}, {"Artist", "a.Name"}, {"BPM", "c.BPM"}, {"Key", "k.ScaleName"}, {"Time", "c.Length"}};
        identity = QStringLiteral("c.ID");
    }
    if (!listOrder && !sorts.contains(sort)) return {0, rows};
    QVariantList parameters;
    QString order;
    if (scope) {
        const auto &[kind, id] = *scope;
        if (kind == "PLAYLIST" || kind == "HISTORY") {
            const bool playlist = kind == "PLAYLIST";
            const Members m = device_ ? Members{playlist ? "playlist_content" : "history_content", playlist ? "playlist_id" : "history_id", "content_id", "sequenceNo"}
                                      : Members{playlist ? "djmdSongPlaylist" : "djmdSongHistory", playlist ? "PlaylistID" : "HistoryID", "ContentID", "TrackNo"};
            if (!has(m.table, {m.list, m.content, m.order})) return {0, rows};
            tables += QStringLiteral(" JOIN %1 m ON m.%2=%3").arg(m.table, m.content, identity);
            condition += QStringLiteral(" AND m.%1=?").arg(m.list);
            order = QStringLiteral("m.") + m.order;
        } else {
            const QString column = device_ ? (kind == "ARTIST" ? "artist_id_artist" : kind == "ALBUM" ? "album_id" : "key_id")
                                           : (kind == "ARTIST" ? "ArtistID" : kind == "ALBUM" ? "AlbumID" : "KeyID");
            condition += QStringLiteral(" AND c.%1=?").arg(column);
        }
        parameters << id;
    }
    for (const QString &word : search.split(' ', Qt::SkipEmptyParts)) {
        // Literal substring search: % and _ typed on the keyboard are not wildcards.
        QString pattern = word;
        pattern.replace('\\', "\\\\").replace('%', "\\%").replace('_', "\\_");
        pattern = '%' + pattern + '%';
        condition += QStringLiteral(" AND (%1 LIKE ? ESCAPE '\\' OR %2 LIKE ? ESCAPE '\\')").arg(sorts["Title"], sorts["Artist"]);
        parameters << pattern << pattern;
    }
    int total = 0;
    {
        Query count(db_, QStringLiteral("SELECT count(*) FROM %1 WHERE %2").arg(tables, condition));
        for (const QVariant &p : parameters) count.bind(p);
        if (count.next()) total = int(count.integer(0));
    }
    // Only fixed identifiers enter the SQL; search text is always bound.
    const QString field = listOrder ? order : sorts[sort] + QStringLiteral(" COLLATE NOCASE");
    Query q(db_, QStringLiteral("SELECT %1 FROM %2 WHERE %3 ORDER BY %4 %5, %6 COLLATE NOCASE, %7 LIMIT ? OFFSET ?")
                     .arg(fields, tables, condition, field, descending ? "DESC" : "ASC", sorts["Title"], identity));
    for (const QVariant &p : parameters) q.bind(p);
    q.bind(qint64(limit)).bind(qint64(offset));
    while (q.next()) {
        CatalogTrack t{q.text(0), q.text(1), q.text(2), q.real(3), q.text(4), q.real(5), q.text(6), QString()};
        if (!q.text(7).isEmpty()) t.artwork = joined(root(), q.text(7));
        rows.push_back(t);
    }
    return {total, rows};
}

std::map<uint32_t, CatalogTrack> Library::tracks(const std::vector<uint32_t> &ids) {
    std::map<uint32_t, CatalogTrack> found;
    std::vector<uint32_t> unique;
    for (uint32_t id : ids) if (id) unique.push_back(id);
    std::sort(unique.begin(), unique.end());
    unique.erase(std::unique(unique.begin(), unique.end()), unique.end());
    for (size_t at = 0; at < unique.size(); at += 200) {
        const size_t n = std::min<size_t>(200, unique.size() - at);
        QStringList marks;
        for (size_t i = 0; i < n; ++i) marks << QStringLiteral("?");
        QString sql;
        if (device_) {
            const bool keyed = has("key", {"key_id", "name"}) && has("content", {"key_id"});
            const bool imaged = has("image", {"image_id", "path"}) && has("content", {"image_id"});
            sql = QStringLiteral("SELECT c.content_id,c.title,coalesce(a.name,''),coalesce(c.bpmx100,0)/100.0,coalesce(%1,''),%2,c.path,"
                                 "coalesce(%3,'') FROM content c LEFT JOIN artist a ON a.artist_id=c.artist_id_artist%4%5 "
                                 "WHERE c.content_id IN (%6)")
                      .arg(keyed ? "k.name" : "''", kDeviceSeconds, imaged ? "i.path" : "''",
                           keyed ? " LEFT JOIN \"key\" k ON k.key_id=c.key_id" : "", imaged ? " LEFT JOIN image i ON i.image_id=c.image_id" : "",
                           marks.join(','));
        } else {
            sql = QStringLiteral("SELECT c.ID,c.Title,coalesce(a.Name,''),coalesce(c.BPM,0)/100.0,coalesce(k.ScaleName,''),coalesce(c.Length,0),"
                                 "c.FolderPath,coalesce(%1,'') FROM djmdContent c LEFT JOIN djmdArtist a ON a.ID=c.ArtistID "
                                 "LEFT JOIN djmdKey k ON k.ID=c.KeyID WHERE c.ID IN (%2)")
                      .arg(has("djmdContent", {"ImagePath"}) ? "c.ImagePath" : "''", marks.join(','));
        }
        Query q(db_, sql);
        for (size_t i = 0; i < n; ++i) q.bind(qint64(unique[at + i]));
        while (q.next()) {
            CatalogTrack t{q.text(0), q.text(1), q.text(2), q.real(3), q.text(4), q.real(5), q.text(6), QString()};
            if (!q.text(7).isEmpty()) t.artwork = joined(root(), q.text(7));
            found[q.text(0).toUInt()] = t;
        }
    }
    return found;
}

std::optional<Located> Library::locate(const std::vector<uint32_t> &ids, int rows) {
    std::vector<uint32_t> unique;
    for (uint32_t id : ids) if (id) unique.push_back(id);
    std::sort(unique.begin(), unique.end());
    unique.erase(std::unique(unique.begin(), unique.end()), unique.end());
    if (unique.size() > 40) unique.resize(40);
    if (unique.empty() || rows <= 0) return std::nullopt;
    QStringList marks;
    for (size_t i = 0; i < unique.size(); ++i) marks << QStringLiteral("?");
    for (const QString &kind : {QStringLiteral("PLAYLIST"), QStringLiteral("HISTORY")}) {
        const bool playlist = kind == "PLAYLIST";
        const Spec t = device_ ? Spec{playlist ? "playlist" : "history", playlist ? "playlist_id" : "history_id", "name",
                                      playlist ? "playlist_id_parent" : "history_id_parent", "", ""}
                               : Spec{playlist ? "djmdPlaylist" : "djmdHistory", "ID", "Name", "ParentID", "", ""};
        const Members m = device_ ? Members{playlist ? "playlist_content" : "history_content", t.id, "content_id", ""}
                                  : Members{playlist ? "djmdSongPlaylist" : "djmdSongHistory", playlist ? "PlaylistID" : "HistoryID", "ContentID", ""};
        if (!has(t.table, {t.id, t.name, t.parent}) || !has(m.table, {m.list, m.content})) continue;
        Query q(db_, QStringLiteral("SELECT m.%1 FROM %2 m WHERE CAST(m.%3 AS INTEGER) IN (%4) GROUP BY m.%1 "
                                    "HAVING count(DISTINCT CAST(m.%3 AS INTEGER))=? AND (SELECT count(*) FROM %2 n WHERE n.%1=m.%1)=?")
                         .arg(m.list, m.table, m.content, marks.join(',')));
        for (uint32_t id : unique) q.bind(qint64(id));
        q.bind(qint64(unique.size())).bind(qint64(rows));
        QStringList candidates;
        while (q.next()) candidates << q.text(0);
        if (candidates.size() != 1) continue;  // none, or several lists alike: don't guess
        Located found{kind, candidates[0], {}, {}};
        auto look = [&](const QString &id, QString *name, QString *parent) {
            Query l(db_, QStringLiteral("SELECT %1,%2 FROM %3 WHERE %4=?").arg(t.name, t.parent, t.table, t.id));
            l.bind(id);
            if (!l.next()) return false;
            *name = l.text(0).isEmpty() ? QStringLiteral("Untitled") : l.text(0);
            *parent = l.text(1);
            return true;
        };
        QString parent;
        if (!look(found.id, &found.name, &parent)) continue;
        std::set<QString> seen;
        while (!parent.isEmpty() && parent != "root" && parent != "0" && !seen.count(parent)) {
            seen.insert(parent);
            QString name, above;
            if (!look(parent, &name, &above)) break;
            found.trail.insert(found.trail.begin(), {parent, name});
            parent = above;
        }
        return found;
    }
    const auto [total, none] = browse({}, QStringLiteral("Title"), false, 0, 1);
    if (std::abs(total - rows) <= rows / 50) return Located{QStringLiteral("TRACK"), {}, {}, {}};
    return std::nullopt;
}

SourceStats Library::stats() {
    SourceStats s;
    s.tracks = browse({}, QStringLiteral("Title"), false, 0, 1).first;
    const QString table = device_ ? "playlist" : "djmdPlaylist", attribute = device_ ? "attribute" : "Attribute";
    if (has(table, {attribute})) {
        Query q(db_, QStringLiteral("SELECT count(*) FROM %1 WHERE coalesce(%2,0)=0").arg(table, attribute));
        if (q.next()) s.playlists = int(q.integer(0));
    }
    if (device_) {
        const QStorageInfo volume(root());
        s.total = volume.bytesTotal();
        s.free = volume.bytesAvailable();
    }
    return s;
}

std::optional<LibraryTrack> Libraries::resolvePath(const QString &audioPath) {
    if (!main_ && now() >= nextOpen_) {
        main_ = Library::open({});
        if (!main_) nextOpen_ = now() + 5;  // no collection yet (a fresh install): look again later
    }
    if (main_) if (auto track = main_->resolvePath(audioPath)) return track;
    // Sticks mounted now, reopened when a stick's library changes.
    const QStringList paths = stickLibraries();
    for (auto it = sticks_.begin(); it != sticks_.end();) it = paths.contains(it->first) ? std::next(it) : sticks_.erase(it);
    for (const QString &path : paths) {
        const qint64 modified = QFileInfo(path).lastModified().toMSecsSinceEpoch();
        auto &stick = sticks_[path];
        if (!stick.library || stick.modified != modified) {
            stick.library = Library::open(path);  // an unreadable or older-format stick is skipped, never fatal
            stick.modified = modified;
        }
        if (stick.library) if (auto track = stick.library->resolvePath(audioPath)) return track;
    }
    return std::nullopt;
}

std::vector<Cue> Libraries::cues(const QString &analysisFolder) {
    if (!main_) main_ = Library::open({});
    return main_ ? main_->cues(analysisFolder) : std::vector<Cue>{};
}

void Libraries::close() {
    main_.reset();
    sticks_.clear();
}

}  // namespace wave
