// Shared-memory layout between the bridge (inside Rekordbox) and Wave.
// rb_waveform_core/mac_bridge.py mirrors this byte for byte; change both together.
#pragma once
#include <atomic>
#include <cstdint>

#ifndef WAVE_BRIDGE_VERSION
#define WAVE_BRIDGE_VERSION "1"
#endif

constexpr char kBridgeMagic[8] = {'R', 'K', 'W', 'B', 'R', 'D', 'G', '1'};
constexpr uint32_t kBridgeVersion = 3;
constexpr const char *kBridgeStateFile = "state.bin";
constexpr uint32_t kBridgeDecks = 4;

enum : uint32_t { kStatusStarting = 0, kStatusLive = 1, kStatusUnsupported = 2, kStatusFailed = 3 };
enum : uint32_t { kDeckLoaded = 1, kDeckPlaying = 2, kDeckLooping = 4, kDeckMasterTempo = 8, kDeckSync = 16,
                  kDeckSlip = 32, kDeckKeySync = 64 };
enum : uint32_t { kCommandLoad = 1 };
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
    uint32_t reserved;
    char path[1024];
};

struct BridgeCommand {         // written by Wave; seq last. check = FNV-1a of the 28 bytes before it
    uint32_t seq;
    uint32_t kind;
    uint32_t deck;             // 0-based channel
    uint32_t content_id;       // djmdContent.ID
    uint64_t issued_ns;        // CLOCK_UPTIME_RAW; older than 2 s is refused
    uint32_t reserved;
    uint32_t pad;
    uint64_t check;
};

struct BridgeReply {           // written by the bridge; seq = the command it answers
    uint32_t seq;
    int32_t result;
    char message[120];
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
};

static_assert(sizeof(BridgeDeck) == 1080, "layout");
static_assert(sizeof(BridgeCommand) == 40, "layout");
static_assert(sizeof(BridgeReply) == 128, "layout");
static_assert(sizeof(BridgeState) == 176 + 4 * 1080 + 40 + 128, "layout");
