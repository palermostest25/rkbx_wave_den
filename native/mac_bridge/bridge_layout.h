// Shared-memory layout between the bridge (inside Rekordbox) and Wave.
// rb_waveform_core/mac_bridge.py mirrors this byte for byte; change both together.
#pragma once
#include <atomic>
#include <cstdint>

#ifndef WAVE_BRIDGE_VERSION
#define WAVE_BRIDGE_VERSION "1"
#endif

constexpr char kBridgeMagic[8] = {'R', 'K', 'W', 'B', 'R', 'D', 'G', '1'};
constexpr uint32_t kBridgeVersion = 5;
constexpr const char *kBridgeStateFile = "state.bin";
constexpr uint32_t kBridgeDecks = 4;
constexpr uint32_t kBridgeDevices = 4;
constexpr uint32_t kBrowseRows = 64;

enum : uint32_t { kStatusStarting = 0, kStatusLive = 1, kStatusUnsupported = 2, kStatusFailed = 3 };
enum : uint32_t { kDeckLoaded = 1, kDeckPlaying = 2, kDeckLooping = 4, kDeckMasterTempo = 8, kDeckSync = 16,
                  kDeckSlip = 32, kDeckKeySync = 64 };
enum : uint32_t { kCommandLoad = 1, kCommandMasterTempo = 2, kCommandLoadDevice = 3 };
enum : int32_t { kReplyRequested = 1, kReplyBadRequest = -1, kReplyExpired = -2, kReplyBadDeck = -3,
                 kReplyDeckPlaying = -4, kReplyDeckBusy = -5, kReplyNoTrack = -6, kReplyNotReady = -7,
                 kReplyUnsupported = -8 };

struct BridgeDeck {            // 1080 bytes
    uint32_t flags;
    int32_t total_ms;
    double position_ms;
    float bpm;                 // as getBpm returns it; Wave calibrates the unit
    float original_bpm;
    float tempo;               // percent
    uint32_t path_seq;
    int64_t loop_in;           // -1 when no loop is running
    int64_t loop_out;
    int32_t key_shift;         // semitones
    int32_t tempo_range;       // the deck's tempo range setting as Rekordbox numbers it, -1 unknown
    char path[1024];
};

struct BridgeCommand {         // written by Wave; check = FNV-1a of the 32 bytes before it
    uint32_t seq;
    uint32_t kind;
    uint32_t deck;             // 0-based channel
    uint32_t content_id;       // track id (djmdContent.ID, or the stick's content_id); for kCommandMasterTempo 0/1
    uint64_t issued_ns;        // CLOCK_UPTIME_RAW; older than 2 s is refused
    int64_t device;            // a handle from BridgeState::devices, for kCommandLoadDevice
    uint64_t check;
};

struct BridgeDevice {          // a mounted rekordbox device (USB stick) as Rekordbox knows it
    int64_t handle;            // 0: none
    char root[248];
};

struct BridgeReply {           // written by the bridge; seq = the command it answers
    uint32_t seq;
    int32_t result;
    char message[120];
};

struct BrowseRow {             // one row of Rekordbox's track list
    uint32_t id;               // the track's id in its library
    uint32_t reserved;
    char path[256];            // its file: tells the collection from a stick
};

struct BridgeBrowser {         // Rekordbox's own browser, as its controller moves through it
    int32_t rows;              // rows in the track list, -1 while there is none
    int32_t cursor;            // the row its cursor is on, -1 for none
    int32_t first;             // the list row rows[0] shows
    int32_t count;             // rows[] filled
    uint32_t focus;            // 0 track list, 1 playlist tree, 2 neither
    uint32_t reserved[3];
    char tree_path[512];       // where the tree's cursor is, as Rekordbox names it
    BrowseRow rows_[kBrowseRows];
};

struct BridgeState {           // seqlock: odd while the bridge writes
    char magic[8];
    uint32_t version;
    std::atomic<uint32_t> seq;
    uint32_t pid;
    uint32_t status;
    uint32_t deck_count;
    int32_t master;            // channel Rekordbox follows as tempo master, -1 for none
    uint64_t ticks;
    uint64_t uptime_ns;        // CLOCK_UPTIME_RAW at the last tick: a heartbeat
    char note[128];
    BridgeDeck decks[kBridgeDecks];
    BridgeCommand command;
    BridgeReply reply;
    BridgeDevice devices[kBridgeDevices];
    BridgeBrowser browser;
};

static_assert(sizeof(BridgeDeck) == 1080, "layout");
static_assert(sizeof(BridgeCommand) == 40, "layout");
static_assert(sizeof(BridgeReply) == 128, "layout");
static_assert(sizeof(BridgeDevice) == 256, "layout");
static_assert(sizeof(BrowseRow) == 264, "layout");
static_assert(sizeof(BridgeBrowser) == 32 + 512 + 64 * 264, "layout");
static_assert(sizeof(BridgeState) == 176 + 4 * 1080 + 40 + 128 + 4 * 256 + 32 + 512 + 64 * 264, "layout");
