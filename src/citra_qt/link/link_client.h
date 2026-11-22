// Copyright Azahar-Link contributors
// Licensed under GPLv3 or any later version
// Refer to the LICENSE.txt file included.

// 3DS Link: a real (modded) 3DS running the 3DS-Link app is the controller for
// Azahar: its buttons, circle pad, C-stick and touch screen go to the game, and
// the game's bottom screen is shown on the 3DS.
//
// One global client. A worker thread keeps the TCP control connection and the
// UDP input stream alive and hands every input packet to the HID service; a
// second thread sends the bottom screen. Nothing here blocks the emulation.
// The protocol is shared with the 3DS app and melonDS-Link (LinkProto.h).

#pragma once

#include <array>
#include <atomic>
#include <cstdint>
#include <deque>
#include <future>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "citra_qt/link/LinkProto.h"
#include "citra_qt/link/link_h264.h"

namespace Link {

enum class State {
    Disconnected,
    Connecting,
    Connected,
};

class Client {
public:
    Client();
    ~Client();

    /// Starts connecting in the background (returns at once). Keeps reconnecting
    /// until Disconnect, e.g. after the 3DS was closed and opened again.
    void Connect(const std::string& host);
    void Disconnect();

    State GetState() const {
        return state.load();
    }
    std::string GetHost();

    /// Messages for the user (connected, lost, ...), oldest first.
    bool PopMessage(std::string& msg);

    /// State of the console (kept after the connection is gone).
    struct ConsoleState {
        bool known = false;
        int battery = -1; // percent, -1 = unknown
        int wifi = -1;    // 0..3, -1 = unknown
        bool charging = false;
        bool lid_closed = false;
    };
    ConsoleState GetConsoleState();

    /// Name of the running game (empty = none), shown in the 3DS app.
    void SetGameName(const std::string& name);

    /// Azahar's game list for the 3DS tab of the app ("title id TAB path TAB name" lines);
    /// sent whenever it changes and on every connect.
    void SetLibrary(const std::string& text);
    /// A game picked on the 3DS (its path in Azahar's game list); true once per pick.
    bool TakeLaunchRequest(std::string& path);
    /// The 3DS asked to end the running game; true once.
    bool TakeStopRequest() {
        return stop_game_wanted.exchange(false);
    }
    /// The 3DS asked for the game list again (Y); true once.
    bool TakeLibraryRequest() {
        return library_wanted.exchange(false);
    }
    /// Send the game list again even if it did not change.
    void ResendLibrary() {
        library_dirty = true;
    }
    /// A pick from the 3DS did not start a game (cancelled or failed): the 3DS stops waiting.
    void LaunchFailed() {
        launch_failed = true;
        session_dirty = true;
    }

    /// Settings (3DS Link > Settings): what happens on every connect.
    std::atomic<bool> option_miis{true};       // take the Miis of the 3DS
    std::atomic<bool> option_system_data{true}; // take missing system data from the 3DS
    std::atomic<bool> option_h264{true};        // bottom screen as H.264 on a New 3DS
    std::atomic<int> wifi_prio{0}; // picture's WiFi class: 0 video, 1 background, 2 normal

    /// Copies the Mii database (Mii Maker) of the 3DS into Azahar now. It also happens
    /// on every connect; Azahar's own database is kept once as CFL_DB.dat.before-3ds-link.
    void RequestMiiSync() {
        mii_sync_wanted = true;
    }

    /// The clock of the 3DS (as its games see it) minus the PC's UTC time, in seconds;
    /// true once after every connect.
    bool TakeConsoleClock(int64_t& delta_seconds) {
        if (!clock_new.exchange(false)) {
            return false;
        }
        delta_seconds = clock_delta.load();
        return true;
    }

    /// The user time offset of the 3DS (config block 0x30001); false with an older app.
    bool UserTimeOffset(uint64_t& offset) const {
        offset = clock_user_offset.load();
        return clock_has_user_offset.load();
    }

    // --- 3DS file system archives (saves, extdata, system data) ---
    // Blocking: call them from a worker thread, never from the UI. They return false if the
    // connection failed; `status` then holds nothing useful, otherwise a LinkStatus.
    struct DirEntry {
        std::string name;
        bool is_dir = false;
        uint64_t size = 0;
        uint64_t hash = 0; // with_hash: FNV-1a 64 of the file
    };
    /// with_hash: also the hash of every file (the 3DS reads them for it); an older app
    /// answers BADCMD then.
    bool ArcList(const LinkArchiveReq& archive, const std::string& dir,
                 std::vector<DirEntry>& entries, uint16_t& status, bool with_hash = false);
    bool ArcRead(const LinkArchiveReq& archive, const std::string& path,
                 std::vector<uint8_t>& data, uint16_t& status);
    bool ArcInfo(const LinkArchiveReq& archive, LinkFormatInfo& info, uint16_t& status);
    /// Writes a whole file (in pieces); create: make it anew with this size.
    bool ArcWrite(const LinkArchiveReq& archive, const std::string& path,
                  const std::vector<uint8_t>& data, bool create, uint16_t& status);
    /// Simple archive commands without data (ARCDELETE, ARCMKDIR, ARCCOMMIT).
    bool ArcSimple(uint16_t cmd, const LinkArchiveReq& archive, const std::string& path,
                   uint16_t& status);
    /// Part of a file (one request, at most LINK_ARCZ_MAX_CHUNK bytes).
    bool ArcReadPart(const LinkArchiveReq& archive, const std::string& path, uint32_t offset,
                     uint32_t length, std::vector<uint8_t>& data, uint16_t& status);
    /// The RomFS (level 3) of a system data archive, as the 3DS's own file system reads it.
    bool ReadSystemRomfs(uint64_t title_id, std::vector<uint8_t>& data, uint16_t& status);
    /// Size and FNV-1a 64 of a whole file (false + status BADCMD: the app is too old).
    bool ArcHash(const LinkArchiveReq& archive, const std::string& path, uint64_t& size,
                 uint64_t& hash, uint16_t& status);
    /// The 3DS result code of the last archive call that failed (0 = none sent).
    uint32_t LastArcResult() const {
        return last_arc_result.load();
    }
    /// Title ids installed on the SD card and on the game card.
    bool Titles(std::vector<uint64_t>& sd, std::vector<uint64_t>& card, uint16_t& status);

    /// The user turned the picture on the 3DS off (setting in the 3DS app).
    bool VideoWanted() const {
        return video_wanted.load();
    }

private:
    void Worker(std::string host);
    void VideoWorker();
    bool Request(uint16_t cmd, const void* payload, uint32_t len, uint16_t& status,
                 std::vector<uint8_t>* out = nullptr);
    int TryRequest(uint16_t cmd, const void* payload, uint32_t len, uint16_t& status,
                   std::vector<uint8_t>* out = nullptr);
    bool RequestLocked(uint16_t cmd, const void* payload, uint32_t len, uint16_t& status,
                       std::vector<uint8_t>* out);
    int PollEvents();
    void PushMessage(const std::string& msg);
    void CloseSockets();
    void UpdateCapture();

    // video
    void ProcessAcks();
    void TileSlower();
    bool TileDivFits(int d) const;
    bool TilesPending() const;
    double tile_content_gap = 2;      // tiles: frames between the game's pictures, lately
    void TilePace();
    int tile_div = 2;                 // tiles: a picture every tile_div-th Azahar frame
    bool tile_held = false;           // tiles: a new picture waits for its turn
    uint64_t tile_last_emu = 0, tile_sent_emu = 0, tile_send_emu = 0;
    double tile_frame_bytes = 0;      // tiles: bytes per picture lately
    int64_t tile_room_since = 0;
    void NoteAckDelay(int64_t delay, int64_t now);
    int64_t QueueDelay() const;
    // receipts: delay of each packet until confirmed (for spotting a queue in the WiFi)
    int64_t ack_min_cur = 1000, ack_min_prev = 1000, ack_min_since = 0;
    double ack_delay_recent = 0;
    int64_t queue_cut_ms = 0;
    int32_t acks_behind = 0;          // packets sent but not yet confirmed
    double tiles_rate_actual = 0;     // tiles: bytes per second that really go out
    int64_t tiles_rate_last_ms = 0;
    std::atomic<bool> queue_squeeze{false};
    int64_t stats_ack_delay_sum = 0, stats_ack_delay_n = 0, stats_ack_delay_max = 0, stats_queue_cuts = 0;
    void SendTiles(const std::vector<uint16_t>& frame);
    void PackTilePackets(std::vector<std::vector<uint8_t>>& packets,
                         std::vector<std::vector<uint16_t>>& packet_tiles);
    void SendH264(const std::vector<uint16_t>& frame);
    void VideoStats(int64_t now);
    void CalibrateColors();
    void LogRoundTrip(const std::vector<uint8_t>& out);
    bool StatsReply(std::vector<uint8_t>& out);
    // compression test for the log (runs beside the picture, on every 3rd tile picture)
    void ProbeCompression(const std::vector<std::vector<uint8_t>>& packets);
    // (the counters before the future: it is destroyed first and waits for the test)
    std::atomic<uint64_t> cmp_raw{0}, cmp_lz4{0}, cmp_z1{0}, cmp_p1{0}, cmp_p2{0}, cmp_whole{0};
    uint32_t cmp_tick = 0;
    std::future<void> cmp_future;
    std::future<std::vector<uint8_t>> stats_future;
    static std::string MarkQos(intptr_t sock, const void* to_sockaddr_in, bool voice,
                               bool background);
    H264Encoder h264;
    std::atomic<bool> h264_capable{false}; // the picture goes as H.264 now
    std::atomic<bool> h264_supported{false}; // the 3DS decodes H.264, we can encode it
    std::atomic<bool> h264_failed{false};    // it did not work this connection: tiles
    std::atomic<bool> h264_force_key{false};
    std::atomic<bool> h264_key_wanted{false}; // the 3DS asks for a key frame (input packets)
    int64_t h264_key_ms = 0;        // when the last key frame went out
    // R, G, B -> Y, U, V for the encoder: BT.601 TV range (Y 16..235, U/V 16..240) - the
    // 3DS decoder turns it back with 1.164 (Y - 16) + 1.596 (V - 128) ... (measured: with
    // full range everything came out ~16 % brighter, the light parts cut off)
    float color_matrix[3][4] = {{0.256788f, 0.504129f, 0.097906f, 16.0f},
                                {-0.148223f, -0.290993f, 0.439216f, 128.0f},
                                {0.439216f, -0.367788f, -0.071427f, 128.0f}};
    // 0 measure and correct, 1 only check (log the difference), 2 done. Starts at 1: the
    // conversion is known now; the check stays for the log
    int calib_state = 1;
    int calib_tries = 0;
    int64_t calib_last_ms = 0;
    uint32_t h264_last_made = 0;    // the time stamp of the newest picture sent
    uint64_t h264_emu_frame = 0;
    uint64_t h264_prev_emu_frame = 0;
    uint32_t stats_src_gaps[4] = {}; // frames between changed pictures (1, 2, 3, 4-6)       // the emulated frame of the newest picture
    std::atomic<int64_t> busy_ms{0}; // last request other than polling (save transfers)
    bool h264_rs = false;           // the 3DS repairs with Reed-Solomon parity
    bool tile_rs = false;           // tiles: the 3DS repairs with parity too
    bool tile_ts = false;           // tiles: the 3DS paces them by their time stamps
    std::atomic<bool> echo_ok{false}; // the 3DS measures the input round trip
    bool tile_lz4 = false;            // tiles: the 3DS unpacks LZ4 packets
    bool tile_lz4x = false;           // ... with the packed size in the packet (MDFX)
    double lz4_ratio = 0.65;          // tiles: packed size / raw size, recently
    int64_t tiles_sent_ms = 0;        // tiles: when the last frame went out
    uint64_t watch_received = 0, watch_lost = 0; // for spotting stuck receipts
    intptr_t echo_sock = -1;          // the echo goes out with the top WiFi priority
    std::atomic<uint32_t> emu_clock_off{0}; // our clock - the newest picture's time stamp
    size_t stats_cmp_raw = 0, stats_cmp_lz4 = 0, stats_cmp_zstd1 = 0, stats_cmp_zstd9 = 0;
    uint32_t tile_last_made = 0;    // tiles: the time stamp of the last changed picture
    // the 3DS's input packets: gaps mean the WiFi is too full (the picture makes room)
    int64_t last_input_ms = 0;
    std::atomic<int64_t> input_gap_max{0};
    std::atomic<uint32_t> input_gaps{0}, input_count{0};
    uint32_t input_gaps_seen = 0, input_gaps_logged = 0;
    std::atomic<bool> input_squeeze{false};
    std::atomic<bool> capture_dirty{false}; // the lid changed: update the capture
    double stats_send_ms = 0, stats_send_max_ms = 0, stats_encode_ms = 0; // tiles: per frame
    uint32_t tile_same_count = 0;   // unchanged pictures from Azahar in a row
    uint64_t stats_skipped = 0;     // pictures of Azahar the sender had no time for
    int h264_silent = 0;            // statistics in a row in which the 3DS showed nothing
    int64_t h264_changed_ms = 0;    // when the picture last changed
    bool h264_settled = true;       // a key frame went out since it stands still
    int h264_bitrate = 8'000'000;
    bool h264_loss_since_key = true; // a packet got lost since the last key frame
    bool h264_second_key = false;    // send one more key frame right after the first
    std::vector<uint16_t> h264_last;
    std::vector<uint8_t> h264_yuv, h264_out;
    // statistics for the log (every 10 s)
    int64_t stats_since = 0;
    uint64_t stats_frames = 0, stats_bytes = 0, stats_keys = 0, stats_key_asks = 0, stats_received = 0, stats_lost = 0;

    std::atomic<State> state{State::Disconnected};
    std::atomic<bool> stop_requested{false};
    std::thread thread;
    std::thread video_thread;

    std::mutex host_mutex;
    std::string current_host;

    std::mutex msg_mutex;
    std::deque<std::string> messages;

    std::mutex console_mutex;
    ConsoleState console;
    uint32_t last_packet_id = 0;
    bool have_packet = false;

    std::mutex session_mutex;
    std::string game_name;
    std::atomic<bool> session_dirty{false}; // the 3DS does not know the game yet
    std::atomic<bool> launch_failed{false};
    std::mutex library_mutex;
    std::string library_text;
    std::atomic<bool> library_dirty{false};
    std::atomic<bool> library_unsupported{false}; // the app on the 3DS is older
    std::string launch_path;
    std::atomic<bool> launch_wanted{false};
    std::atomic<bool> stop_game_wanted{false};
    std::atomic<bool> library_wanted{false};
    void SendLibrary();
    void SendSession();

    std::atomic<bool> clock_new{false};
    std::atomic<int64_t> clock_delta{0};
    std::atomic<uint64_t> clock_user_offset{0};
    std::atomic<bool> clock_has_user_offset{false};
    void ReadConsoleClock();

    std::atomic<uint32_t> last_arc_result{0};
    std::atomic<bool> arc_compressed{true}; // the app knows LINK_CMD_ARCREADZ
    void NoteArcResult(uint16_t status, const std::vector<uint8_t>& out);

    std::atomic<bool> mii_sync_wanted{false};
    void SyncMiis(bool report_unchanged);

    std::atomic<bool> video_wanted{true}; // bottom screen picture on (3DS setting)
    std::atomic<bool> lid_closed{false};  // closed: nobody sees the picture
    std::atomic<bool> video_resync{true}; // resend everything (new connection)

    // video: what the 3DS should show
    std::mutex video_mutex; // guards the receipts below
    std::vector<uint16_t> shown_source;
    std::vector<uint8_t> tile_level; // 0 exact, 1 half, 2 quarter, 255 unknown -> resend
    std::vector<uint16_t> last_frame;
    struct SentPacket {
        uint32_t seq;
        int64_t sent_ms;
        std::vector<uint16_t> tiles;
        bool h264 = false;
        bool parity = false; // H.264 repair packet: losing it breaks nothing
        bool fec = false;    // tiles with parity after the frame (may still be rebuilt)
        uint32_t frame_end = 0; // H.264: seq of the frame's last packet (parity included)
    };
    std::deque<SentPacket> in_flight;
    uint32_t video_frame_id = 0;
    uint32_t video_seq = 0;
    uint32_t refresh_cursor = 0;
    uint32_t scan_start = 0;
    double budget_bytes = 1.5e6; // video data rate, bytes per second
    uint32_t stat_received = 0, stat_lost = 0;
    bool ack_new = false;
    bool ack_valid = false;
    uint32_t ack_seq = 0;
    static constexpr uint32_t kAckRing = 4096; // every confirmed seq (slot = seq % size)
    std::array<uint32_t, kAckRing> acked_ring{};
    uint64_t ack_mask = 0;
    uint8_t video_addr[16] = {}; // sockaddr_in of the 3DS video port
    std::atomic<bool> have_video_addr{false};

    std::mutex req_mutex; // serialises requests
    std::mutex udp_mutex; // keeps udp_sock alive for the video sender
    intptr_t tcp_sock = -1;
    intptr_t udp_sock = -1;
    uint32_t seq = 1;
};

Client& Get();

/// A line in <user>/3ds-link/link-log.txt (what the link did, for finding problems).
void Log(const std::string& line);

/// A text in the language of Azahar's interface (German or English).
std::string Text(const char* key);

/// Puts a and b in the places of %1 and %2.
std::string Format(std::string text, const std::string& a, const std::string& b = {});

} // namespace Link
