// Rekordbox Wave bridge: an OpenSSL provider that Rekordbox loads when started with
// OPENSSL_CONF pointing at Wave's config. It provides no algorithms. On one exact,
// verified Rekordbox build it reads deck state through Rekordbox's own engine getters,
// on Rekordbox's main (JUCE message) thread, and publishes it to a private shared file.
// Anything unexpected - another build, another architecture, changed code - and it
// does nothing but report why.
#include <openssl/core.h>
#include <openssl/core_dispatch.h>
#include <openssl/core_names.h>
#include <openssl/params.h>

#include <dispatch/dispatch.h>
#include <dlfcn.h>
#include <fcntl.h>
#include <libproc.h>
#include <mach-o/dyld.h>
#include <mach-o/loader.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>

#include "bridge_layout.h"

namespace {

// Rekordbox 7.2.19.0342, arm64 slice. Static addresses; runtime = static + ASLR slide.
[[maybe_unused]] constexpr uint8_t kUuid[16] = {0xD8, 0x27, 0xBD, 0x18, 0x69, 0x18, 0x35, 0x7A,
                               0x9F, 0x57, 0xC5, 0x06, 0xCF, 0x42, 0x87, 0x00};
constexpr uint64_t kEngineHolder = 0x1056c7a60ULL;  // djengine::DjEngineIF::singletonHolder
constexpr uint64_t kHolderInstance = 0x40;          // SingletonHolder::instance
constexpr uint64_t kEngineGraph = 0xc8;             // DjEngineIF -> rbxfrm::DjUnitAudioGraph*
constexpr uint64_t kDatabaseHolder = 0x1056ceba8ULL; // db::DatabaseIF::singletonHolder
constexpr size_t kTrackSize = 0x4e0;                 // sizeof(db::RowDataTrack), from its operator new sites
constexpr uint64_t kTrackBasicInfoSlot = 0xc8;       // vtable slot WikiBrowse::loadTrack calls
constexpr int kCollection = 1;                       // db::_browse_type the browser uses for the collection
constexpr int kDevice = 2;                           // ... and for a device, as its drag-and-drop loader passes it
constexpr uint64_t kMainComponent = 0x10546d800ULL;  // MainComponent::getInstance's static
constexpr uint64_t kUiManager = 0x568;               // MainComponent -> djplay::UiManager*
constexpr uint64_t kLoadMessageVtable = 0x10517e590ULL;  // djplay::DjplayCommonMessage address point
constexpr uint64_t kBrowseView = 0x1053fd840ULL;         // BrowseBasicView::getInstance's static
[[maybe_unused]] constexpr uint64_t kLoadMessageDtor = 0x1002e24a0ULL;    // its first slot, checked before use

struct Code { uint64_t address, size, hash; };
enum : int { NUM_PLAYERS, IS_LOADED, IS_PLAYING, POSITION_MS, TOTAL_MS, FILE_PATH, TEMPO, BPM,
             RUNNING_LOOP, IS_LOOPING, STRING_DTOR, ENGINE_HOLDER_GET, IS_LOADING, TRACK_CTOR,
             TRACK_DTOR, GET_ROW_DATA_TRACK, DATABASE_HOLDER_GET, TRACK_BASIC_INFO, MAIN_COMPONENT, UI_PLAYER,
             POST_MESSAGE, MESSAGE_CTOR, BROWSER_LOAD, WIKI_LOAD, MASTER_TEMPO, GET_MASTER, IS_SYNC,
             KEY_SHIFT, IS_SLIP, KEY_SYNCING, SET_MASTER_TEMPO, TEMPO_SELECT, ALL_DEVICES, DEVICE_ROOT,
             BROWSE_INSTANCE, FOCUSED_LIST, FOCUSED_TREE, GET_LIST, GET_TREE, NUM_ROWS, LIST_TRACK, SELECTED_IDS,
             SELECTED_ROWS, FILE_EXTENSION, TREE_PATH, HAS_FOCUS, CODE_COUNT };
// FNV-1a of each function's first bytes. Entries never called pin layouts the bridge relies on:
// the *_HOLDER_GET ones the holder's instance at +0x40, BROWSER_LOAD and WIKI_LOAD (hashed whole)
// the load message the bridge posts as Rekordbox's own browser does.
constexpr Code kCode[CODE_COUNT] = {
    {0x1027ce324, 0x20, 0xd965db31f8e20c84ULL},  // DjUnitAudioGraph::getNumOfPlayer
    {0x100912df8, 0x20, 0x93984f40e266566cULL},  // DjEngineIF::isLoaded
    {0x100912f08, 0x20, 0x3d59d338708c08d8ULL},  // DjEngineIF::isPlaying
    {0x100913114, 0x20, 0xc0551f2d8edc6db7ULL},  // DjEngineIF::getPlayingTimeFloatMs
    {0x1009131c8, 0x20, 0xcf64db6490f786fbULL},  // DjEngineIF::getTotalTime
    {0x100913200, 0x20, 0xb43c94019bfe1d64ULL},  // DjEngineIF::getFilePath
    {0x100913440, 0x20, 0x82ae1d8459ec42dcULL},  // DjEngineIF::getTempo
    {0x100913c20, 0x20, 0x561f1ada66c55ebcULL},  // DjEngineIF::getBpm
    {0x100915e9c, 0x20, 0x1aa321eb52a46757ULL},  // DjEngineIF::getRunningLoop
    {0x100916298, 0x20, 0xe3d65d654f365413ULL},  // DjEngineIF::isLooping
    {0x1024aa6fc, 0x20, 0xb9338aadc2040a0cULL},  // juce::String::~String
    {0x10001b524, 0x20, 0xab1a24870bdc25e0ULL},  // SingletonHolder<DjEngineIF>::get
    {0x100912e2c, 0x20, 0x93f94e76b52fa825ULL},  // DjEngineIF::isLoading
    {0x1000b6ab8, 0x20, 0xe301719bfc127e9eULL},  // db::RowDataTrack::RowDataTrack()
    {0x1000b04d0, 0x20, 0xce5a939f3520f759ULL},  // db::RowDataTrack::~RowDataTrack()
    {0x1020d6f48, 0x20, 0x387e1aedab58d656ULL},  // db::DatabaseIF::getRowDataTrack(uint, RowDataTrack&, _browse_type, long)
    {0x100037d38, 0x20, 0xab1a24870bdc25e0ULL},  // SingletonHolder<DatabaseIF>::get
    {0x1000b1650, 0x20, 0x427bb6eb9a79e0d3ULL},  // db::RowDataTrack::getBasicInfo: a fresh copy the load message owns
    {0x10094f780, 0x20, 0xa94ae279c506e460ULL},  // MainComponent::getInstance
    {0x1018a9918, 0x20, 0x5c1ff637f9399fffULL},  // djplay::UiManager::getPlayer(ePlayerID)
    {0x101477290, 0x20, 0x189711618b6a6d0aULL},  // common::CommonMessageReceiver::postCommonMessage
    {0x101c027e0, 0x20, 0xc12a23c3d3906445ULL},  // common::Message::Message(int, int, int, void*)
    {0x100412500, 0x43c, 0x86c28178e21d579dULL}, // browse::ListViewer::loadTrack(RowDataTrack&, ePlayerID, int, bool, bool)
    {0x1007c5c28, 0xc0, 0xb7ab0b571b452336ULL},  // browse::WikiBrowse::loadTrack(ePlayerID, RowDataTrack&)
    {0x100913800, 0x20, 0xe67c1bb79dbf6d9cULL},  // DjEngineIF::isMasterTempoMode
    {0x100914508, 0x20, 0xb25848828c07663bULL},  // DjEngineIF::getMaster: the master channel, or -1
    {0x1009145ec, 0x20, 0x984e7fc0f5fef17fULL},  // DjEngineIF::isSync
    {0x100913b38, 0x20, 0x82c7eb1437046ef1ULL},  // DjEngineIF::getShiftedPlayKeySemitone
    {0x1009173ac, 0x20, 0x383b8b5bba4f4062ULL},  // DjEngineIF::isSlipMode
    {0x100914818, 0x20, 0x265dbf9aa155a2e2ULL},  // DjEngineIF::isKeySyncing
    {0x1009137c8, 0x20, 0x912087053766f180ULL},  // DjEngineIF::setMasterTempoMode(channel, bool)
    {0x101ac111c, 0x20, 0x7afac00a87d061adULL},  // djplay::UiPlayer::getTempoSelectStatus
    {0x1020d5b18, 0x20, 0x30ca9940e88081c1ULL},  // db::DatabaseIF::getAllDeviceHandle(Array<long>&)
    {0x1020d5b04, 0x20, 0xadbcf040411634e8ULL},  // db::DatabaseIF::getDeviceRoot(long)
    // Rekordbox's browser. Whole bodies pin the layout read below: the view's list (+0x268) and tree
    // (+0x270) managers, the list's row count (+0x258), list box (+0x238), sort map (+0x240/+0x24c)
    // and tracks (+0x268), the list box's selection (+0x138/+0x144), a track's id (+0x8) and path (+0xa8).
    {0x100313584, 0x20, 0xe0fc993b33070a96ULL},  // browse::BrowseBasicView::getInstance
    {0x10032d25c, 0x1b4, 0x95410468c120a585ULL}, // browse::BrowseBasicView::getFocusedListView
    {0x10032db0c, 0xa0, 0x278bcb503d4c6af7ULL},  // browse::BrowseBasicView::getFocusedTreeView
    {0x101ce9864, 0x20, 0xce1181edef2b5ae6ULL},  // browse::BrowseListManager::getList
    {0x101c789d8, 0x20, 0x145234816f0475acULL},  // browse::BrowseTreeManager::getTree
    {0x100213a98, 0x20, 0xe2249b11b00160b3ULL},  // browse::ListViewer::getNumRows
    {0x1003ecb98, 0x20, 0x89d894fa77049d59ULL},  // browse::ListViewer::getCurrentListTrack
    {0x1003e4068, 0x26c, 0xc16e5bde4f104845ULL}, // browse::ListViewer::getSelectedTrackIDs
    {0x100e7bb74, 0x7c, 0x042df1eb3b6ed0a5ULL},  // browse::CustomListBox::getSelectedRows
    {0x1003ec428, 0x20, 0x00b44b89932e8e9fULL},  // db::RowDataTrack::getFileExtension
    {0x10155fb88, 0x20, 0xfa043bbf6aa07fa2ULL},  // browse::TreeViewer::getCurrentItemPath
    {0x1026561bc, 0x20, 0x28a3e31698861815ULL},  // juce::Component::hasKeyboardFocus
};

struct JString {  // juce::String: one pointer to its UTF-8 text; non-trivial, so returned via x8
    const char *text;
    ~JString();
};
struct RunningLoop { int64_t in, out; };  // djengine::StCueInfo as getRunningLoop returns it

using NumPlayers = int (*)(const void *graph);
using DeckBool = bool (*)(const void *engine, int channel);
using DeckFlag = bool (*)(const void *engine, int channel, bool);
using DeckFloat = float (*)(const void *engine, int channel);
using DeckFloatFlag = float (*)(const void *engine, int channel, bool);
using DeckInt = int (*)(const void *engine, int channel);
using DeckPath = JString (*)(const void *engine, int channel);
using DeckLoop = RunningLoop (*)(const void *engine, int channel);
using StringDtor = void (*)(JString *);
using TrackFn = void (*)(void *track);
using GetRowDataTrack = bool (*)(void *database, uint32_t id, void *track, int browse_type, long);
using EngineInt = int (*)(const void *engine);
using BasicInfo = void *(*)(void *track);
using UiPlayer = void *(*)(const void *ui_manager, int player);
using MessageCtor = void (*)(void *message, int, int, int, void *);
using PostMessage = void (*)(void *receiver, void *message);
struct LongArray { int64_t *data; int32_t allocated, used; };  // juce::Array<long>; its storage is malloc'd
using SetDeckFlag = void (*)(const void *engine, int channel, bool);
using PlayerInt = int (*)(const void *ui_player);
using AllDevices = void (*)(void *database, LongArray *);
using DeviceRoot = JString (*)(void *database, long handle);
using GetComponent = void *(*)(const void *manager);
using ItemPath = JString (*)(void *tree);
using HasFocus = bool (*)(const void *component, bool children);

struct Api {
    uintptr_t slide = 0;
    NumPlayers num_players;
    DeckBool is_loaded, is_playing;
    DeckFloat position_ms, tempo;
    DeckInt total_ms;
    DeckPath file_path;
    DeckFloatFlag bpm;
    DeckLoop running_loop;
    DeckFlag is_looping;
    StringDtor string_dtor;
    DeckBool is_loading;
    TrackFn track_ctor, track_dtor;
    GetRowDataTrack get_row_data_track;
    UiPlayer ui_player;
    DeckBool master_tempo, is_sync, is_slip, key_syncing;
    DeckInt key_shift;
    EngineInt master;
    SetDeckFlag set_master_tempo;
    PlayerInt tempo_select;
    AllDevices all_devices;
    DeviceRoot device_root;
    GetComponent get_list;
    ItemPath tree_path;
    HasFocus has_focus;
    MessageCtor message_ctor;
    PostMessage post_message;
} api;

JString::~JString() { api.string_dtor(this); }

BridgeState *shared = nullptr;
dispatch_source_t timer = nullptr;
uint64_t ticks = 0;

template <typename F> F at(int index) { return reinterpret_cast<F>(kCode[index].address + api.slide); }

uint64_t fnv(const uint8_t *p, size_t n) {
    uint64_t h = 0xcbf29ce484222325ULL;
    for (size_t i = 0; i < n; ++i) h = (h ^ p[i]) * 0x100000001b3ULL;
    return h;
}

void note(const char *text) {
    if (shared) snprintf(shared->note, sizeof shared->note, "%s", text);
}

void publish_status(uint32_t status, const char *why) {
    if (!shared) return;
    // May follow a tick that threw mid-write, so force odd rather than increment.
    __atomic_store_n(&shared->seq, __atomic_load_n(&shared->seq, __ATOMIC_ACQUIRE) | 1, __ATOMIC_RELEASE);
    shared->status = status;
    shared->deck_count = 0;
    note(why);
    __atomic_store_n(&shared->seq, (__atomic_load_n(&shared->seq, __ATOMIC_ACQUIRE) | 1) + 1, __ATOMIC_RELEASE);
}

bool open_shared() {
    const char *home = getenv("HOME");
    if (!home || !*home) return false;
    char dir[1024], path[1100];
    snprintf(dir, sizeof dir, "%s/Library/Application Support/rkbx_wave/bridge", home);
    mkdir(dir, 0700);
    snprintf(path, sizeof path, "%s/%s", dir, kBridgeStateFile);
    int fd = open(path, O_RDWR | O_CREAT | O_NOFOLLOW | O_CLOEXEC, 0600);
    if (fd < 0) return false;
    struct stat st;
    // Only ever our own private regular file.
    if (fstat(fd, &st) != 0 || !S_ISREG(st.st_mode) || st.st_uid != getuid() ||
        fchmod(fd, 0600) != 0 || ftruncate(fd, sizeof(BridgeState)) != 0) {
        close(fd);
        return false;
    }
    void *map = mmap(nullptr, sizeof(BridgeState), PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    close(fd);
    if (map == MAP_FAILED) return false;
    shared = static_cast<BridgeState *>(map);
    __atomic_store_n(&shared->seq, __atomic_load_n(&shared->seq, __ATOMIC_ACQUIRE) | 1, __ATOMIC_RELEASE);  // a reader waits until the header is whole
    memcpy(shared->magic, kBridgeMagic, sizeof shared->magic);
    shared->version = kBridgeVersion;
    shared->pid = static_cast<uint32_t>(getpid());
    shared->deck_count = 0;
    shared->status = kStatusStarting;
    memset(shared->note, 0, sizeof shared->note);
    memset(shared->decks, 0, sizeof shared->decks);
    // A request left from an earlier Rekordbox session must never run in this one.
    shared->reply = BridgeReply{};
    shared->reply.seq = shared->command.seq;
    __atomic_store_n(&shared->seq, (__atomic_load_n(&shared->seq, __ATOMIC_ACQUIRE) | 1) + 1, __ATOMIC_RELEASE);
    return true;
}

// Fails closed: the main executable must be exactly the build the addresses came from.
const char *verify_build() {
#if !defined(__aarch64__)
    return "This Rekordbox is running as Intel code; the bridge supports Apple Silicon only";
#else
    const mach_header_64 *header = reinterpret_cast<const mach_header_64 *>(_dyld_get_image_header(0));
    if (!header || header->magic != MH_MAGIC_64 || header->cputype != CPU_TYPE_ARM64)
        return "Unexpected main executable";
    const uint8_t *command = reinterpret_cast<const uint8_t *>(header + 1);
    bool uuid_ok = false;
    for (uint32_t i = 0; i < header->ncmds; ++i) {
        const load_command *lc = reinterpret_cast<const load_command *>(command);
        if (lc->cmd == LC_UUID)
            uuid_ok = !memcmp(reinterpret_cast<const uuid_command *>(lc)->uuid, kUuid, sizeof kUuid);
        command += lc->cmdsize;
    }
    if (!uuid_ok) return "Unsupported Rekordbox build: Wave's bridge supports Rekordbox 7.2.19.0342";
    api.slide = static_cast<uintptr_t>(_dyld_get_image_vmaddr_slide(0));
    for (const Code &code : kCode)
        if (fnv(reinterpret_cast<const uint8_t *>(code.address + api.slide), code.size) != code.hash)
            return "Rekordbox's code differs from the verified build; bridge disabled";
    api.num_players = at<NumPlayers>(NUM_PLAYERS);
    api.is_loaded = at<DeckBool>(IS_LOADED);
    api.is_playing = at<DeckBool>(IS_PLAYING);
    api.position_ms = at<DeckFloat>(POSITION_MS);
    api.total_ms = at<DeckInt>(TOTAL_MS);
    api.file_path = at<DeckPath>(FILE_PATH);
    api.tempo = at<DeckFloat>(TEMPO);
    api.bpm = at<DeckFloatFlag>(BPM);
    api.running_loop = at<DeckLoop>(RUNNING_LOOP);
    api.is_looping = at<DeckFlag>(IS_LOOPING);
    api.string_dtor = at<StringDtor>(STRING_DTOR);
    api.is_loading = at<DeckBool>(IS_LOADING);
    api.track_ctor = at<TrackFn>(TRACK_CTOR);
    api.track_dtor = at<TrackFn>(TRACK_DTOR);
    api.get_row_data_track = at<GetRowDataTrack>(GET_ROW_DATA_TRACK);
    api.ui_player = at<UiPlayer>(UI_PLAYER);
    api.master_tempo = at<DeckBool>(MASTER_TEMPO);
    api.master = at<EngineInt>(GET_MASTER);
    api.is_sync = at<DeckBool>(IS_SYNC);
    api.key_shift = at<DeckInt>(KEY_SHIFT);
    api.is_slip = at<DeckBool>(IS_SLIP);
    api.key_syncing = at<DeckBool>(KEY_SYNCING);
    api.set_master_tempo = at<SetDeckFlag>(SET_MASTER_TEMPO);
    api.tempo_select = at<PlayerInt>(TEMPO_SELECT);
    api.all_devices = at<AllDevices>(ALL_DEVICES);
    api.device_root = at<DeviceRoot>(DEVICE_ROOT);
    api.get_list = at<GetComponent>(GET_LIST);
    api.tree_path = at<ItemPath>(TREE_PATH);
    api.has_focus = at<HasFocus>(HAS_FOCUS);
    api.message_ctor = at<MessageCtor>(MESSAGE_CTOR);
    api.post_message = at<PostMessage>(POST_MESSAGE);
    if (*reinterpret_cast<const uint64_t *>(kLoadMessageVtable + api.slide) != kLoadMessageDtor + api.slide)
        return "Rekordbox's load message differs from the verified build; bridge disabled";
    return nullptr;
#endif
}

const void *instance(uint64_t holder_address) {
    auto holder = reinterpret_cast<const uint8_t *>(holder_address + api.slide);
    return reinterpret_cast<const std::atomic<const void *> *>(holder + kHolderInstance)->load(std::memory_order_acquire);
}
const void *engine() { return instance(kEngineHolder); }

uint64_t command_check(const BridgeCommand &c) {
    uint8_t bytes[32];
    memcpy(bytes, &c.seq, 4); memcpy(bytes + 4, &c.kind, 4); memcpy(bytes + 8, &c.deck, 4);
    memcpy(bytes + 12, &c.content_id, 4); memcpy(bytes + 16, &c.issued_ns, 8); memcpy(bytes + 24, &c.device, 8);
    return fnv(bytes, sizeof bytes);
}

// Posts the load message Rekordbox's own browser posts for a stopped deck: no autoplay
// (+0x40), an ordinary asynchronous load (+0x50), from Rekordbox's own cue point (+0x58 = -1).
// Rekordbox's message owns basic_info from here on.
void post_load(void *player, void *basic_info) {
    auto m = static_cast<uint8_t *>(::operator new(0x70));
    api.message_ctor(m, 0x1234, 0, 0, nullptr);
    *reinterpret_cast<uint32_t *>(m + 0x38) = 7;
    *reinterpret_cast<int64_t *>(m + 0x40) = 0;
    *reinterpret_cast<int64_t *>(m + 0x48) = 0x59;
    *reinterpret_cast<int64_t *>(m + 0x50) = 0;
    *reinterpret_cast<double *>(m + 0x58) = -1.0;
    *reinterpret_cast<void **>(m + 0x60) = basic_info;
    *reinterpret_cast<void **>(m + 0x68) = nullptr;
    *reinterpret_cast<uint64_t *>(m) = kLoadMessageVtable + api.slide;
    api.post_message(player, m);
}

void *ui_player(int deck) {
    const uint8_t *main = *reinterpret_cast<uint8_t *const *>(kMainComponent + api.slide);
    const void *manager = main ? *reinterpret_cast<const void *const *>(main + kUiManager) : nullptr;
    return manager ? api.ui_player(manager, deck) : nullptr;
}

// Mounted devices, refreshed every couple of seconds: Wave picks a stick's handle by its root.
void read_devices() {
    void *database = const_cast<void *>(instance(kDatabaseHolder));
    BridgeDevice found[kBridgeDevices] = {};
    if (database) {
        LongArray handles = {};
        api.all_devices(database, &handles);
        for (int i = 0; i < handles.used && i < int(kBridgeDevices); ++i) {
            found[i].handle = handles.data[i];
            JString root = api.device_root(database, static_cast<long>(handles.data[i]));
            snprintf(found[i].root, sizeof found[i].root, "%s", root.text ? root.text : "");
        }
        free(handles.data);
    }
    memcpy(shared->devices, found, sizeof found);
}

template <typename T> T field(const void *object, size_t offset) {
    return *reinterpret_cast<const T *>(static_cast<const uint8_t *>(object) + offset);
}

// Rekordbox's browser as its controller moves through it: the rows around its cursor, the
// cursor, and which pane has focus. Only reads, on the main thread that owns all of it.
void read_browser(bool with_path) {
    BridgeBrowser &b = shared->browser;
    const void *view = field<const void *>(reinterpret_cast<const void *>(kBrowseView + api.slide), 0);
    const void *lists = view ? field<const void *>(view, 0x268) : nullptr;
    const void *trees = view ? field<const void *>(view, 0x270) : nullptr;
    const void *list = lists && field<const void *>(lists, 0x108) ? api.get_list(lists) : nullptr;
    void *tree = trees ? field<void *>(trees, 0x100) : nullptr;
    b.focus = tree && api.has_focus(tree, true) ? 1 : list && api.has_focus(list, true) ? 0 : 2;
    if (tree && with_path) {
        JString path = api.tree_path(tree);
        snprintf(b.tree_path, sizeof b.tree_path, "%s", path.text ? path.text : "");
    }
    if (!list) { b.rows = -1; b.cursor = -1; b.first = 0; b.count = 0; return; }
    const int rows = field<int>(list, 0x258);
    const void *box = field<const void *>(list, 0x238);
    const int *ranges = box ? field<const int *>(box, 0x138) : nullptr;
    const int cursor = box && ranges && field<int>(box, 0x144) > 0 ? ranges[0] : -1;
    const int *order = field<const int *>(list, 0x240);
    const int ordered = field<int>(list, 0x24c);
    const void *tracks = field<const void *>(list, 0x268);
    const int stored = tracks ? field<int>(tracks, 0xc) : 0;
    const void *const *track_data = tracks ? field<const void *const *>(tracks, 0) : nullptr;
    int first = cursor < 0 ? 0 : cursor - int(kBrowseRows) / 4;
    first = first > rows - int(kBrowseRows) ? rows - int(kBrowseRows) : first;
    first = first < 0 ? 0 : first;
    int count = 0;
    for (int row = first; row < rows && count < int(kBrowseRows); ++row, ++count) {
        // As Rekordbox maps a list row to its track: through the sort order, when the row is in it.
        const int index = row < ordered && order ? order[row] : 0;
        const void *track = track_data && index >= 0 && index < stored ? track_data[index] : nullptr;
        BrowseRow &out = b.rows_[count];
        out.id = track ? field<uint32_t>(track, 0x8) : 0;
        const char *path = track ? field<const char *>(track, 0xa8) : nullptr;
        snprintf(out.path, sizeof out.path, "%s", path ? path : "");
    }
    b.rows = rows; b.cursor = cursor; b.first = first; b.count = count;
}

bool known_device(int64_t handle) {
    for (const BridgeDevice &device : shared->devices)
        if (handle && device.handle == handle) return true;
    return false;
}

// Loads a track as Rekordbox's browser does: fetch the row from the collection or a mounted
// device, then post the deck's ordinary load message. Never onto a playing or loading deck.
int load(const void *dj, int players, const BridgeCommand &c, char *message, size_t size) {
    const int deck = static_cast<int>(c.deck);
    const bool device = c.kind == kCommandLoadDevice;
    if (device && !known_device(c.device)) { snprintf(message, size, "That USB stick is no longer mounted"); return kReplyNoTrack; }
    if (deck < 0 || deck >= players) { snprintf(message, size, "Deck %d does not exist", deck + 1); return kReplyBadDeck; }
    if (api.is_playing(dj, deck)) { snprintf(message, size, "Deck %d is playing", deck + 1); return kReplyDeckPlaying; }
    if (api.is_loading(dj, deck)) { snprintf(message, size, "Deck %d is still loading", deck + 1); return kReplyDeckBusy; }
    void *database = const_cast<void *>(instance(kDatabaseHolder));
    void *player = ui_player(deck);
    if (!database || !player) { snprintf(message, size, "Rekordbox is still starting"); return kReplyNotReady; }
    alignas(16) uint8_t track[kTrackSize];
    api.track_ctor(track);
    int result;
    const auto basic_info = reinterpret_cast<BasicInfo>(
        *reinterpret_cast<uint64_t *>(*reinterpret_cast<uint8_t **>(track) + kTrackBasicInfoSlot));
    if (reinterpret_cast<uint64_t>(basic_info) != kCode[TRACK_BASIC_INFO].address + api.slide) {
        snprintf(message, size, "Rekordbox's track type differs from the verified build");
        result = kReplyUnsupported;
    } else if (!api.get_row_data_track(database, c.content_id, track, device ? kDevice : kCollection,
                                       device ? static_cast<long>(c.device) : 0)) {
        snprintf(message, size, device ? "Track %u is not on that USB stick" : "Track %u is not in Rekordbox's collection",
                 c.content_id);
        result = kReplyNoTrack;
    } else if (void *info = basic_info(track)) {
        post_load(player, info);
        snprintf(message, size, "Load requested on deck %d", deck + 1);
        result = kReplyRequested;
    } else {
        snprintf(message, size, "Rekordbox could not prepare track %u", c.content_id);
        result = kReplyNoTrack;
    }
    api.track_dtor(track);
    return result;
}

void run_command(const void *dj, int players) {
    const BridgeCommand c = shared->command;  // a copy: Wave may be writing the next one
    if (c.seq == shared->reply.seq || command_check(c) != c.check) return;  // nothing new, or half-written
    char message[sizeof shared->reply.message] = {0};
    int result;
    const uint64_t now = clock_gettime_nsec_np(CLOCK_UPTIME_RAW);
    if (c.issued_ns > now || now - c.issued_ns > 2 * NSEC_PER_SEC) {
        snprintf(message, sizeof message, "Request expired before Rekordbox could act on it");
        result = kReplyExpired;
    } else if (!dj || players <= 0) {
        snprintf(message, sizeof message, "Rekordbox is still starting");
        result = kReplyNotReady;
    } else if (c.kind == kCommandLoad || c.kind == kCommandLoadDevice) {
        result = load(dj, players, c, message, sizeof message);
    } else if (c.kind == kCommandMasterTempo && int(c.deck) < players && api.is_loaded(dj, int(c.deck))) {
        // The engine's own setter: Rekordbox's MT button and the controller's LED follow its notification.
        api.set_master_tempo(dj, int(c.deck), c.content_id != 0);
        snprintf(message, sizeof message, "Master tempo %s on deck %u", c.content_id ? "on" : "off", c.deck + 1);
        result = kReplyRequested;
    } else if (c.kind == kCommandMasterTempo) {
        snprintf(message, sizeof message, "Deck %u has no track", c.deck + 1);
        result = kReplyBadDeck;
    } else {
        snprintf(message, sizeof message, "Unknown request");
        result = kReplyBadRequest;
    }
    shared->reply.result = result;
    memcpy(shared->reply.message, message, sizeof message);
    std::atomic_thread_fence(std::memory_order_release);
    shared->reply.seq = c.seq;
}

// Runs on the main thread, where Rekordbox's own UI reads the same getters.
void tick() {
    const void *dj = engine();
    const void *graph = dj ? *reinterpret_cast<const void *const *>(static_cast<const uint8_t *>(dj) + kEngineGraph) : nullptr;
    const int players = graph ? api.num_players(graph) : 0;
    const bool read_paths = ticks % 25 == 0;
    __atomic_fetch_add(&shared->seq, 1, __ATOMIC_ACQ_REL);
    ++ticks;
    shared->ticks = ticks;
    shared->uptime_ns = clock_gettime_nsec_np(CLOCK_UPTIME_RAW);
    shared->status = players > 0 ? kStatusLive : kStatusStarting;
    shared->master = players > 0 ? api.master(dj) : -1;
    if (ticks % 200 == 1) read_devices();
    if (ticks % 5 == 0) read_browser(ticks % 10 == 0);
    shared->deck_count = static_cast<uint32_t>(players < 0 ? 0 : players > int(kBridgeDecks) ? int(kBridgeDecks) : players);
    for (uint32_t ch = 0; ch < shared->deck_count; ++ch) {
        BridgeDeck &deck = shared->decks[ch];
        const int c = static_cast<int>(ch);
        const bool loaded = api.is_loaded(dj, c);
        const bool was_loaded = deck.flags & kDeckLoaded;
        uint32_t flags = loaded ? kDeckLoaded : 0;
        if (loaded) {
            if (api.is_playing(dj, c)) flags |= kDeckPlaying;
            if (api.master_tempo(dj, c)) flags |= kDeckMasterTempo;
            if (api.is_sync(dj, c)) flags |= kDeckSync;
            if (api.is_slip(dj, c)) flags |= kDeckSlip;
            if (api.key_syncing(dj, c)) flags |= kDeckKeySync;
            deck.key_shift = api.key_shift(dj, c);
            const void *player = ui_player(c);
            deck.tempo_range = player ? api.tempo_select(player) : -1;
            deck.position_ms = api.position_ms(dj, c);
            deck.total_ms = api.total_ms(dj, c);
            deck.bpm = api.bpm(dj, c, true);
            deck.original_bpm = api.bpm(dj, c, false);
            deck.tempo = api.tempo(dj, c);
            if (api.is_looping(dj, c, false)) {
                RunningLoop loop = api.running_loop(dj, c);
                deck.loop_in = loop.in;
                deck.loop_out = loop.out;
                if (loop.out != -1 && loop.out > loop.in) flags |= kDeckLooping;
            } else {
                deck.loop_in = deck.loop_out = -1;
            }
        } else {
            deck.position_ms = 0; deck.total_ms = 0; deck.bpm = deck.original_bpm = deck.tempo = 0;
            deck.loop_in = deck.loop_out = -1;
            deck.key_shift = 0;
        }
        if (loaded && (read_paths || !was_loaded)) {
            JString path = api.file_path(dj, c);
            const char *text = path.text ? path.text : "";
            if (strncmp(text, deck.path, sizeof deck.path)) {
                snprintf(deck.path, sizeof deck.path, "%s", text);
                ++deck.path_seq;
            }
        } else if (!loaded && deck.path[0]) {
            deck.path[0] = 0;
            ++deck.path_seq;
        }
        deck.flags = flags;
    }
    __atomic_fetch_add(&shared->seq, 1, __ATOMIC_ACQ_REL);
    run_command(dj, players);
}

void start() {
    // Never unloaded while the timer can still run our code.
    Dl_info info;
    if (dladdr(reinterpret_cast<void *>(&start), &info) && info.dli_fname)
        dlopen(info.dli_fname, RTLD_NOW | RTLD_NODELETE);
    timer = dispatch_source_create(DISPATCH_SOURCE_TYPE_TIMER, 0, 0, dispatch_get_main_queue());
    dispatch_source_set_timer(timer, dispatch_time(DISPATCH_TIME_NOW, 0), 10 * NSEC_PER_MSEC, NSEC_PER_MSEC);
    dispatch_source_set_event_handler(timer, ^{
        try { tick(); } catch (...) {
            dispatch_source_cancel(timer);
            publish_status(kStatusFailed, "Rekordbox engine read failed; bridge stopped");
        }
    });
    dispatch_resume(timer);
}

bool is_rekordbox() {
    char exe[PROC_PIDPATHINFO_MAXSIZE] = {0};
    if (proc_pidpath(getpid(), exe, sizeof exe) <= 0) return false;
    const char *suffix = "/Contents/MacOS/rekordbox";
    size_t n = strlen(exe), m = strlen(suffix);
    return n >= m && !strcmp(exe + n - m, suffix);
}

char provider_name[] = "Rekordbox Wave bridge";
char provider_version[] = WAVE_BRIDGE_VERSION;
const OSSL_PARAM provider_params[] = {
    OSSL_PARAM_utf8_ptr(OSSL_PROV_PARAM_NAME, nullptr, 0),
    OSSL_PARAM_utf8_ptr(OSSL_PROV_PARAM_VERSION, nullptr, 0),
    OSSL_PARAM_END};
const OSSL_PARAM *gettable(void *) { return provider_params; }
int get_params(void *, OSSL_PARAM params[]) {
    for (OSSL_PARAM *p = params; p && p->key; ++p) {
        if (p->data_type != OSSL_PARAM_UTF8_PTR || !p->data) continue;
        if (!strcmp(p->key, OSSL_PROV_PARAM_NAME)) *static_cast<char **>(p->data) = provider_name;
        if (!strcmp(p->key, OSSL_PROV_PARAM_VERSION)) *static_cast<char **>(p->data) = provider_version;
    }
    return 1;
}
const OSSL_DISPATCH provider_dispatch[] = {
    {OSSL_FUNC_PROVIDER_GETTABLE_PARAMS, reinterpret_cast<void (*)(void)>(gettable)},
    {OSSL_FUNC_PROVIDER_GET_PARAMS, reinterpret_cast<void (*)(void)>(get_params)},
    {0, nullptr}};

}  // namespace

extern "C" __attribute__((visibility("default")))
int OSSL_provider_init(const OSSL_CORE_HANDLE *, const OSSL_DISPATCH *, const OSSL_DISPATCH **out, void **ctx) {
    *out = provider_dispatch;
    *ctx = nullptr;
    static std::atomic<bool> once{false};
    const char *enabled = getenv("RKBX_WAVE_BRIDGE");
    // Child processes inherit the environment; only Rekordbox's own main process reads state.
    if (!enabled || strcmp(enabled, "1") || !is_rekordbox() || once.exchange(true)) return 1;
    if (!open_shared()) return 1;
    if (const char *why = verify_build()) {
        publish_status(kStatusUnsupported, why);
        return 1;
    }
    start();
    return 1;
}
