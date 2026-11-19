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

#include <atomic>
#include <cstdint>
#include <deque>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "citra_qt/link/LinkProto.h"

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

    /// Settings (3DS Link > Settings): what happens on every connect.
    std::atomic<bool> option_miis{true};       // take the Miis of the 3DS
    std::atomic<bool> option_system_data{true}; // take missing system data from the 3DS

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
    void SendTiles(const std::vector<uint16_t>& frame);

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
    void SendSession();

    std::atomic<bool> clock_new{false};
    std::atomic<int64_t> clock_delta{0};
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
    };
    std::deque<SentPacket> in_flight;
    uint32_t video_frame_id = 0;
    uint32_t video_seq = 0;
    uint32_t refresh_cursor = 0;
    uint32_t scan_start = 0;
    double budget_bytes = 24 * 1024;
    uint32_t stat_received = 0, stat_lost = 0;
    bool ack_new = false;
    bool ack_valid = false;
    uint32_t ack_seq = 0;
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
