// Rekordbox Wave bridge for Windows: an OpenSSL provider that Rekordbox loads when started with
// OPENSSL_CONF pointing at Wave's config. It provides no algorithms. On one exact, verified
// Rekordbox build it reads deck state through Rekordbox's own engine functions on Rekordbox's
// UI thread and shares it through a private file, as the macOS bridge does. Anything unexpected -
// another build, changed code - and it does nothing but report why. A fault inside the bridge
// stops the bridge, never Rekordbox.
//
// Addresses come from rekordbox.exe 7.2.19.0342 x64 (no symbols): each function was matched to its
// macOS counterpart by its code, and every one is hash-checked before use. MSVC x64 differs from the
// macOS build in three ways that matter here: a returned class comes back through a hidden pointer
// passed after `this`, `long` is 32-bit, and vtable slots sit 8 lower (one destructor slot, not two).
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <tlhelp32.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "../mac_bridge/bridge_layout.h"

namespace {

constexpr uint64_t kBase = 0x140000000ULL;                 // rekordbox.exe's preferred image base
constexpr uint32_t kTimestamp = 0x6A97B2E4;                // its PE TimeDateStamp
constexpr uint8_t kPdbGuid[16] = {0x3b, 0x80, 0x21, 0x25, 0x13, 0xfd, 0x6b, 0x40,
                                  0x9c, 0x0f, 0x93, 0x11, 0xfa, 0x3c, 0xf8, 0x36};
constexpr uint64_t kEngine = 0x14617a9e8ULL;               // SingletonHolder<DjEngineIF>'s instance
constexpr uint64_t kEngineGraph = 0xb8;                    // DjEngineIF -> rbxfrm::DjUnitAudioGraph*
constexpr uint64_t kMainComponent = 0x145fe0ea0ULL;        // MainComponent::getInstance's static
constexpr uint64_t kUiManager = 0x4c0;                     // MainComponent -> djplay::UiManager*
constexpr uint64_t kDatabase = 0x14612dfd0ULL;             // SingletonHolder<DatabaseIF>'s instance
constexpr size_t kTrackSize = 0x4e0;                       // sizeof(db::RowDataTrack)
constexpr uint64_t kTrackItem = 0x38;                      // its item interface: getBasicInfo is slot 0x8
constexpr uint64_t kLoadMessageVtable = 0x143879948ULL;    // djplay::DjplayCommonMessage
constexpr int kCollection = 1;
constexpr uint64_t kBrowseView = 0x145f38738ULL;           // browse::BrowseBasicView's instance (initBrowseComponent)

struct Code { uint64_t address, size, hash; };
enum : int { NUM_PLAYERS, IS_LOADED, IS_LOADING, IS_PLAYING, POSITION_MS, TOTAL_MS, FILE_PATH, TEMPO, BPM,
             RUNNING_LOOP, IS_LOOPING, MASTER_TEMPO, SET_MASTER_TEMPO, GET_MASTER, IS_SYNC, KEY_SHIFT, IS_SLIP,
             KEY_SYNCING, PLAYER_CONTROL, ENGINE_GET, STRING_DTOR, OPERATOR_NEW, MAIN_COMPONENT, UI_PLAYER,
             TEMPO_SELECT, MESSAGE_CTOR, POST_MESSAGE, LOAD_TEMPLATE, DATABASE_GET, GET_ROW_DATA_TRACK,
             TRACK_CTOR, TRACK_DTOR, TRACK_BASIC_INFO, BROWSE_INSTANCE, BROWSE_MANAGERS, LIST_CONTAINER,
             CONTAINER_LIST, TREE_VIEWER, ROW_TRACK, SELECTED_ROWS, LIST_BOX, HAS_FOCUS, CODE_COUNT };
// FNV-1a of each function's first bytes; entries never called pin what the bridge reads directly:
// PLAYER_CONTROL the graph's players (+0x430/+0x43c), ENGINE_GET and DATABASE_GET the singletons,
// MAIN_COMPONENT its static, LOAD_TEMPLATE (whole) the load message and UiManager offset.
constexpr Code kCode[CODE_COUNT] = {
    {0x142cd7230, 0x20, 0xdaf94750df27c748ULL},  // DjUnitAudioGraph::getNumOfPlayer
    {0x14253b610, 0x20, 0x11a1b4fbc39dda72ULL},  // DjEngineIF::isLoaded
    {0x14253b660, 0x20, 0x85cf734eb06a5942ULL},  // DjEngineIF::isLoading
    {0x14253b950, 0x20, 0x5f817f22eb4e395bULL},  // DjEngineIF::isPlaying
    {0x14253bbc0, 0x20, 0x297a72177840c30eULL},  // DjEngineIF::getPlayingTimeFloatMs
    {0x14253b6b0, 0x20, 0xddcf3b777f811f52ULL},  // DjEngineIF::getTotalTime
    {0x14253b6f0, 0x20, 0x2e2d9baffbbc7373ULL},  // DjEngineIF::getFilePath
    {0x14253bd50, 0x20, 0x4eac48f5afbee8a8ULL},  // DjEngineIF::getTempo
    {0x14253dc40, 0x20, 0xb57f0788d1112998ULL},  // DjEngineIF::getBpm(channel, bool)
    {0x14253cfa0, 0x20, 0xa6a64b6b969ba83dULL},  // DjEngineIF::getRunningLoop
    {0x14253d3e0, 0x20, 0xdce6a516d252f411ULL},  // DjEngineIF::isLooping
    {0x14253bef0, 0x20, 0xabefce556a0c502fULL},  // DjEngineIF::isMasterTempoMode
    {0x14253beb0, 0x20, 0xd59bd31d72a3fec9ULL},  // DjEngineIF::setMasterTempoMode
    {0x14253e0d0, 0x20, 0xf0e61f8851c6b178ULL},  // DjEngineIF::getMaster
    {0x14253e2c0, 0x20, 0xbd62b85bc52837b6ULL},  // DjEngineIF::isSync
    {0x14253e660, 0x20, 0x995a4d1898c03512ULL},  // DjEngineIF::getShiftedPlayKeySemitone
    {0x142540120, 0x20, 0xe99945065534c531ULL},  // DjEngineIF::isSlipMode
    {0x14253e830, 0x20, 0xc0372604de3318ecULL},  // DjEngineIF::isKeySyncing
    {0x142cd74d0, 0x1d, 0x00d733ece65207f6ULL},  // DjUnitAudioGraph::getPlayerControl
    {0x141424a00, 0xca, 0x7edd0dda4b6a5c25ULL},  // SingletonHolder<DjEngineIF>::get
    {0x142a6e690, 0x28, 0xbe53737ce1d4b641ULL},  // juce::String::~String
    {0x14280dfc8, 0x20, 0x6212dc52d2cf3389ULL},  // operator new
    {0x14182a9c0, 0x8, 0x5e33906c339fa626ULL},   // MainComponent::getInstance
    {0x1425e82b0, 0x11, 0xbf66c9d3c80fa7e7ULL},  // djplay::UiManager::getPlayer
    {0x142560360, 0x7, 0x1a618aaaa69f7defULL},   // djplay::UiPlayer::getTempoSelectStatus
    {0x141e59e40, 0x20, 0x97e9be5df6af7c9aULL},  // common::Message::Message(int, int, int, void*)
    {0x141e5a9a0, 0x5, 0xd9bb96dd2d251dcfULL},   // common::CommonMessageReceiver::postCommonMessage
    {0x14140b910, 0x102f, 0x05e5fbe580a5b045ULL},// a browser load: the message layout and UiManager offset
    {0x141380ab0, 0x20, 0xc2d3aaabb6e7610fULL},  // SingletonHolder<DatabaseIF>::get
    {0x141f33610, 0xf, 0x5bd03a968f8269f1ULL},   // db::DatabaseIF::getRowDataTrack
    {0x1413a8200, 0x20, 0x139d7e05aa2b8015ULL},  // db::RowDataTrack::RowDataTrack()
    {0x1413a8d70, 0x40, 0x9b5d12ef0a6dfc9cULL},  // db::RowDataTrack::~RowDataTrack()
    {0x1413ab3e0, 0x20, 0xc3d179c4b9d43ac4ULL},  // db::RowDataTrack::getBasicInfo (item interface)
    // Rekordbox's browser: these pin every offset read_browser reads, as Rekordbox's own code uses them.
    {0x141418072, 0xa, 0xe499856b0feefaafULL},   // initBrowseComponent: stores the view's instance
    {0x14141833c, 0x63, 0x2d3be0df7fd04bf4ULL},  // ... its tree (+0x240) and list (+0x238) managers
    {0x14149314c, 0x18, 0x86b5b228d5dd4401ULL},  // BrowseListManager(): its LowerContainer (+0x108)
    {0x1414e754f, 0x18, 0xedb84c271364738eULL},  // LowerContainer(): its list view (+0x110)
    {0x141536709, 0x18, 0x911251fd3b4cb6d4ULL},  // BrowseTreeManager(): its tree viewer (+0x100)
    {0x1414addc0, 0x4b, 0x2bd636dbd896197cULL},  // ListViewer::getRowByIndex: rows, sort map, tracks
    {0x14161cc70, 0x21, 0xbdb8e61ee08df6d4ULL},  // CustomListBox::getSelectedRows: selection (+0x138)
    {0x1414ae26d, 0xc, 0x1da696860474cf7dULL},   // a ListViewer method: its list box (+0x220)
    {0x142c0c3d0, 0x29, 0x550b123365a23717ULL},  // juce::Component::hasKeyboardFocus
};

struct JString { const char *text; };  // juce::String: one pointer to its UTF-8 text
struct CueInfo { int32_t in, out; };   // djengine::StCueInfo: `long` is 32-bit on Windows

using NumPlayers = int (*)(const void *graph);
using DeckBool = bool (*)(const void *engine, int channel);
using DeckFlag = bool (*)(const void *engine, int channel, bool);
using DeckFloat = float (*)(const void *engine, int channel);
using DeckFloatFlag = float (*)(const void *engine, int channel, bool);
using DeckInt = int (*)(const void *engine, int channel);
using DeckPath = JString *(*)(const void *engine, JString *out, int channel);
using DeckLoop = CueInfo *(*)(const void *engine, CueInfo *out, int channel);
using SetDeckFlag = void (*)(const void *engine, int channel, bool);
using EngineInt = int (*)(const void *engine);
using StringDtor = void (*)(JString *);
using OperatorNew = void *(*)(size_t);
using UiPlayer = void *(*)(const void *ui_manager, int player);
using PlayerInt = int (*)(const void *ui_player);
using MessageCtor = void *(*)(void *message, int, int, int, void *);
using PostMessage = void (*)(void *receiver, void *message);
using GetRowDataTrack = bool (*)(void *database, uint32_t id, void *track, int browse_type, int device);
using TrackFn = void *(*)(void *track);
using HasFocus = bool (*)(const void *component, bool children);
using BasicInfo = void *(*)(void *item);

struct Api {
    uintptr_t slide = 0;
    NumPlayers num_players;
    DeckBool is_loaded, is_loading, is_playing, master_tempo, is_sync, is_slip, key_syncing;
    DeckFlag is_looping;
    DeckFloat position_ms, tempo;
    DeckFloatFlag bpm;
    DeckInt total_ms, key_shift;
    DeckPath file_path;
    DeckLoop running_loop;
    SetDeckFlag set_master_tempo;
    EngineInt master;
    StringDtor string_dtor;
    OperatorNew alloc;
    UiPlayer ui_player;
    PlayerInt tempo_select;
    MessageCtor message_ctor;
    PostMessage post_message;
    GetRowDataTrack get_row_data_track;
    TrackFn track_ctor, track_dtor;
    HasFocus has_focus;
} api;

BridgeState *shared = nullptr;
uint64_t ticks = 0;
LARGE_INTEGER frequency;
bool failed = false;

template <typename F> F at(int index) { return reinterpret_cast<F>(kCode[index].address - kBase + api.slide); }
template <typename T> T field(const void *object, size_t offset) {
    return *reinterpret_cast<const T *>(static_cast<const uint8_t *>(object) + offset);
}
const void *global(uint64_t address) {
    return *reinterpret_cast<const void *const volatile *>(address - kBase + api.slide);
}

uint64_t fnv(const uint8_t *p, size_t n) {
    uint64_t h = 0xcbf29ce484222325ULL;
    for (size_t i = 0; i < n; ++i) h = (h ^ p[i]) * 0x100000001b3ULL;
    return h;
}

uint64_t now_ns() {
    LARGE_INTEGER counter;
    QueryPerformanceCounter(&counter);
    return static_cast<uint64_t>(counter.QuadPart / frequency.QuadPart) * 1000000000ULL +
           static_cast<uint64_t>(counter.QuadPart % frequency.QuadPart) * 1000000000ULL / frequency.QuadPart;
}

void seq_begin() { InterlockedOr(reinterpret_cast<volatile LONG *>(&shared->seq), 1); MemoryBarrier(); }
void seq_end() { MemoryBarrier(); InterlockedIncrement(reinterpret_cast<volatile LONG *>(&shared->seq)); }

void publish_status(uint32_t status, const char *why) {
    if (!shared) return;
    seq_begin();  // may follow a tick that faulted mid-write
    shared->status = status;
    shared->deck_count = 0;
    snprintf(shared->note, sizeof shared->note, "%s", why);
    seq_end();
}

bool open_shared() {
    wchar_t dir[MAX_PATH], path[MAX_PATH];
    DWORD n = GetEnvironmentVariableW(L"LOCALAPPDATA", dir, MAX_PATH);
    if (!n || n >= MAX_PATH - 40) return false;
    wcscat_s(dir, L"\\rkbx_wave");
    CreateDirectoryW(dir, nullptr);
    wcscat_s(dir, L"\\bridge");
    CreateDirectoryW(dir, nullptr);
    swprintf_s(path, L"%s\\%hs", dir, kBridgeStateFile);
    HANDLE file = CreateFileW(path, GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr,
                              OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) return false;
    HANDLE mapping = CreateFileMappingW(file, nullptr, PAGE_READWRITE, 0, sizeof(BridgeState), nullptr);
    CloseHandle(file);
    if (!mapping) return false;
    void *view = MapViewOfFile(mapping, FILE_MAP_ALL_ACCESS, 0, 0, sizeof(BridgeState));
    CloseHandle(mapping);
    if (!view) return false;
    shared = static_cast<BridgeState *>(view);
    seq_begin();
    memcpy(shared->magic, kBridgeMagic, sizeof shared->magic);
    shared->version = kBridgeVersion;
    shared->pid = GetCurrentProcessId();
    shared->deck_count = 0;
    shared->status = kStatusStarting;
    shared->master = -1;
    memset(shared->note, 0, sizeof shared->note);
    memset(shared->decks, 0, sizeof shared->decks);
    memset(shared->devices, 0, sizeof shared->devices);
    memset(&shared->browser, 0, sizeof shared->browser);
    shared->browser.rows = -1;
    shared->browser.cursor = -1;
    shared->browser.focus = 2;
    memset(&shared->reply, 0, sizeof shared->reply);
    shared->reply.seq = shared->command.seq;  // a request from an earlier session never runs in this one
    seq_end();
    return true;
}

// Fails closed: the running executable must be exactly the build the addresses came from.
const char *verify_build() {
    const uint8_t *image = reinterpret_cast<const uint8_t *>(GetModuleHandleW(nullptr));
    const auto *dos = reinterpret_cast<const IMAGE_DOS_HEADER *>(image);
    if (!image || dos->e_magic != IMAGE_DOS_SIGNATURE) return "Unexpected main executable";
    const auto *nt = reinterpret_cast<const IMAGE_NT_HEADERS64 *>(image + dos->e_lfanew);
    if (nt->Signature != IMAGE_NT_SIGNATURE || nt->FileHeader.Machine != IMAGE_FILE_MACHINE_AMD64 ||
        nt->FileHeader.TimeDateStamp != kTimestamp)
        return "Unsupported Rekordbox build: Wave's bridge supports Rekordbox 7.2.19.0342";
    const IMAGE_DATA_DIRECTORY &debug = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_DEBUG];
    bool guid_ok = false;
    for (DWORD i = 0; debug.VirtualAddress && i < debug.Size / sizeof(IMAGE_DEBUG_DIRECTORY); ++i) {
        const auto *entry = reinterpret_cast<const IMAGE_DEBUG_DIRECTORY *>(image + debug.VirtualAddress) + i;
        if (entry->Type != IMAGE_DEBUG_TYPE_CODEVIEW || !entry->AddressOfRawData) continue;
        const uint8_t *cv = image + entry->AddressOfRawData;
        guid_ok = !memcmp(cv, "RSDS", 4) && !memcmp(cv + 4, kPdbGuid, sizeof kPdbGuid);
    }
    if (!guid_ok) return "Unsupported Rekordbox build: Wave's bridge supports Rekordbox 7.2.19.0342";
    api.slide = reinterpret_cast<uintptr_t>(image);
    for (const Code &code : kCode)
        if (fnv(image + (code.address - kBase), code.size) != code.hash)
            return "Rekordbox's code differs from the verified build; bridge disabled";
    api.num_players = at<NumPlayers>(NUM_PLAYERS);
    api.is_loaded = at<DeckBool>(IS_LOADED);
    api.is_loading = at<DeckBool>(IS_LOADING);
    api.is_playing = at<DeckBool>(IS_PLAYING);
    api.position_ms = at<DeckFloat>(POSITION_MS);
    api.total_ms = at<DeckInt>(TOTAL_MS);
    api.file_path = at<DeckPath>(FILE_PATH);
    api.tempo = at<DeckFloat>(TEMPO);
    api.bpm = at<DeckFloatFlag>(BPM);
    api.running_loop = at<DeckLoop>(RUNNING_LOOP);
    api.is_looping = at<DeckFlag>(IS_LOOPING);
    api.master_tempo = at<DeckBool>(MASTER_TEMPO);
    api.set_master_tempo = at<SetDeckFlag>(SET_MASTER_TEMPO);
    api.master = at<EngineInt>(GET_MASTER);
    api.is_sync = at<DeckBool>(IS_SYNC);
    api.key_shift = at<DeckInt>(KEY_SHIFT);
    api.is_slip = at<DeckBool>(IS_SLIP);
    api.key_syncing = at<DeckBool>(KEY_SYNCING);
    api.string_dtor = at<StringDtor>(STRING_DTOR);
    api.alloc = at<OperatorNew>(OPERATOR_NEW);
    api.ui_player = at<UiPlayer>(UI_PLAYER);
    api.tempo_select = at<PlayerInt>(TEMPO_SELECT);
    api.message_ctor = at<MessageCtor>(MESSAGE_CTOR);
    api.post_message = at<PostMessage>(POST_MESSAGE);
    api.get_row_data_track = at<GetRowDataTrack>(GET_ROW_DATA_TRACK);
    api.track_ctor = at<TrackFn>(TRACK_CTOR);
    api.track_dtor = at<TrackFn>(TRACK_DTOR);
    api.has_focus = at<HasFocus>(HAS_FOCUS);
    return nullptr;
}

void *ui_player(int deck) {
    const void *main = global(kMainComponent);
    const void *manager = main ? field<const void *>(main, kUiManager) : nullptr;
    return manager ? api.ui_player(manager, deck) : nullptr;
}

// Rekordbox's browser as its controller moves through it: the rows around its cursor, the cursor,
// and which pane has focus. Only reads, on the UI thread that owns all of it. Same layout as macOS
// but for the view's managers (+0x238/+0x240; macOS +0x268/+0x270) and the list's fields (0x18 lower).
// shortcut: no tree path on Windows (TreeViewer::getCurrentItemPath is unmapped); Wave's heading
// then reads REKORDBOX only. Map it when the breadcrumb is wanted on Windows.
void read_browser() {
    BridgeBrowser &b = shared->browser;
    const void *view = global(kBrowseView);
    const void *lists = view ? field<const void *>(view, 0x238) : nullptr;
    const void *trees = view ? field<const void *>(view, 0x240) : nullptr;
    const void *container = lists ? field<const void *>(lists, 0x108) : nullptr;
    const void *list = container ? field<const void *>(container, 0x110) : nullptr;
    const void *tree = trees ? field<const void *>(trees, 0x100) : nullptr;
    b.focus = tree && api.has_focus(tree, true) ? 1 : list && api.has_focus(list, true) ? 0 : 2;
    if (!list) { b.rows = -1; b.cursor = -1; b.first = 0; b.count = 0; return; }
    const int rows = field<int>(list, 0x240);
    const void *box = field<const void *>(list, 0x220);
    const int *ranges = box ? field<const int *>(box, 0x138) : nullptr;
    const int cursor = box && ranges && field<int>(box, 0x144) > 0 ? ranges[0] : -1;
    const int *order = field<const int *>(list, 0x228);
    const int ordered = field<int>(list, 0x234);
    const void *tracks = field<const void *>(list, 0x250);
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

uint64_t command_check(const BridgeCommand &c) {
    uint8_t bytes[32];
    memcpy(bytes, &c.seq, 4); memcpy(bytes + 4, &c.kind, 4); memcpy(bytes + 8, &c.deck, 4);
    memcpy(bytes + 12, &c.content_id, 4); memcpy(bytes + 16, &c.issued_ns, 8); memcpy(bytes + 24, &c.device, 8);
    return fnv(bytes, sizeof bytes);
}

// The load message Rekordbox's own browser posts for a stopped deck, as on macOS (same layout).
void post_load(void *player, void *basic_info) {
    auto *m = static_cast<uint8_t *>(api.alloc(0x70));  // Rekordbox's heap: Rekordbox frees it
    api.message_ctor(m, 0x1234, 0, 0, nullptr);
    *reinterpret_cast<uint32_t *>(m + 0x38) = 7;
    *reinterpret_cast<int64_t *>(m + 0x40) = 0;     // no autoplay
    *reinterpret_cast<int64_t *>(m + 0x48) = 0x59;
    *reinterpret_cast<int64_t *>(m + 0x50) = 0;     // an ordinary load
    *reinterpret_cast<double *>(m + 0x58) = -1.0;   // from Rekordbox's own cue
    *reinterpret_cast<void **>(m + 0x60) = basic_info;
    *reinterpret_cast<void **>(m + 0x68) = nullptr;
    *reinterpret_cast<uint64_t *>(m) = kLoadMessageVtable - kBase + api.slide;
    api.post_message(player, m);
}

int load(const void *dj, int players, const BridgeCommand &c, char *message, size_t size) {
    const int deck = static_cast<int>(c.deck);
    if (c.kind == kCommandLoadDevice) { snprintf(message, size, "Loading from a USB stick is not available on Windows yet"); return kReplyUnsupported; }
    if (deck < 0 || deck >= players) { snprintf(message, size, "Deck %d does not exist", deck + 1); return kReplyBadDeck; }
    if (api.is_playing(dj, deck)) { snprintf(message, size, "Deck %d is playing", deck + 1); return kReplyDeckPlaying; }
    if (api.is_loading(dj, deck)) { snprintf(message, size, "Deck %d is still loading", deck + 1); return kReplyDeckBusy; }
    void *database = const_cast<void *>(global(kDatabase));
    void *player = ui_player(deck);
    if (!database || !player) { snprintf(message, size, "Rekordbox is still starting"); return kReplyNotReady; }
    alignas(16) uint8_t track[0x600];  // RowDataTrack is 0x4e0; the margin costs nothing
    static_assert(sizeof track >= kTrackSize, "track buffer");
    api.track_ctor(track);
    int result;
    void *item = track + kTrackItem;
    const auto basic_info = reinterpret_cast<BasicInfo>(field<uint64_t>(field<const void *>(item, 0), 0x8));
    if (reinterpret_cast<uint64_t>(basic_info) != kCode[TRACK_BASIC_INFO].address - kBase + api.slide) {
        snprintf(message, size, "Rekordbox's track type differs from the verified build");
        result = kReplyUnsupported;
    } else if (!api.get_row_data_track(database, c.content_id, track, kCollection, 0)) {
        snprintf(message, size, "Track %u is not in Rekordbox's collection", c.content_id);
        result = kReplyNoTrack;
    } else if (void *info = basic_info(item)) {
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
    if (c.seq == shared->reply.seq || command_check(c) != c.check) return;
    char message[sizeof shared->reply.message] = {0};
    int result;
    const uint64_t now = now_ns();
    if (c.issued_ns > now || now - c.issued_ns > 2000000000ULL) {
        snprintf(message, sizeof message, "Request expired before Rekordbox could act on it");
        result = kReplyExpired;
    } else if (!dj || players <= 0) {
        snprintf(message, sizeof message, "Rekordbox is still starting");
        result = kReplyNotReady;
    } else if (c.kind == kCommandLoad || c.kind == kCommandLoadDevice) {
        result = load(dj, players, c, message, sizeof message);
    } else if (c.kind == kCommandMasterTempo && int(c.deck) < players && api.is_loaded(dj, int(c.deck))) {
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
    MemoryBarrier();
    shared->reply.seq = c.seq;
}

void tick() {
    const void *dj = global(kEngine);
    const void *graph = dj ? field<const void *>(dj, kEngineGraph) : nullptr;
    const int players = graph ? api.num_players(graph) : 0;
    const bool read_paths = ticks % 25 == 0;
    seq_begin();
    ++ticks;
    shared->ticks = ticks;
    shared->uptime_ns = now_ns();
    shared->status = players > 0 ? kStatusLive : kStatusStarting;
    shared->master = players > 0 ? api.master(dj) : -1;
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
                CueInfo loop = {-1, -1};
                api.running_loop(dj, &loop, c);
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
            JString path = {nullptr};
            api.file_path(dj, &path, c);
            const char *text = path.text ? path.text : "";
            if (strncmp(text, deck.path, sizeof deck.path)) {
                snprintf(deck.path, sizeof deck.path, "%s", text);
                ++deck.path_seq;
            }
            api.string_dtor(&path);  // Rekordbox's own release, on Rekordbox's own heap
        } else if (!loaded && deck.path[0]) {
            deck.path[0] = 0;
            ++deck.path_seq;
        }
        deck.flags = flags;
    }
    if (ticks % 5 == 0) read_browser();
    seq_end();
    run_command(dj, players);
}

// A fault anywhere in a tick stops the bridge and leaves Rekordbox running.
void guarded_tick() {
    __try {
        tick();
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        failed = true;
        publish_status(kStatusFailed, "Rekordbox engine read failed; bridge stopped");
    }
}

void CALLBACK on_timer(HWND, UINT, UINT_PTR, DWORD) {
    if (!failed) guarded_tick();
}

HHOOK hook = nullptr;

// Runs on Rekordbox's UI thread the first time it takes a message: start the tick timer there,
// where Rekordbox's own UI reads the same engine, then let go of the hook.
LRESULT CALLBACK on_message(int code, WPARAM wparam, LPARAM lparam) {
    HHOOK self = hook;
    if (self && code >= 0) {
        hook = nullptr;
        SetTimer(nullptr, 0, 10, on_timer);
        UnhookWindowsHookEx(self);
    }
    return CallNextHookEx(nullptr, code, wparam, lparam);
}

DWORD ui_thread() {
    // Rekordbox's UI thread is its first: the one that started the process.
    DWORD best = 0;
    ULONGLONG earliest = ~0ULL;
    HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
    if (snapshot == INVALID_HANDLE_VALUE) return 0;
    THREADENTRY32 entry = {sizeof entry};
    for (BOOL more = Thread32First(snapshot, &entry); more; more = Thread32Next(snapshot, &entry)) {
        if (entry.th32OwnerProcessID != GetCurrentProcessId()) continue;
        HANDLE thread = OpenThread(THREAD_QUERY_LIMITED_INFORMATION, FALSE, entry.th32ThreadID);
        if (!thread) continue;
        FILETIME created, exited, kernel, user;
        if (GetThreadTimes(thread, &created, &exited, &kernel, &user)) {
            const ULONGLONG t = (ULONGLONG(created.dwHighDateTime) << 32) | created.dwLowDateTime;
            if (t < earliest) { earliest = t; best = entry.th32ThreadID; }
        }
        CloseHandle(thread);
    }
    CloseHandle(snapshot);
    return best;
}

bool start() {
    HMODULE self;  // never unloaded while the timer can still run its code
    GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_PIN,
                       reinterpret_cast<LPCWSTR>(&start), &self);
    const DWORD thread = ui_thread();
    if (!thread) return false;
    if (thread == GetCurrentThreadId()) {
        SetTimer(nullptr, 0, 10, on_timer);
        return true;
    }
    hook = SetWindowsHookExW(WH_GETMESSAGE, on_message, nullptr, thread);
    return hook != nullptr;
}

bool is_rekordbox() {
    wchar_t exe[MAX_PATH];
    DWORD n = GetModuleFileNameW(nullptr, exe, MAX_PATH);
    const wchar_t suffix[] = L"\\rekordbox.exe";
    const size_t m = wcslen(suffix);
    return n >= m && n < MAX_PATH && !_wcsicmp(exe + n - m, suffix);
}

// The minimum of OpenSSL's provider interface: a name and a version, no algorithms.
struct OsslDispatch { int function_id; void (*function)(void); };
struct OsslParam { const char *key; unsigned int data_type; void *data; size_t data_size; size_t return_size; };
constexpr int kGettableParams = 1025, kGetParams = 1026;  // OSSL_FUNC_PROVIDER_GETTABLE/GET_PARAMS
constexpr unsigned int kUtf8Ptr = 6;                       // OSSL_PARAM_UTF8_PTR
char provider_name[] = "Rekordbox Wave bridge";
char provider_version[] = WAVE_BRIDGE_VERSION;
const OsslParam provider_params[] = {{"name", kUtf8Ptr, nullptr, 0, 0}, {"version", kUtf8Ptr, nullptr, 0, 0}, {}};
const OsslParam *gettable(void *) { return provider_params; }
int get_params(void *, OsslParam params[]) {
    for (OsslParam *p = params; p && p->key; ++p) {
        if (p->data_type != kUtf8Ptr || !p->data) continue;
        if (!strcmp(p->key, "name")) *static_cast<char **>(p->data) = provider_name;
        if (!strcmp(p->key, "version")) *static_cast<char **>(p->data) = provider_version;
    }
    return 1;
}
const OsslDispatch provider_dispatch[] = {
    {kGettableParams, reinterpret_cast<void (*)(void)>(gettable)},
    {kGetParams, reinterpret_cast<void (*)(void)>(get_params)},
    {0, nullptr}};

}  // namespace

extern "C" __declspec(dllexport)
int OSSL_provider_init(const void *, const void *, const OsslDispatch **out, void **ctx) {
    *out = provider_dispatch;
    *ctx = nullptr;
    static LONG once = 0;
    char enabled[4] = {0};
    // Child processes inherit the environment; only Rekordbox's own process reads state.
    if (GetEnvironmentVariableA("RKBX_WAVE_BRIDGE", enabled, sizeof enabled) != 1 || enabled[0] != '1' ||
        !is_rekordbox() || InterlockedExchange(&once, 1))
        return 1;
    QueryPerformanceFrequency(&frequency);
    if (!open_shared()) return 1;
    if (const char *why = verify_build()) {
        publish_status(kStatusUnsupported, why);
        return 1;
    }
    if (!start()) publish_status(kStatusFailed, "Could not reach Rekordbox's UI thread");
    return 1;
}
