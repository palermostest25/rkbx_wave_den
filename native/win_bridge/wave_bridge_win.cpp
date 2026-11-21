// Rekordbox Wave bridge for Windows: an OpenSSL provider that Rekordbox loads when started with
// OPENSSL_CONF pointing at Wave's config. It provides no algorithms. On one exact, verified
// Rekordbox build it reads deck state through Rekordbox's own engine functions on Rekordbox's
// UI thread and shares it through a private file, as the macOS bridge does. Anything unexpected -
// another build, changed code - and it does nothing but report why. A fault inside the bridge
// stops the bridge, never Rekordbox.
//
// Two ways in: Rekordbox loads it as an OpenSSL provider when Wave starts Rekordbox, or Wave loads it
// into a Rekordbox that is already running and calls wave_bridge_attach. Either way it runs once.
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
#include "detour.h"

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
constexpr int kCollection = 1;                       // db::_browse_type for the collection
constexpr int kDevice = 2;                           // ... and for a device, as on macOS (the same enum)
constexpr uint64_t kBrowseView = 0x145f38738ULL;           // browse::BrowseBasicView's instance (initBrowseComponent)
// A stick has two databases: its Device Library (export.pdb) and its OneLibrary (exportLibrary.db),
// each under its own handle. Wave's ids are OneLibrary's, so only that one is offered to Wave.
constexpr uint64_t kOneLibraryVtable = 0x143d43910ULL;     // the OneLibrary database's class
constexpr uint64_t kStickLoadSlot = 0x590;                 // its slot for loading the track table
constexpr uint64_t kStickTracks = 0x88;                    // its track table: null until loaded

struct Code { uint64_t address, size, hash; };
enum : int { NUM_PLAYERS, IS_LOADED, IS_LOADING, IS_PLAYING, POSITION_MS, TOTAL_MS, FILE_PATH, TEMPO, BPM,
             RUNNING_LOOP, IS_LOOPING, MASTER_TEMPO, SET_MASTER_TEMPO, GET_MASTER, IS_SYNC, KEY_SHIFT, IS_SLIP,
             KEY_SYNCING, PLAYER_CONTROL, ENGINE_GET, STRING_DTOR, OPERATOR_NEW, MAIN_COMPONENT, UI_PLAYER,
             TEMPO_SELECT, MESSAGE_CTOR, POST_MESSAGE, LOAD_TEMPLATE, DATABASE_GET, GET_ROW_DATA_TRACK,
             TRACK_CTOR, TRACK_DTOR, TRACK_BASIC_INFO, BROWSE_INSTANCE, BROWSE_MANAGERS, LIST_CONTAINER,
             CONTAINER_LIST, TREE_VIEWER, ROW_TRACK, SELECTED_ROWS, LIST_BOX, HAS_FOCUS, DEVICE_ROOT,
             ALL_DEVICES, CRT_FREE, STICK_DATABASE, STICK_LOAD, STICK_ROW, MIDI_CALLBACK, MIDI_OPEN, PERFORM, SPLIT_SCREEN,
             SET_TEXT, SEARCH_RETURN, SEARCH_RUN, STRING_CTOR, PREFERENCES, SEEK, CODE_COUNT };
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
    // Mounted devices (USB sticks): the DatabaseIF wrappers beside the macOS ones, in the same order.
    {0x141f316a0, 0x30, 0x0010aaa567934b28ULL},  // db::DatabaseIF::getDeviceRoot(long), String by hidden pointer
    {0x141f316e0, 0xd, 0x07e17db09ecbcf74ULL},   // db::DatabaseIF::getAllDeviceHandle(Array<long>&): 32-bit handles
    {0x14299f450, 0x5, 0x3b02d066bf27fab5ULL},   // free, as that Array releases its storage with it
    // A stick's tracks: Rekordbox on Windows reads a stick's track table only when its own browser
    // first opens that stick, and until then finds no track on it. The bridge reads it the same way.
    {0x141f557f0, 0x40, 0x7c8d688eac9c82a1ULL},  // the stick database for a handle (null if unusable)
    {0x14218bdf0, 0x48, 0x8a7f9ea550e6b48fULL},  // a stick database's track table loader
    {0x142190ae0, 0x50, 0x42d649ab558148d3ULL},  // its row lookup: no table (+0x88), no track
    // The controller: Rekordbox reads every MIDI input with WinMM, through one callback (JUCE's MidiInCollector),
    // and the one midiInOpen that passes it (CALLBACK_FUNCTION). The bridge hears the controller there.
    {0x1410692b0, 0x29, 0x97482164307fc9cbULL},  // midiInCallback(HMIDIIN, UINT, DWORD_PTR instance, msg, time)
    {0x141069ab0, 0x24, 0x401859268165a48bULL},  // midiInOpen(&handle, id, midiInCallback, collector, 0x30000)
    // Rekordbox's own commands: MainComponent is its JUCE ApplicationCommandTarget (at +0x238), and perform runs a
    // command as its menus and buttons do. Show Split Screen (0xB04C, the browser's own bottom-right button too)
    // toggles the split browser window through +0x328's slot 0x190, reading nothing else of the request.
    {0x141824840, 0x30, 0xea4f7fb0902eb0edULL},  // MainComponent::perform(const InvocationInfo &)
    {0x141826fa9, 0x20, 0x882a3c70274bc1ceULL},  // ... its Show Split Screen case
    // The browser's search box (browse::SearchFilter): its juce::TextEditor at +0x110, and its TextEditor::Listener (at
    // +0xf0) whose Return handler runs the search (Beatport's too, on Search Tracks) as pressing Return does.
    {0x142b91710, 0x30, 0x0ccbe7cbebbbd10cULL},  // juce::TextEditor::setText(const String &, bool sendTextChangeMessage)
    {0x14163e420, 0x15, 0x58dc6bf2e1f56077ULL},  // SearchFilter::textEditorReturnKeyPressed(TextEditor &)
    {0x14163e2e0, 0x30, 0xd2fac023c246035cULL},  // ... what it runs: the search, posted to run next
    {0x142a6e420, 0x20, 0xdc98d15944a5e9f5ULL},  // juce::String::String(const char *)
    // MainComponent::perform's case for its Preferences command (0x200A): MainComponent::showPreference(-1), the page
    // it was last on, as File > Preferences opens it.
    {0x141825070, 0x18, 0x4b091471eea1d316ULL},
    // DjEngineIF's jump to a time (channel, ms): what Rekordbox's own Needle Search runs (UiPlayer's needle search,
    // 0x142598a40, from the MIDI NeedleSearch control), its player's slot 0x138. 0 when it took.
    {0x14253ba10, 0x20, 0x8f8fe9146d05beb1ULL},
};
constexpr int kPreferencesCommand = 0x200A;
constexpr uint64_t kSearchFilterVtable = 0x143938ca0ULL;   // browse::SearchFilter (its Component)
constexpr uint64_t kSearchListenerVtable = 0x143938e38ULL; // its TextEditor::Listener, at +0xf0
constexpr uint64_t kSearchEditor = 0x110;                  // its juce::TextEditor*
constexpr uint64_t kRowTitle = 0x20, kRowArtist = 0xd0;    // db::RowDataTrack's title and artist (juce::String)
// From the browser view to its search box's TextEditor, then that editor's parent component (the SearchFilter).
constexpr uint64_t kSearchPath[4] = {0x228, 0x40, 0xa8, 0x40};
constexpr uint64_t kCommandTargetVtable = 0x1439e11c8ULL;  // MainComponent's ApplicationCommandTarget vtable
constexpr uint64_t kCommandTarget = 0x238;                 // that base, inside MainComponent
constexpr int kShowSplitScreen = 0xB04C;

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
using DeckSeek = int (*)(const void *engine, int channel, int ms);
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
struct IntArray { int32_t *data; int32_t allocated, used; };  // juce::Array<long>: `long` is 32-bit on Windows
using AllDevices = void (*)(void *database, IntArray *);
using DeviceRoot = JString *(*)(void *database, JString *out, int32_t handle);
using CrtFree = void (*)(void *);
using BasicInfo = void *(*)(void *item);
using StickDatabase = void *(*)(void *database_impl, int32_t handle, int32_t *index);
using StickLoad = int32_t (*)(void *stick, int32_t *status, void *progress);

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
    DeckSeek seek;
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
    AllDevices all_devices;
    DeviceRoot device_root;
    CrtFree crt_free;
    StickDatabase stick_database;
    StickLoad stick_load;
} api;

BridgeState *shared = nullptr;
uint64_t ticks = 0;
LARGE_INTEGER frequency;
bool failed = false;
// Rekordbox's browser is read apart from its decks: a fault there (a list, a row or the tree laid out otherwise than
// verified) turns off only Wave's view of the browser, never the decks, and says where it failed.
bool browser_failed = false;
const char *volatile browser_step = "";

// --- The controller, as Rekordbox hears it ------------------------------------------------------------------
// Rekordbox's MIDI callback is detoured to midi_hook: every short message it receives is shared with Wave, and the
// ones Wave keeps (its browser controls, while Wave runs and says so) never reach Rekordbox. A fault in here
// passes the message on untouched. The callback's first three instructions (15 bytes, none RIP-relative) move to
// a trampoline that continues into the rest of it.
constexpr UINT kMimData = 0x3C3;  // MIM_DATA
constexpr size_t kMidiPrologue = 15;  // three whole instructions, none RIP-relative
using MidiProc = void (CALLBACK *)(HANDLE, UINT, DWORD_PTR, DWORD_PTR, DWORD_PTR);  // HMIDIIN
MidiProc midi_next = nullptr;
volatile LONG midi_seq = 0;
volatile LONG midi_broken = 0;
uint64_t now_ns();

bool wave_keeps(uint32_t status, uint32_t data1) {
    BridgeMidi &m = shared->midi;
    const uint64_t wave = *reinterpret_cast<volatile uint64_t *>(&m.wave_ns);
    const uint64_t now = now_ns();
    if (!wave || wave > now + 1000000000ULL || now - wave > 1000000000ULL) return false;  // Wave isn't there
    uint32_t n = *reinterpret_cast<volatile uint32_t *>(&m.keep_count);
    if (n > kMidiKeeps) n = kMidiKeeps;
    if ((status & 0xF0) == 0x80) status = 0x90 | (status & 0x0F);  // a note-off is its note-on's release
    const uint16_t key = static_cast<uint16_t>(status << 8 | data1);
    for (uint32_t i = 0; i < n; ++i)
        if (reinterpret_cast<volatile uint16_t *>(m.keep)[i] == key) return true;
    return false;
}

bool midi_seen(DWORD_PTR message) {
    const uint32_t status = static_cast<uint32_t>(message & 0xFF), data1 = static_cast<uint32_t>((message >> 8) & 0x7F),
                   data2 = static_cast<uint32_t>((message >> 16) & 0x7F);
    if (status < 0x80 || status >= 0xF0) return false;  // only channel messages: clock and sysex pass by
    const bool keep = wave_keeps(status, data1);
    const LONG seq = InterlockedIncrement(&midi_seq);
    BridgeMidiEvent &e = shared->midi.events[(static_cast<uint32_t>(seq) - 1) % kMidiEvents];
    e.status = static_cast<uint8_t>(status);
    e.data1 = static_cast<uint8_t>(data1);
    e.data2 = static_cast<uint8_t>(data2);
    e.kept = keep ? 1 : 0;
    MemoryBarrier();
    InterlockedExchange(reinterpret_cast<volatile LONG *>(&e.seq), seq);
    InterlockedExchange(reinterpret_cast<volatile LONG *>(&shared->midi.written), seq);
    if (keep) InterlockedIncrement(reinterpret_cast<volatile LONG *>(&shared->midi.kept));
    return keep;
}

void CALLBACK midi_hook(HANDLE in, UINT message, DWORD_PTR instance, DWORD_PTR param1, DWORD_PTR param2) {
    bool keep = false;
    if (message == kMimData && shared && !midi_broken) {
        __try {
            keep = midi_seen(param1);
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            InterlockedExchange(&midi_broken, 1);
            keep = false;
        }
    }
    if (!keep) midi_next(in, message, instance, param1, param2);
}

// Detours the verified callback (see detour.h): Rekordbox's MIDI input goes through midi_hook from now on.
bool hook_midi() {
    // midi_next is set before the entry jumps to the hook, which calls it.
    auto *target = reinterpret_cast<uint8_t *>(kCode[MIDI_CALLBACK].address - kBase + api.slide);
    return wave_detour::install(target, kMidiPrologue, reinterpret_cast<void *>(&midi_hook), reinterpret_cast<void **>(&midi_next));
}

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
    memset(&shared->midi, 0, sizeof shared->midi);
    memset(shared->texts, 0, sizeof shared->texts);
    memset(&shared->search, 0, sizeof shared->search);
    shared->search.first = -1;
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
    api.seek = at<DeckSeek>(SEEK);
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
    api.all_devices = at<AllDevices>(ALL_DEVICES);
    api.device_root = at<DeviceRoot>(DEVICE_ROOT);
    api.crt_free = at<CrtFree>(CRT_FREE);
    api.stick_database = at<StickDatabase>(STICK_DATABASE);
    api.stick_load = at<StickLoad>(STICK_LOAD);
    // The OneLibrary class must load and look up rows with the functions checked above.
    const auto *one_library = reinterpret_cast<const uint64_t *>(kOneLibraryVtable - kBase + api.slide);
    if (one_library[kStickLoadSlot / 8] != kCode[STICK_LOAD].address - kBase + api.slide ||
        one_library[0x40 / 8] != 0x1421685d0ULL - kBase + api.slide)
        return "Rekordbox's code differs from the verified build; bridge disabled";
    return nullptr;
}

// A mounted stick's OneLibrary database, or null: its Device Library, or a stick Rekordbox can't use.
void *one_library(int32_t handle) {
    void *database = const_cast<void *>(global(kDatabase));
    void *impl = database ? field<void *>(database, 0) : nullptr;
    void *stick = impl ? api.stick_database(impl, handle, nullptr) : nullptr;
    return stick && field<uint64_t>(stick, 0) == kOneLibraryVtable - kBase + api.slide ? stick : nullptr;
}

// Mounted devices, refreshed every couple of seconds: Wave picks a stick's handle by its root.
void read_devices() {
    void *database = const_cast<void *>(global(kDatabase));
    BridgeDevice found[kBridgeDevices] = {};
    if (database) {
        IntArray handles = {};  // empty: Rekordbox allocates it, on its own heap
        api.all_devices(database, &handles);
        for (int i = 0, n = 0; i < handles.used && n < int(kBridgeDevices); ++i) {
            if (!one_library(handles.data[i])) continue;
            found[n].handle = handles.data[i];
            JString root = {nullptr};
            api.device_root(database, &root, handles.data[i]);
            snprintf(found[n].root, sizeof found[n].root, "%s", root.text ? root.text : "");
            api.string_dtor(&root);
            ++n;
        }
        if (handles.data) api.crt_free(handles.data);
    }
    memcpy(shared->devices, found, sizeof found);
}

bool known_device(int64_t handle) {
    for (const BridgeDevice &device : shared->devices)
        if (handle && device.handle == handle) return true;
    return false;
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
// Rekordbox's browser track list (BrowseListViewer, as its LowerContainer shows it: the collection, a playlist, and
// Beatport's lists and search results alike), and the track behind one of its rows.
const void *browser_list() {
    const void *view = global(kBrowseView);
    const void *lists = view ? field<const void *>(view, 0x238) : nullptr;
    const void *container = lists ? field<const void *>(lists, 0x108) : nullptr;
    return container ? field<const void *>(container, 0x110) : nullptr;
}

const void *row_track(const void *list, int row) {
    if (!list || row < 0 || row >= field<int>(list, 0x240)) return nullptr;
    const int *order = field<const int *>(list, 0x228);
    const int ordered = field<int>(list, 0x234);
    const void *tracks = field<const void *>(list, 0x250);
    const int stored = tracks ? field<int>(tracks, 0xc) : 0;
    const void *const *data = tracks ? field<const void *const *>(tracks, 0) : nullptr;
    const int index = row < ordered && order ? order[row] : row;
    return data && index >= 0 && index < stored ? data[index] : nullptr;
}

void read_browser() {
    BridgeBrowser &b = shared->browser;
    const void *view = global(kBrowseView);
    const void *lists = view ? field<const void *>(view, 0x238) : nullptr;
    const void *trees = view ? field<const void *>(view, 0x240) : nullptr;
    const void *container = lists ? field<const void *>(lists, 0x108) : nullptr;
    const void *list = container ? field<const void *>(container, 0x110) : nullptr;
    const void *tree = trees ? field<const void *>(trees, 0x100) : nullptr;
    browser_step = "focus";
    b.focus = tree && api.has_focus(tree, true) ? 1 : list && api.has_focus(list, true) ? 0 : 2;
    browser_step = "tree path";
    // Where the sidebar is (BrowseTreeViewer's path, a StringArray at +0x2e8: Root, Beatport, Search Tracks...).
    b.tree_path[0] = 0;
    const JString *path = tree ? field<const JString *>(tree, 0x2e8) : nullptr;
    const int parts = tree ? field<int>(tree, 0x2f4) : 0;
    for (int i = 1, used = 0; path && i < parts && i < 16; ++i) {
        const char *part = path[i].text ? path[i].text : "";
        const int n = snprintf(b.tree_path + used, sizeof b.tree_path - used, "%s%s", used ? " / " : "", part);
        if (n < 0 || used + n >= int(sizeof b.tree_path)) break;
        used += n;
    }
    if (!list) { b.rows = -1; b.cursor = -1; b.first = 0; b.count = 0; return; }
    browser_step = "list cursor";
    const int rows = field<int>(list, 0x240);
    const void *box = field<const void *>(list, 0x220);
    const int *ranges = box ? field<const int *>(box, 0x138) : nullptr;
    const int cursor = box && ranges && field<int>(box, 0x144) > 0 ? ranges[0] : -1;
    const int wanted = shared->search.first;  // Wave scrolling Rekordbox's list in its own browser
    int first = wanted >= 0 ? wanted : cursor < 0 ? 0 : cursor - int(kBrowseRows) / 4;
    first = first > rows - int(kBrowseRows) ? rows - int(kBrowseRows) : first;
    first = first < 0 ? 0 : first;
    int count = 0;
    for (int row = first; row < rows && count < int(kBrowseRows); ++row, ++count) {
        browser_step = "row track";
        const void *track = row_track(list, row);
        BrowseRow &out = b.rows_[count];
        out.id = track ? field<uint32_t>(track, 0x8) : 0;
        const char *path = track ? field<const char *>(track, 0x98) : nullptr;  // the full path; +0xa8 is its name
        snprintf(out.path, sizeof out.path, "%s", path ? path : "");
        browser_step = "row title";
        // As Rekordbox shows the row: a streamed one (Beatport, its search results) has no file to look up.
        BrowseText &text = shared->texts[count];
        const char *title = track ? field<const char *>(track, kRowTitle) : nullptr;
        const char *artist = track ? field<const char *>(track, kRowArtist) : nullptr;
        snprintf(text.title, sizeof text.title, "%s", title ? title : "");
        snprintf(text.artist, sizeof text.artist, "%s", artist ? artist : "");
    }
    b.rows = rows; b.cursor = cursor; b.first = first; b.count = count;
}

void browser_fault(const char *what) {
    browser_failed = true;
    BridgeBrowser &b = shared->browser;
    b.rows = -1; b.cursor = -1; b.first = 0; b.count = 0; b.focus = 2;
    memset(shared->texts, 0, sizeof shared->texts);
    // Wave reads a path starting with '!' as this note, not as a place in the tree.
    snprintf(b.tree_path, sizeof b.tree_path, "!Rekordbox's browser could not be read (%s at %s)", what, browser_step);
}

void guarded_read_browser() {
    if (browser_failed) return;
    __try {
        read_browser();
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        browser_fault("reading");
    }
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
    const bool device = c.kind == kCommandLoadDevice;
    if (device && !known_device(c.device)) { snprintf(message, size, "That USB stick is no longer mounted"); return kReplyNoTrack; }
    if (deck < 0 || deck >= players) { snprintf(message, size, "Deck %d does not exist", deck + 1); return kReplyBadDeck; }
    if (api.is_playing(dj, deck)) { snprintf(message, size, "Deck %d is playing", deck + 1); return kReplyDeckPlaying; }
    if (api.is_loading(dj, deck)) { snprintf(message, size, "Deck %d is still loading", deck + 1); return kReplyDeckBusy; }
    void *database = const_cast<void *>(global(kDatabase));
    void *player = ui_player(deck);
    if (!database || !player) { snprintf(message, size, "Rekordbox is still starting"); return kReplyNotReady; }
    if (device) {
        void *stick = one_library(static_cast<int32_t>(c.device));
        if (!stick) { snprintf(message, size, "That USB stick is no longer mounted"); return kReplyNoTrack; }
        // Never reloaded once read: Rekordbox's own browser may hold its rows.
        int32_t status = 0;
        if (!field<void *>(stick, kStickTracks) && (api.stick_load(stick, &status, nullptr) != 0 ||
                                                     !field<void *>(stick, kStickTracks))) {
            snprintf(message, size, "Rekordbox could not read that USB stick's library");
            return kReplyNoTrack;
        }
    }
    alignas(16) uint8_t track[0x600];  // RowDataTrack is 0x4e0; the margin costs nothing
    static_assert(sizeof track >= kTrackSize, "track buffer");
    api.track_ctor(track);
    int result;
    void *item = track + kTrackItem;
    const auto basic_info = reinterpret_cast<BasicInfo>(field<uint64_t>(field<const void *>(item, 0), 0x8));
    if (reinterpret_cast<uint64_t>(basic_info) != kCode[TRACK_BASIC_INFO].address - kBase + api.slide) {
        snprintf(message, size, "Rekordbox's track type differs from the verified build");
        result = kReplyUnsupported;
    } else if (!api.get_row_data_track(database, c.content_id, track, device ? kDevice : kCollection,
                                       device ? static_cast<int>(c.device) : 0)) {
        snprintf(message, size, device ? "Track %u is not on that USB stick" : "Track %u is not in Rekordbox's collection",
                 c.content_id);
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

// The row Wave shows of Rekordbox's list, onto a deck: as Rekordbox's own browser loads it, whatever it is (a
// collection track, a stick's, or a Beatport result, which has no file and no library id of Wave's).
int load_row(const void *dj, int players, const BridgeCommand &c, char *message, size_t size) {
    const int deck = static_cast<int>(c.deck);
    if (deck < 0 || deck >= players) { snprintf(message, size, "Deck %d does not exist", deck + 1); return kReplyBadDeck; }
    if (api.is_playing(dj, deck)) { snprintf(message, size, "Deck %d is playing", deck + 1); return kReplyDeckPlaying; }
    if (api.is_loading(dj, deck)) { snprintf(message, size, "Deck %d is still loading", deck + 1); return kReplyDeckBusy; }
    void *player = ui_player(deck);
    const void *track = row_track(browser_list(), static_cast<int>(c.device));
    if (!player) { snprintf(message, size, "Rekordbox is still starting"); return kReplyNotReady; }
    if (!track || field<uint32_t>(track, 0x8) != c.content_id) {
        snprintf(message, size, "Rekordbox's list changed: pick the track again");
        return kReplyNoTrack;
    }
    void *item = const_cast<uint8_t *>(static_cast<const uint8_t *>(track)) + kTrackItem;
    const auto basic_info = reinterpret_cast<BasicInfo>(field<uint64_t>(field<const void *>(item, 0), 0x8));
    if (reinterpret_cast<uint64_t>(basic_info) != kCode[TRACK_BASIC_INFO].address - kBase + api.slide) {
        snprintf(message, size, "Rekordbox's track type differs from the verified build");
        return kReplyUnsupported;
    }
    void *info = basic_info(item);
    if (!info) { snprintf(message, size, "Rekordbox could not prepare that track"); return kReplyNoTrack; }
    post_load(player, info);
    snprintf(message, size, "Load requested on deck %d", deck + 1);
    return kReplyRequested;
}

// The browser's search box (browse::SearchFilter), as Rekordbox shows it above its track list.
void *search_filter() {
    const void *view = global(kBrowseView);
    const void *a = view ? field<const void *>(view, kSearchPath[0]) : nullptr;
    const void *b = a ? field<const void *>(a, kSearchPath[1]) : nullptr;
    const void *editor = b ? field<const void *>(b, kSearchPath[2]) : nullptr;
    const void *filter = editor ? field<const void *>(editor, kSearchPath[3]) : nullptr;  // the editor's parent component
    if (!filter || field<uint64_t>(filter, 0) != kSearchFilterVtable - kBase + api.slide ||
        field<uint64_t>(filter, 0xf0) != kSearchListenerVtable - kBase + api.slide || field<const void *>(filter, kSearchEditor) != editor)
        return nullptr;
    return const_cast<void *>(filter);
}

// Types the query into Rekordbox's search box and presses Return there: Rekordbox searches what its browser shows
// (Beatport's Search Tracks searches Beatport), and its results fill the list Wave reads.
int search(const BridgeCommand &c, char *message, size_t size) {
    char query[sizeof shared->search.query + 1] = {0};
    memcpy(query, shared->search.query, sizeof shared->search.query);
    uint32_t check = 0x811c9dc5u;
    for (const char *p = query; *p; ++p) check = (check ^ static_cast<uint8_t>(*p)) * 0x01000193u;
    if (check != c.content_id) { snprintf(message, size, "Search text arrived damaged"); return kReplyBadRequest; }
    void *filter = search_filter();
    if (!filter) { snprintf(message, size, "Rekordbox's search box isn't there"); return kReplyNotReady; }
    void *editor = field<void *>(filter, kSearchEditor);
    using StringCtor = JString *(*)(JString *, const char *);
    using SetText = void (*)(void *editor, const JString *, bool);
    using ReturnPressed = void (*)(void *listener, void *editor);
    JString text = {nullptr};
    reinterpret_cast<StringCtor>(kCode[STRING_CTOR].address - kBase + api.slide)(&text, query);
    reinterpret_cast<SetText>(kCode[SET_TEXT].address - kBase + api.slide)(editor, &text, true);
    api.string_dtor(&text);
    reinterpret_cast<ReturnPressed>(kCode[SEARCH_RETURN].address - kBase + api.slide)(static_cast<uint8_t *>(filter) + 0xf0, editor);
    snprintf(message, size, "Searching for %s", query);
    return kReplyRequested;
}

// Rekordbox's Show Split Screen, as its View menu and the browser's split-screen button run it: the browser in a
// window of its own. A toggle: Wave asks only while that window isn't open.
bool show_split_screen(char *message, size_t size);

// One of Rekordbox's own menu commands, run as its menu runs it (through MainComponent::perform).
bool perform_command(int command, char *message, size_t size) {
    const void *main = global(kMainComponent);
    void *target = main ? const_cast<uint8_t *>(static_cast<const uint8_t *>(main)) + kCommandTarget : nullptr;
    if (!target || field<uint64_t>(target, 0) != kCommandTargetVtable - kBase + api.slide ||
        field<uint64_t>(reinterpret_cast<const void *>(kCommandTargetVtable - kBase + api.slide), 4 * 8) !=
            kCode[PERFORM].address - kBase + api.slide) {
        snprintf(message, size, "Rekordbox is still starting");
        return false;
    }
    alignas(16) uint8_t info[0x60] = {};  // juce::ApplicationCommandTarget::InvocationInfo: the command, invoked directly
    memcpy(info, &command, sizeof command);
    using Perform = bool (*)(void *target, const void *info);
    reinterpret_cast<Perform>(kCode[PERFORM].address - kBase + api.slide)(target, info);
    snprintf(message, size, "Rekordbox command 0x%X run", command);
    return true;
}

bool show_split_screen(char *message, size_t size) {
    const void *main = global(kMainComponent);
    void *target = main ? const_cast<uint8_t *>(static_cast<const uint8_t *>(main)) + kCommandTarget : nullptr;
    if (!target || field<uint64_t>(target, 0) != kCommandTargetVtable - kBase + api.slide ||
        field<uint64_t>(reinterpret_cast<const void *>(kCommandTargetVtable - kBase + api.slide), 4 * 8) !=
            kCode[PERFORM].address - kBase + api.slide) {
        snprintf(message, size, "Rekordbox is still starting");
        return false;
    }
    alignas(16) uint8_t info[0x60] = {};  // juce::ApplicationCommandTarget::InvocationInfo: the command, invoked directly
    const int command = kShowSplitScreen;
    memcpy(info, &command, sizeof command);
    using Perform = bool (*)(void *target, const void *info);
    reinterpret_cast<Perform>(kCode[PERFORM].address - kBase + api.slide)(target, info);
    snprintf(message, size, "Rekordbox's split screen browser toggled");
    return true;
}

// LoadRow and Search work on the browser: guarded as reading it is.
int guarded_browser_command(const void *dj, int players, const BridgeCommand &c, char *message, size_t size) {
    if (browser_failed) {
        snprintf(message, size, "%s", shared->browser.tree_path + 1);
        return kReplyUnsupported;
    }
    __try {
        browser_step = c.kind == kCommandSearch ? "search" : "load row";
        return c.kind == kCommandSearch ? search(c, message, size) : load_row(dj, players, c, message, size);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        browser_fault(c.kind == kCommandSearch ? "searching" : "loading a row");
        snprintf(message, size, "%s", shared->browser.tree_path + 1);
        return kReplyUnsupported;
    }
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
    } else if (c.kind == kCommandSeek) {
        // A paused deck to a time in its track (content_id, ms), as Rekordbox's Needle Search does: CUE then sets
        // the cue there. Never a playing deck, nor one in a loop (Wave's touch is only a look then).
        const int deck = int(c.deck);
        if (deck >= players || !api.is_loaded(dj, deck)) {
            snprintf(message, sizeof message, "Deck %d has no track", deck + 1);
            result = kReplyNoTrack;
        } else if (api.is_playing(dj, deck)) {
            snprintf(message, sizeof message, "Deck %d is playing", deck + 1);
            result = kReplyDeckPlaying;
        } else if (api.is_looping(dj, deck, false)) {
            snprintf(message, sizeof message, "Deck %d is in a loop", deck + 1);
            result = kReplyDeckBusy;
        } else if (int(c.content_id) < 0 || int(c.content_id) > api.total_ms(dj, deck)) {
            snprintf(message, sizeof message, "Past the end of deck %d's track", deck + 1);
            result = kReplyBadRequest;
        } else {
            const int took = api.seek(dj, deck, int(c.content_id));
            snprintf(message, sizeof message, "Deck %d to %.1f s%s", deck + 1, c.content_id / 1000.0, took == 0 ? "" : " (refused)");
            result = took == 0 ? kReplyRequested : kReplyNotReady;
        }
    } else if (c.kind == kCommandLoadRow || c.kind == kCommandSearch) {
        result = guarded_browser_command(dj, players, c, message, sizeof message);
    } else if (c.kind == kCommandRekordbox) {
        if (c.content_id != kPreferencesCommand) {
            snprintf(message, sizeof message, "Only Rekordbox's Preferences can be opened this way");
            result = kReplyBadRequest;
        } else {
            result = perform_command(kPreferencesCommand, message, sizeof message) ? kReplyRequested : kReplyNotReady;
        }
    } else if (c.kind == kCommandBrowser && c.content_id) {
        result = show_split_screen(message, sizeof message) ? kReplyRequested : kReplyNotReady;
    } else if (c.kind == kCommandBrowser) {
        snprintf(message, sizeof message, "Wave puts the split screen browser away itself");
        result = kReplyRequested;
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
            // Rekordbox's loop as it stands, running or not: after LOOP IN (no OUT yet) its in point is already there,
            // placed as Rekordbox placed it (quantized or not). Looping only flags a loop that runs.
            CueInfo loop = {-1, -1};
            api.running_loop(dj, &loop, c);
            deck.loop_in = loop.in;
            deck.loop_out = loop.out;
            if (api.is_looping(dj, c, false) && loop.out != -1 && loop.out > loop.in) flags |= kDeckLooping;
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
    if (ticks % 5 == 0) guarded_read_browser();
    if (ticks % 200 == 1) read_devices();
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

HHOOK volatile hook = nullptr;
volatile LONG timer_started = 0;  // set on the UI thread once the tick timer runs there

// Runs on Rekordbox's UI thread the first time it takes a message: start the tick timer there,
// where Rekordbox's own UI reads the same engine, then let go of the hook.
LRESULT CALLBACK on_message(int code, WPARAM wparam, LPARAM lparam) {
    HHOOK self = reinterpret_cast<HHOOK>(InterlockedExchangePointer(reinterpret_cast<PVOID volatile *>(&hook), nullptr));
    if (self && code >= 0) {
        SetTimer(nullptr, 0, 10, on_timer);
        InterlockedExchange(&timer_started, 1);
        UnhookWindowsHookEx(self);
    } else if (self) {
        InterlockedExchangePointer(reinterpret_cast<PVOID volatile *>(&hook), self);  // not a message yet: keep waiting
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

// wait_ms: how long to wait for the UI thread to take the hook (0: don't wait). Rekordbox may load
// OpenSSL on a thread its UI thread waits for, so that path never waits; Wave attaching does.
bool start(DWORD wait_ms = 0) {
    HMODULE self;  // never unloaded while the timer can still run its code
    GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_PIN,
                       reinterpret_cast<LPCWSTR>(&start), &self);
    const DWORD thread = ui_thread();
    if (!thread) return false;
    if (thread == GetCurrentThreadId()) {
        SetTimer(nullptr, 0, 10, on_timer);
        InterlockedExchange(&timer_started, 1);
        return true;
    }
    HHOOK installed = SetWindowsHookExW(WH_GETMESSAGE, on_message, nullptr, thread);
    if (!installed) return false;
    InterlockedExchangePointer(reinterpret_cast<PVOID volatile *>(&hook), installed);
    PostThreadMessageW(thread, WM_NULL, 0, 0);  // a message to take, even while Rekordbox sits idle
    if (!wait_ms) return true;
    for (DWORD waited = 0; waited < wait_ms && !timer_started; waited += 20) {
        Sleep(20);
        if (waited % 500 == 0) PostThreadMessageW(thread, WM_NULL, 0, 0);
    }
    if (timer_started) return true;
    // The UI thread never took a message: take the hook back so it can't start later, unseen.
    if (HHOOK left = reinterpret_cast<HHOOK>(InterlockedExchangePointer(reinterpret_cast<PVOID volatile *>(&hook), nullptr)))
        UnhookWindowsHookEx(left);
    return timer_started != 0;
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

volatile LONG once = 0;  // one bridge per Rekordbox, however it arrived

enum : DWORD { kAttached = 0, kAlreadyRunning = 1, kNotRekordbox = 2, kNoSharedFile = 3, kUnsupported = 4,
               kNoUiThread = 5 };

DWORD begin(DWORD wait_ms) {
    if (!is_rekordbox()) return kNotRekordbox;
    if (InterlockedExchange(&once, 1)) return kAlreadyRunning;
    QueryPerformanceFrequency(&frequency);
    if (!open_shared()) return kNoSharedFile;
    if (const char *why = verify_build()) {
        publish_status(kStatusUnsupported, why);
        return kUnsupported;
    }
    if (!start(wait_ms)) {
        publish_status(kStatusFailed, "Could not reach Rekordbox's UI thread");
        return kNoUiThread;
    }
    // The controller: Wave reads it through Rekordbox, which holds it. Decks work either way.
    bool hooked = false;
    __try { hooked = hook_midi(); } __except (EXCEPTION_EXECUTE_HANDLER) { hooked = false; }
    InterlockedExchange(reinterpret_cast<volatile LONG *>(&shared->midi.hooked), hooked ? 1 : 0);
    return kAttached;
}

}  // namespace

extern "C" __declspec(dllexport)
int OSSL_provider_init(const void *, const void *, const OsslDispatch **out, void **ctx) {
    *out = provider_dispatch;
    *ctx = nullptr;
    char enabled[4] = {0};
    // Child processes inherit the environment; only Rekordbox's own process reads state.
    if (GetEnvironmentVariableA("RKBX_WAVE_BRIDGE", enabled, sizeof enabled) == 1 && enabled[0] == '1')
        begin(0);
    return 1;
}

// Wave attaching to a Rekordbox that was started some other way: Wave loads this library into
// Rekordbox, then runs this on a thread of its own. The same checks as above; the answer is one of
// the codes in `begin`, which Wave reads as the thread's exit code.
extern "C" __declspec(dllexport)
DWORD WINAPI wave_bridge_attach(LPVOID) {
    return begin(15000);
}
