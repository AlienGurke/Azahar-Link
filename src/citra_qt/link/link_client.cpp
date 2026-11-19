// Copyright Azahar-Link contributors
// Licensed under GPLv3 or any later version
// Refer to the LICENSE.txt file included.

// 3DS Link client, see link_client.h. Ported from melonDS-Link (LinkClient.cpp);
// the picture is the 3DS bottom screen (320x240) instead of the DS screen.

// the socket headers first: on Windows winsock2.h must come before windows.h
#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <winsock2.h>
#include <ws2tcpip.h>
typedef SOCKET sock_t;
#define closesock closesocket
#else
#include <arpa/inet.h>
#include <cerrno>
#include <fcntl.h>
#include <netdb.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>
typedef int sock_t;
#define closesock close
#endif

#include <algorithm>
#include <chrono>
#include <ctime>
#include <cstring>
#include <map>

#include <QLocale>

#include "citra_qt/link/link_client.h"
#include "citra_qt/link/link_save.h"
#include "citra_qt/link/lz4/lz4.h"
#include "citra_qt/uisettings.h"
#include "common/file_util.h"
#include "core/hle/service/hid/hid.h"
#include "video_core/renderer_base.h"


namespace Link {

namespace {

// the 3DS bottom screen
constexpr int kWidth = 320;
constexpr int kHeight = 240;
constexpr int kTilesX = kWidth / 8;
constexpr int kTiles = kTilesX * (kHeight / 8);
static_assert(kTiles <= static_cast<int>(LINK_MAX_TILES), "tile index does not fit");

int64_t NowMs() {
    using namespace std::chrono;
    return duration_cast<milliseconds>(steady_clock::now().time_since_epoch()).count();
}

void NetInit() {
#ifdef _WIN32
    static bool done = false;
    if (!done) {
        WSADATA wsa;
        WSAStartup(MAKEWORD(2, 2), &wsa);
        done = true;
    }
#endif
}

void SetNonBlocking(sock_t s, bool nb) {
#ifdef _WIN32
    u_long mode = nb ? 1 : 0;
    ioctlsocket(s, FIONBIO, &mode);
#else
    int flags = fcntl(s, F_GETFL, 0);
    fcntl(s, F_SETFL, nb ? (flags | O_NONBLOCK) : (flags & ~O_NONBLOCK));
#endif
}

// wait until the socket is readable (dir = 0) or writable (dir = 1)
bool WaitSock(sock_t s, int dir, int timeout_ms) {
#ifdef _WIN32
    WSAPOLLFD p = {};
    p.fd = s;
    p.events = dir ? POLLWRNORM : POLLRDNORM;
    return WSAPoll(&p, 1, timeout_ms) > 0;
#else
    pollfd p = {};
    p.fd = s;
    p.events = dir ? POLLOUT : POLLIN;
    return poll(&p, 1, timeout_ms) > 0;
#endif
}

bool Resolve(const std::string& host, uint16_t port, sockaddr_in& out) {
    addrinfo hints = {};
    hints.ai_family = AF_INET;
    addrinfo* res = nullptr;
    if (getaddrinfo(host.c_str(), nullptr, &hints, &res) != 0 || !res) {
        return false;
    }
    out = *reinterpret_cast<sockaddr_in*>(res->ai_addr);
    out.sin_port = htons(port);
    freeaddrinfo(res);
    return true;
}

bool SendAll(sock_t s, const void* buf, size_t len, int timeout_ms = 5000) {
    const char* p = static_cast<const char*>(buf);
    while (len > 0) {
        if (!WaitSock(s, 1, timeout_ms)) {
            return false;
        }
        const int n = send(s, p, static_cast<int>(len), 0);
        if (n <= 0) {
            return false;
        }
        p += n;
        len -= static_cast<size_t>(n);
    }
    return true;
}

bool RecvAll(sock_t s, void* buf, size_t len, int timeout_ms = 10000) {
    char* p = static_cast<char*>(buf);
    while (len > 0) {
        if (!WaitSock(s, 0, timeout_ms)) {
            return false;
        }
        const int n = recv(s, p, static_cast<int>(len), 0);
        if (n <= 0) {
            return false;
        }
        p += n;
        len -= static_cast<size_t>(n);
    }
    return true;
}

} // namespace

std::string Format(std::string text, const std::string& a, const std::string& b) {
    auto put = [&](const char* mark, const std::string& value) {
        const size_t pos = text.find(mark);
        if (pos != std::string::npos) {
            text.replace(pos, 2, value);
        }
    };
    put("%1", a);
    put("%2", b);
    return text;
}

void Log(const std::string& line) {
    static std::mutex log_mutex;
    std::scoped_lock lock{log_mutex};
    const std::string dir = FileUtil::GetUserPath(FileUtil::UserPath::UserDir) + "3ds-link/";
    FileUtil::CreateFullPath(dir);
    const std::string path = dir + "link-log.txt";
    std::string old;
    if (FileUtil::Exists(path)) {
        FileUtil::ReadFileToString(true, path, old);
        if (old.size() > 512 * 1024) {
            old.erase(0, old.size() - 256 * 1024); // keep the end
        }
    }
    const std::time_t t = std::time(nullptr);
    char stamp[32];
    std::strftime(stamp, sizeof(stamp), "%Y-%m-%d %H:%M:%S", std::localtime(&t));
    old += std::string(stamp) + "  " + line + "\n";
    FileUtil::WriteStringToFile(true, path, old);
}

std::string Text(const char* key) {
    struct Entry {
        const char* en;
        const char* de;
    };
    static const std::map<std::string, Entry> texts = {
        {"menu", {"3DS Link", "3DS Link"}},
        {"connect", {"Connect to 3DS...", "Mit 3DS verbinden..."}},
        {"disconnect", {"Disconnect", "Trennen"}},
        {"autoconnect", {"Connect automatically on start", "Beim Start automatisch verbinden"}},
        {"ask_ip_title", {"Connect to 3DS", "Mit 3DS verbinden"}},
        {"ask_ip", {"IP address of the 3DS (shown in the 3DS Link app):",
                    "IP-Adresse des 3DS (steht in der 3DS-Link-App):"}},
        {"connected", {"3DS connected (%1, %2)", "3DS verbunden (%1, %2)"}},
        {"lost", {"Connection to the 3DS lost - reconnecting...",
                  "Verbindung zum 3DS verloren - verbinde neu..."}},
        {"badaddr", {"Invalid address: %1", "Ungültige Adresse: %1"}},
        {"version", {"The 3DS Link app on the 3DS does not match this Azahar version. Please "
                     "install the matching app.",
                     "Die 3DS-Link-App auf dem 3DS passt nicht zu dieser Azahar-Version. Bitte "
                     "die passende App installieren."}},
        {"status_off", {"3DS: not connected", "3DS: nicht verbunden"}},
        {"status_connecting", {"3DS: connecting...", "3DS: verbinde..."}},
        {"status_on", {"3DS: connected", "3DS: verbunden"}},
        {"status_closed", {"3DS: closed", "3DS: zugeklappt"}},
        {"battery", {"battery %1%", "Akku %1 %"}},
        {"charging", {"charging", "lädt"}},
        {"status_3ds", {"Azahar: connected", "Azahar: verbunden"}},
        {"clock_set", {"Azahar now uses the clock of the 3DS (%1)",
                       "Azahar nutzt jetzt die Uhrzeit des 3DS (%1)"}},
        {"clock_restart", {"Azahar now uses the clock of the 3DS (%1) - restart the game for it",
                           "Azahar nutzt jetzt die Uhrzeit des 3DS (%1) - dafür das Spiel neu starten"}},
        {"clock_old_app", {"For the clock, please update the 3DS Link app on the 3DS",
                           "Für die Uhrzeit bitte die 3DS-Link-App auf dem 3DS aktualisieren"}},
        {"save_ask_title", {"Save from the 3DS?", "Spielstand vom 3DS?"}},
        {"save_ask", {"The 3DS is connected. Load the save of this game from the 3DS?\n\n"
                      "Azahar's save is kept as a backup first. Nothing is written to the 3DS.",
                      "Der 3DS ist verbunden. Den Spielstand dieses Spiels vom 3DS laden?\n\n"
                      "Azahars Spielstand wird vorher gesichert. Auf den 3DS wird nichts "
                      "geschrieben."}},
        {"save_ask_load", {"Load from the 3DS", "Vom 3DS laden"}},
        {"save_ask_pc", {"Use the PC save", "PC-Spielstand nutzen"}},
        {"save_progress_title", {"3DS Link", "3DS Link"}},
        {"save_reading", {"Reading the save", "Lese Spielstand"}},
        {"save_reading_ext", {"Reading extra data", "Lese Extradaten"}},
        {"save_backup", {"Making the backup...", "Lege die Sicherung an..."}},
        {"save_writing", {"Putting the save into Azahar...", "Übernehme den Spielstand in Azahar..."}},
        {"save_done", {"Save loaded from the 3DS (%1 KB)", "Spielstand vom 3DS geladen (%1 KB)"}},
        {"save_done_ext", {"Save and extra data loaded from the 3DS (%1 KB)",
                           "Spielstand und Extradaten vom 3DS geladen (%1 KB)"}},
        {"save_same", {"The save in Azahar is already the one on the 3DS",
                       "Der Spielstand in Azahar ist schon der vom 3DS"}},
        {"save_none", {"The 3DS has no save for this game (title ID %1). Starting with the PC "
                       "save.",
                       "Auf dem 3DS gibt es keinen Spielstand für dieses Spiel (Title-ID %1). Es "
                       "startet mit dem Spielstand vom PC."}},
        {"save_diag", {"Installed on the 3DS: %1. Codes of the 3DS: %2", "Auf dem 3DS installiert: %1. Codes des 3DS: %2"}},
        {"yes_sd", {"yes (SD card)", "ja (SD-Karte)"}},
        {"yes_card", {"yes (game card)", "ja (Modul)"}},
        {"no", {"no", "nein"}},
        {"save_failed_title", {"Save from the 3DS", "Spielstand vom 3DS"}},
        {"save_failed", {"The save could not be loaded from the 3DS:\n%1\n\nAzahar's save is "
                         "unchanged. Start with it?",
                         "Der Spielstand konnte nicht vom 3DS geladen werden:\n%1\n\nAzahars "
                         "Spielstand ist unverändert. Damit starten?"}},
        {"save_conn_lost", {"the connection to the 3DS was lost", "die Verbindung zum 3DS ist abgebrochen"}},
        {"save_old_app", {"the 3DS Link app on the 3DS is too old", "die 3DS-Link-App auf dem 3DS ist zu alt"}},
        {"save_read_failed", {"reading failed", "Lesen fehlgeschlagen"}},
        {"save_bad_name", {"unusual file name", "ungewöhnlicher Dateiname"}},
        {"save_write_failed", {"could not write %1", "konnte %1 nicht schreiben"}},
        {"sys_done", {"System data taken from the 3DS (%1 parts: Mii graphics, fonts, ...) - "
                      "restart the game for it",
                      "Systemdaten vom 3DS übernommen (%1 Teile: Mii-Grafik, Schrift, ...) - "
                      "dafür das Spiel neu starten"}},
        {"sys_denied", {"The 3DS Link app may not read the system data",
                        "Die 3DS-Link-App darf die Systemdaten nicht lesen"}},
        {"sys_old_app", {"For the system data, please update the 3DS Link app on the 3DS",
                         "Für die Systemdaten bitte die 3DS-Link-App auf dem 3DS aktualisieren"}},
        {"backups_open", {"Open the save backups folder", "Ordner mit den Spielstand-Sicherungen öffnen"}},
        {"miisync", {"Take the Miis from the 3DS", "Miis vom 3DS übernehmen"}},
        {"mii_done", {"Miis taken from the 3DS (Mii Maker)", "Miis vom 3DS übernommen (Mii-Maker)"}},
        {"mii_same", {"The Miis are already the same as on the 3DS",
                      "Die Miis sind schon dieselben wie auf dem 3DS"}},
        {"mii_failed", {"Could not read the Miis from the 3DS (%1)",
                        "Konnte die Miis nicht vom 3DS lesen (%1)"}},
        {"mii_old_app", {"For the Miis, please update the 3DS Link app on the 3DS",
                         "Für die Miis bitte die 3DS-Link-App auf dem 3DS aktualisieren"}},
    };
    QString lang = UISettings::values.language;
    if (lang.isEmpty()) {
        lang = QLocale::system().name();
    }
    const bool german = lang.startsWith(QStringLiteral("de"));
    const auto it = texts.find(key);
    if (it == texts.end()) {
        return key;
    }
    return german ? it->second.de : it->second.en;
}

Client::Client() {
    NetInit();
}

Client::~Client() {
    Disconnect();
}

std::string Client::GetHost() {
    std::scoped_lock lock{host_mutex};
    return current_host;
}

void Client::Connect(const std::string& host) {
    Disconnect();
    {
        std::scoped_lock lock{host_mutex};
        current_host = host;
    }
    stop_requested = false;
    state = State::Connecting;
    thread = std::thread(&Client::Worker, this, host);
    video_thread = std::thread(&Client::VideoWorker, this);
}

void Client::Disconnect() {
    stop_requested = true;
    if (thread.joinable()) {
        thread.join();
    }
    if (video_thread.joinable()) {
        video_thread.join();
    }
    have_video_addr = false;
    CloseSockets();
    state = State::Disconnected;
    Service::HID::LinkController::Clear();
    UpdateCapture();
}

void Client::CloseSockets() {
    std::scoped_lock lock{req_mutex, udp_mutex};
    if (tcp_sock != -1) {
        closesock(static_cast<sock_t>(tcp_sock));
        tcp_sock = -1;
    }
    if (udp_sock != -1) {
        closesock(static_cast<sock_t>(udp_sock));
        udp_sock = -1;
    }
}

void Client::PushMessage(const std::string& msg) {
    std::scoped_lock lock{msg_mutex};
    messages.push_back(msg);
    while (messages.size() > 8) {
        messages.pop_front();
    }
}

bool Client::PopMessage(std::string& msg) {
    std::scoped_lock lock{msg_mutex};
    if (messages.empty()) {
        return false;
    }
    msg = messages.front();
    messages.pop_front();
    return true;
}

void Client::SetGameName(const std::string& name) {
    std::scoped_lock lock{session_mutex};
    if (name != game_name) {
        game_name = name;
        session_dirty = true;
    }
}

// tell the 3DS which game runs (never waits for a busy connection)
void Client::SendSession() {
    std::vector<uint8_t> payload(1, 0);
    {
        std::scoped_lock lock{session_mutex};
        payload.insert(payload.end(), game_name.begin(), game_name.end());
    }
    session_dirty = false;
    uint16_t status = 0;
    if (TryRequest(LINK_CMD_SESSION, payload.data(), static_cast<uint32_t>(payload.size()),
                   status) != 1) {
        session_dirty = true; // try again in a moment
    }
}

Client::ConsoleState Client::GetConsoleState() {
    std::scoped_lock lock{console_mutex};
    return console;
}

// the renderer only draws the extra picture while someone looks at it
void Client::UpdateCapture() {
    const bool on = state.load() == State::Connected && video_wanted.load() && !lid_closed.load();
    VideoCore::GetLinkCapture().SetEnabled(on);
}

bool Client::Request(uint16_t cmd, const void* payload, uint32_t len, uint16_t& status,
                     std::vector<uint8_t>* out) {
    std::scoped_lock lock{req_mutex};
    return RequestLocked(cmd, payload, len, status, out);
}

int Client::TryRequest(uint16_t cmd, const void* payload, uint32_t len, uint16_t& status,
                       std::vector<uint8_t>* out) {
    std::unique_lock<std::mutex> lock(req_mutex, std::try_to_lock);
    if (!lock.owns_lock()) {
        return -1;
    }
    return RequestLocked(cmd, payload, len, status, out) ? 1 : 0;
}

bool Client::RequestLocked(uint16_t cmd, const void* payload, uint32_t len, uint16_t& status,
                           std::vector<uint8_t>* out) {
    if (tcp_sock == -1) {
        return false;
    }
    const sock_t s = static_cast<sock_t>(tcp_sock);

    LinkHeader h = {};
    h.magic = LINK_MAGIC;
    h.cmd = cmd;
    h.seq = seq++;
    h.len = len;
    bool ok = SendAll(s, &h, sizeof(h)) && (len == 0 || SendAll(s, payload, len));
    if (ok) {
        LinkHeader r = {};
        // asking for events is answered at once: a silent 3DS (closed, asleep,
        // out of reach) is noticed after 4 s, so reconnecting starts right away
        const int wait = (cmd == LINK_CMD_POLLEVENT || cmd == LINK_CMD_PING) ? 4000 : 30000;
        ok = RecvAll(s, &r, sizeof(r), wait) && r.magic == LINK_MAGIC && r.seq == h.seq &&
             r.cmd == (cmd | LINK_RESP) && r.len <= LINK_MAX_PAYLOAD;
        if (ok) {
            std::vector<uint8_t> tmp(r.len);
            ok = r.len == 0 || RecvAll(s, tmp.data(), r.len);
            if (ok) {
                status = r.status;
                if (out) {
                    out->swap(tmp);
                }
                return true;
            }
        }
    }
    // the connection is broken; the worker notices and reports it
    closesock(s);
    tcp_sock = -1;
    return false;
}

int Client::PollEvents() {
    uint16_t status = 0;
    std::vector<uint8_t> out;
    const int res = TryRequest(LINK_CMD_POLLEVENT, nullptr, 0, status, &out);
    if (res != 1 || status != LINK_OK) {
        return res;
    }
    if (out.size() < sizeof(LinkEvent)) {
        return 1;
    }
    LinkEvent ev;
    std::memcpy(&ev, out.data(), sizeof(ev));
    if (ev.type == LINK_EVENT_VIDEO) {
        const bool on = ev.reserved != 0;
        if (on && !video_wanted) {
            video_resync = true; // the 3DS has nothing: send everything
        }
        video_wanted = on;
        UpdateCapture();
    }
    // other events (game list, screenshots, backups...) are melonDS features
    return 1;
}

// The clock of the 3DS: Azahar runs on the same time, so games that remember when they
// were played (and punish a clock that went back) see no jump.
void Client::ReadConsoleClock() {
    uint16_t status = 0;
    std::vector<uint8_t> out;
    if (!Request(LINK_CMD_GETTIME, nullptr, 0, status, &out)) {
        return;
    }
    if (status == LINK_ERR_BADCMD) {
        PushMessage(Text("clock_old_app"));
        return;
    }
    if (status != LINK_OK || out.size() < 8) {
        return;
    }
    uint64_t ms_since_1900 = 0;
    std::memcpy(&ms_since_1900, out.data(), 8);
    constexpr int64_t kSeconds1900To1970 = 2208988800LL;
    const int64_t console = static_cast<int64_t>(ms_since_1900 / 1000) - kSeconds1900To1970;
    const int64_t pc = std::chrono::duration_cast<std::chrono::seconds>(
                           std::chrono::system_clock::now().time_since_epoch())
                           .count();
    clock_delta = console - pc;
    clock_new = true;
    Log("clock: 3DS minus PC (UTC) = " + std::to_string(console - pc) + " s");
}

// The Mii database of the 3DS (shared extdata F000000B, /CFL_DB.dat) goes to the place
// where Azahar's Mii selector reads it. Games like Pokemon Rumble World need a Mii.
void Client::NoteArcResult(uint16_t status, const std::vector<uint8_t>& out) {
    uint32_t code = 0;
    if (status != LINK_OK && out.size() >= 4) {
        std::memcpy(&code, out.data(), 4);
    }
    last_arc_result = code;
}

bool Client::Titles(std::vector<uint64_t>& sd, std::vector<uint64_t>& card, uint16_t& status) {
    std::vector<uint8_t> out;
    if (!Request(LINK_CMD_TITLES, nullptr, 0, status, &out)) {
        return false;
    }
    sd.clear();
    card.clear();
    size_t pos = 0;
    for (auto* list : {&sd, &card}) {
        if (status != LINK_OK || pos + 4 > out.size()) {
            break;
        }
        uint32_t n = 0;
        std::memcpy(&n, out.data() + pos, 4);
        pos += 4;
        for (uint32_t i = 0; i < n && pos + 8 <= out.size(); i++, pos += 8) {
            uint64_t id = 0;
            std::memcpy(&id, out.data() + pos, 8);
            list->push_back(id);
        }
    }
    return true;
}

bool Client::ArcList(const LinkArchiveReq& archive, const std::string& dir,
                     std::vector<DirEntry>& entries, uint16_t& status, bool with_hash) {
    std::vector<uint8_t> req(sizeof(archive));
    std::memcpy(req.data(), &archive, sizeof(archive));
    req.insert(req.end(), dir.begin(), dir.end());
    std::vector<uint8_t> out;
    if (!Request(with_hash ? LINK_CMD_ARCLISTHASH : LINK_CMD_ARCLIST, req.data(),
                 static_cast<uint32_t>(req.size()), status, &out)) {
        return false;
    }
    NoteArcResult(status, out);
    entries.clear();
    size_t pos = 0;
    while (status == LINK_OK && pos + sizeof(LinkDirEntry) <= out.size()) {
        LinkDirEntry de;
        std::memcpy(&de, out.data() + pos, sizeof(de));
        pos += sizeof(de);
        if (pos + de.nameLen > out.size()) {
            break;
        }
        DirEntry e;
        e.name.assign(reinterpret_cast<const char*>(out.data() + pos), de.nameLen);
        e.is_dir = de.isDir != 0;
        e.size = de.size;
        pos += de.nameLen;
        if (with_hash) {
            if (pos + 8 > out.size()) {
                break;
            }
            std::memcpy(&e.hash, out.data() + pos, 8);
            pos += 8;
        }
        entries.push_back(std::move(e));
    }
    return true;
}

namespace {
// one LinkArcChunk answer into `data` (appended); false = broken answer
bool TakeChunk(const std::vector<uint8_t>& part, std::vector<uint8_t>& data, uint32_t& total) {
    LinkArcChunk c;
    if (part.size() < sizeof(c)) {
        return false;
    }
    std::memcpy(&c, part.data(), sizeof(c));
    if (c.totalSize > 64 * 1024 * 1024 || c.rawLen > LINK_ARCZ_MAX_CHUNK) {
        return false;
    }
    total = c.totalSize;
    const size_t at = data.size();
    data.resize(at + c.rawLen);
    const char* src = reinterpret_cast<const char*>(part.data() + sizeof(c));
    const int src_len = static_cast<int>(part.size() - sizeof(c));
    if (c.mode == LINK_BLOCK_LZ4) {
        return LZ4_decompress_safe(src, reinterpret_cast<char*>(data.data() + at), src_len,
                                   static_cast<int>(c.rawLen)) == static_cast<int>(c.rawLen);
    }
    if (c.mode != LINK_BLOCK_RAW || static_cast<uint32_t>(src_len) != c.rawLen) {
        return false;
    }
    if (c.rawLen) {
        std::memcpy(data.data() + at, src, c.rawLen);
    }
    return true;
}
} // namespace

bool Client::ArcReadPart(const LinkArchiveReq& archive, const std::string& path, uint32_t offset,
                         uint32_t length, std::vector<uint8_t>& data, uint16_t& status) {
    LinkArchiveReq ar = archive;
    ar.offset = offset;
    ar.length = length;
    std::vector<uint8_t> req(sizeof(ar));
    std::memcpy(req.data(), &ar, sizeof(ar));
    req.insert(req.end(), path.begin(), path.end());
    std::vector<uint8_t> part;
    if (!Request(LINK_CMD_ARCREADZ, req.data(), static_cast<uint32_t>(req.size()), status,
                 &part)) {
        return false;
    }
    data.clear();
    uint32_t total = 0;
    if (status == LINK_OK && !TakeChunk(part, data, total)) {
        status = LINK_ERR_IO;
    }
    if (status != LINK_OK) {
        NoteArcResult(status, part);
    }
    return true;
}

bool Client::ReadSystemRomfs(uint64_t title_id, std::vector<uint8_t>& data, uint16_t& status) {
    data.clear();
    uint32_t total = 0;
    do {
        LinkRomfsReq r{};
        r.titleId = title_id;
        r.mediaType = LINK_MEDIA_NAND;
        r.offset = static_cast<uint32_t>(data.size());
        r.length = LINK_ARCZ_MAX_CHUNK;
        std::vector<uint8_t> part;
        if (!Request(LINK_CMD_SYSROMFS, &r, sizeof(r), status, &part)) {
            return false;
        }
        if (status != LINK_OK) {
            NoteArcResult(status, part);
            return true;
        }
        const size_t before = data.size();
        if (!TakeChunk(part, data, total)) {
            status = LINK_ERR_IO;
            return true;
        }
        if (data.size() == before) {
            break;
        }
    } while (data.size() < total);
    if (data.size() != total) {
        status = LINK_ERR_IO;
    }
    return true;
}

bool Client::ArcHash(const LinkArchiveReq& archive, const std::string& path, uint64_t& size,
                     uint64_t& hash, uint16_t& status) {
    std::vector<uint8_t> req(sizeof(archive));
    std::memcpy(req.data(), &archive, sizeof(archive));
    req.insert(req.end(), path.begin(), path.end());
    std::vector<uint8_t> out;
    if (!Request(LINK_CMD_ARCHASH, req.data(), static_cast<uint32_t>(req.size()), status, &out)) {
        return false;
    }
    NoteArcResult(status, out);
    if (status == LINK_OK) {
        if (out.size() < 16) {
            status = LINK_ERR_IO;
        } else {
            std::memcpy(&size, out.data(), 8);
            std::memcpy(&hash, out.data() + 8, 8);
        }
    }
    return true;
}

bool Client::ArcRead(const LinkArchiveReq& archive, const std::string& path,
                     std::vector<uint8_t>& data, uint16_t& status) {
    data.clear();
    // compressed pieces (saves are often mostly empty); older apps only know ARCREAD
    while (arc_compressed) {
        LinkArchiveReq ar = archive;
        ar.offset = static_cast<uint32_t>(data.size());
        ar.length = LINK_ARCZ_MAX_CHUNK;
        std::vector<uint8_t> req(sizeof(ar));
        std::memcpy(req.data(), &ar, sizeof(ar));
        req.insert(req.end(), path.begin(), path.end());
        std::vector<uint8_t> part;
        if (!Request(LINK_CMD_ARCREADZ, req.data(), static_cast<uint32_t>(req.size()), status,
                     &part)) {
            return false;
        }
        if (status == LINK_ERR_BADCMD) {
            arc_compressed = false;
            data.clear();
            break;
        }
        if (status != LINK_OK) {
            NoteArcResult(status, part);
            return true;
        }
        LinkArcChunk c;
        if (part.size() < sizeof(c)) {
            status = LINK_ERR_IO;
            return true;
        }
        std::memcpy(&c, part.data(), sizeof(c));
        if (c.totalSize > 64 * 1024 * 1024 || c.rawLen > LINK_ARCZ_MAX_CHUNK ||
            data.size() + c.rawLen > c.totalSize) {
            status = LINK_ERR_IO;
            return true;
        }
        const size_t at = data.size();
        data.resize(at + c.rawLen);
        const char* src = reinterpret_cast<const char*>(part.data() + sizeof(c));
        const int src_len = static_cast<int>(part.size() - sizeof(c));
        if (c.mode == LINK_BLOCK_LZ4) {
            if (LZ4_decompress_safe(src, reinterpret_cast<char*>(data.data() + at), src_len,
                                    static_cast<int>(c.rawLen)) != static_cast<int>(c.rawLen)) {
                status = LINK_ERR_IO;
                return true;
            }
        } else if (c.mode == LINK_BLOCK_RAW && static_cast<uint32_t>(src_len) == c.rawLen) {
            if (c.rawLen) {
                std::memcpy(data.data() + at, src, c.rawLen);
            }
        } else {
            status = LINK_ERR_IO;
            return true;
        }
        if (c.rawLen == 0 || data.size() >= c.totalSize) {
            if (data.size() != c.totalSize) {
                status = LINK_ERR_IO;
            }
            return true;
        }
    }
    uint32_t total = 0;
    // in pieces: one answer holds at most LINK_MAX_PAYLOAD
    do {
        LinkArchiveReq ar = archive;
        ar.offset = static_cast<uint32_t>(data.size());
        ar.length = 128 * 1024;
        std::vector<uint8_t> req(sizeof(ar));
        std::memcpy(req.data(), &ar, sizeof(ar));
        req.insert(req.end(), path.begin(), path.end());
        std::vector<uint8_t> part;
        if (!Request(LINK_CMD_ARCREAD, req.data(), static_cast<uint32_t>(req.size()), status,
                     &part)) {
            return false;
        }
        if (status != LINK_OK) {
            NoteArcResult(status, part);
            return true;
        }
        if (part.size() < 4) {
            status = LINK_ERR_IO;
            return true;
        }
        std::memcpy(&total, part.data(), 4);
        if (total > 64 * 1024 * 1024) {
            status = LINK_ERR_IO;
            return true;
        }
        if (part.size() == 4) {
            break;
        }
        data.insert(data.end(), part.begin() + 4, part.end());
    } while (data.size() < total);
    if (data.size() != total) {
        status = LINK_ERR_IO;
    }
    return true;
}

bool Client::ArcInfo(const LinkArchiveReq& archive, LinkFormatInfo& info, uint16_t& status) {
    std::vector<uint8_t> out;
    if (!Request(LINK_CMD_ARCINFO, &archive, sizeof(archive), status, &out)) {
        return false;
    }
    if (status == LINK_OK) {
        if (out.size() < sizeof(info)) {
            status = LINK_ERR_IO;
        } else {
            std::memcpy(&info, out.data(), sizeof(info));
        }
    }
    return true;
}

void Client::SyncMiis(bool report_unchanged) {
    const LinkArchiveReq ar = {LINK_ARCHIVE_SHARED_EXTDATA, LINK_MEDIA_NAND, LINK_MII_EXTDATA_LOW,
                               LINK_MII_EXTDATA_HIGH, 0, 0};
    uint16_t status = 0;
    std::vector<uint8_t> data;
    if (!ArcRead(ar, "/CFL_DB.dat", data, status)) {
        return; // connection gone; the next connect tries again
    }
    Log("miis: status " + std::to_string(status) + ", " + std::to_string(data.size()) +
        " bytes");
    if (status == LINK_ERR_BADCMD) {
        PushMessage(Text("mii_old_app"));
        return;
    }
    if (status != LINK_OK) {
        PushMessage(Format(Text("mii_failed"), std::to_string(status)));
        return;
    }

    // the database starts with "CFOG"
    if (data.size() < 8 || std::memcmp(data.data(), "CFOG", 4) != 0) {
        PushMessage(Format(Text("mii_failed"), "format"));
        return;
    }

    const std::string dir = FileUtil::GetUserPath(FileUtil::UserPath::NANDDir) +
                            "data/00000000000000000000000000000000/extdata/00048000/F000000B/user/";
    const std::string path = dir + "CFL_DB.dat";
    std::string current;
    if (FileUtil::Exists(path)) {
        FileUtil::ReadFileToString(false, path, current);
    }
    if (current.size() == data.size() && std::memcmp(current.data(), data.data(), data.size()) == 0) {
        if (report_unchanged) {
            PushMessage(Text("mii_same"));
        }
        return;
    }
    FileUtil::CreateFullPath(dir);
    const std::string keep = path + ".before-3ds-link";
    if (!current.empty() && !FileUtil::Exists(keep)) {
        FileUtil::Copy(path, keep); // Azahar's own Miis, kept once
    }
    const std::string_view bytes(reinterpret_cast<const char*>(data.data()), data.size());
    if (FileUtil::WriteStringToFile(false, path, bytes) != data.size()) {
        PushMessage(Format(Text("mii_failed"), "PC"));
        return;
    }
    PushMessage(Text("mii_done"));
}

void Client::Worker(std::string host) {
    sockaddr_in tcp_addr = {};
    if (!Resolve(host, LINK_TCP_PORT, tcp_addr)) {
        PushMessage(Format(Text("badaddr"), host));
        state = State::Disconnected;
        return;
    }

    bool ever_connected = false;
    while (!stop_requested) {
        state = State::Connecting;

        // --- TCP connect (non-blocking with a timeout) ---
        sock_t ts = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
        SetNonBlocking(ts, true);
        connect(ts, reinterpret_cast<sockaddr*>(&tcp_addr), sizeof(tcp_addr));
        bool ok = WaitSock(ts, 1, 1500); // a 3DS in the same WiFi answers far quicker
        if (ok) {
            int err = 0;
            socklen_t elen = sizeof(err);
            getsockopt(ts, SOL_SOCKET, SO_ERROR, reinterpret_cast<char*>(&err), &elen);
            ok = err == 0;
        }
        if (!ok) {
            closesock(ts);
            // try again soon: after the 3DS woke up, every second counts
            for (int i = 0; i < 5 && !stop_requested; i++) {
                std::this_thread::sleep_for(std::chrono::milliseconds(100));
            }
            continue;
        }
        const int one = 1;
        setsockopt(ts, IPPROTO_TCP, TCP_NODELAY, reinterpret_cast<const char*>(&one), sizeof(one));
        {
            std::scoped_lock lock{req_mutex};
            tcp_sock = static_cast<intptr_t>(ts);
        }

        // --- HELLO: our version and that we are Azahar ---
        const uint32_t hello_req[2] = {LINK_VERSION, LINK_CLIENT_AZAHAR};
        uint16_t status = 0;
        std::vector<uint8_t> resp;
        const bool hello_ok = Request(LINK_CMD_HELLO, hello_req, sizeof(hello_req), status, &resp);
        if (hello_ok && status == LINK_ERR_VERSION) {
            PushMessage(Text("version"));
            CloseSockets();
            state = State::Disconnected;
            return;
        }
        if (!hello_ok || resp.size() < sizeof(LinkHelloResp)) {
            CloseSockets();
            continue;
        }
        LinkHelloResp hello;
        std::memcpy(&hello, resp.data(), sizeof(hello));
        video_wanted = (hello.flags & LINK_HELLO_VIDEO_OFF) == 0;
        lid_closed = false;
        {
            std::scoped_lock lock{console_mutex};
            console.lid_closed = false;
            have_packet = false;
        }

        // --- UDP input stream ---
        sockaddr_in udp_addr = tcp_addr;
        udp_addr.sin_port = htons(hello.udpPort);
        const sock_t us = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
        {
            sockaddr_in local = {};
            local.sin_family = AF_INET;
            local.sin_addr.s_addr = htonl(INADDR_ANY);
            local.sin_port = 0;
            bind(us, reinterpret_cast<sockaddr*>(&local), sizeof(local));
            SetNonBlocking(us, true);
        }
        {
            std::scoped_lock lock{req_mutex, udp_mutex};
            udp_sock = static_cast<intptr_t>(us);
        }
        {
            sockaddr_in va = tcp_addr;
            va.sin_port = htons(LINK_VIDEO_PORT);
            static_assert(sizeof(va) == sizeof(video_addr), "sockaddr_in size");
            std::memcpy(video_addr, &va, sizeof(va));
            video_resync = true;
            have_video_addr = true;
        }

        state = State::Connected;
        UpdateCapture();
        Log("connected to " + host);
        const std::string name(hello.name, strnlen(hello.name, sizeof(hello.name)));
        PushMessage(Format(Text("connected"), name, host));
        ever_connected = true;
        {
            const std::string text = Text("status_3ds");
            uint16_t st = 0;
            Request(LINK_CMD_STATUSTEXT, text.data(), static_cast<uint32_t>(text.size()), st);
        }

        session_dirty = true;
        mii_sync_wanted = false;
        ReadConsoleClock();
        SyncMiis(false);
        {
            // system data Azahar lacks (Mii graphics, fonts...): once, quietly if all is there
            std::string sys_message;
            CopySystemDataFrom3DS(sys_message);
            if (!sys_message.empty()) {
                PushMessage(sys_message);
            }
        }
        int64_t last_hello = 0;
        int64_t last_ping = NowMs();
        bool lost_by_lid = false;
        while (!stop_requested) {
            const int64_t t = NowMs();
            if (t - last_hello >= 1000) {
                const uint32_t helo = LINK_UDP_HELLO;
                sendto(us, reinterpret_cast<const char*>(&helo), sizeof(helo), 0,
                       reinterpret_cast<sockaddr*>(&udp_addr), sizeof(udp_addr));
                last_hello = t;
            }
            // asking for events is the keep-alive at the same time
            if (t - last_ping >= 250) {
                if (PollEvents() == 0) {
                    break;
                }
                last_ping = t;
                if (session_dirty) {
                    SendSession();
                }
                if (mii_sync_wanted.exchange(false)) {
                    SyncMiis(true);
                }
            }
            // closed: it goes to sleep. Drop the connection now and keep knocking,
            // so it is back as soon as the 3DS is opened again
            if (lid_closed) {
                lost_by_lid = true;
                break;
            }
            {
                std::unique_lock<std::mutex> lock(req_mutex, std::try_to_lock);
                if (lock.owns_lock() && tcp_sock == -1) {
                    break;
                }
            }

            if (!WaitSock(us, 0, 50)) {
                continue;
            }

            LinkInputPacket pkt;
            sockaddr_in from = {};
            socklen_t flen = sizeof(from);
            while (recvfrom(us, reinterpret_cast<char*>(&pkt), sizeof(pkt), 0,
                            reinterpret_cast<sockaddr*>(&from), &flen) ==
                   static_cast<int>(sizeof(pkt))) {
                if (pkt.magic != LINK_MAGIC) {
                    continue;
                }
                bool closed_now = false;
                {
                    std::scoped_lock lock{console_mutex};
                    // drop reordered packets (with wrap-around)
                    if (have_packet && static_cast<uint32_t>(pkt.id - last_packet_id) > 0x7FFFFFFFu) {
                        continue;
                    }
                    have_packet = true;
                    last_packet_id = pkt.id;
                    console.known = true;
                    const uint32_t bat = pkt.status & 0xFF;
                    const uint32_t wifi = (pkt.status >> 8) & 0xFF;
                    console.battery = bat <= 100 ? static_cast<int>(bat) : -1;
                    console.wifi = wifi <= 3 ? static_cast<int>(wifi) : -1;
                    console.charging = (pkt.status & LINK_STATUS_CHARGING) != 0;
                    console.lid_closed = (pkt.status & LINK_STATUS_LIDCLOSED) != 0;
                    closed_now = console.lid_closed;
                }
                if (closed_now != lid_closed.load()) {
                    lid_closed = closed_now;
                    UpdateCapture();
                }

                Service::HID::ArticBaseController::ControllerData data{};
                data.index = pkt.id;
                data.pad = pkt.keys;
                data.c_pad_x = pkt.cpadX;
                data.c_pad_y = pkt.cpadY;
                data.touch_x = pkt.touchX;
                data.touch_y = pkt.touchY;
                data.c_stick_x = pkt.cstickX;
                data.c_stick_y = pkt.cstickY;
                Service::HID::LinkController::Set(data);

                if (pkt.flags & LINK_INPUT_VIDEOACK) {
                    std::scoped_lock vlock{video_mutex};
                    ack_seq = pkt.videoAckSeq;
                    ack_mask = pkt.videoAckMask;
                    ack_new = true;
                }
            }
        }

        have_video_addr = false;
        CloseSockets();
        Service::HID::LinkController::Clear();
        state = State::Connecting;
        UpdateCapture();
        if (!stop_requested && ever_connected && !lost_by_lid) {
            PushMessage(Text("lost"));
        }
        if (lost_by_lid) {
            // it is falling asleep right now: knocking in the next moment would
            // only get a connection that dies with the sleep
            for (int i = 0; i < 15 && !stop_requested; i++) {
                std::this_thread::sleep_for(std::chrono::milliseconds(100));
            }
        }
    }

    state = State::Disconnected;
}

void Client::VideoWorker() {
    std::vector<uint16_t> frame;
    u64 frame_seq = 0;
    while (!stop_requested) {
        const bool got_frame = VideoCore::GetLinkCapture().Take(frame, frame_seq,
                                                                std::chrono::milliseconds(30));
        if (!have_video_addr || !video_wanted || lid_closed) {
            continue;
        }

        ProcessAcks();

        if (got_frame) {
            last_frame = frame;
            SendTiles(frame);
        } else if (!last_frame.empty()) {
            // nothing new (e.g. emulation paused): still repair lost tiles and
            // replace previews with the exact picture - but once all is exact, stop
            bool pending = !in_flight.empty();
            for (const uint8_t l : tile_level) {
                if (l != 0) {
                    pending = true;
                    break;
                }
            }
            if (pending) {
                SendTiles(last_frame);
            }
        }
    }
}

// Decide for packets in flight whether the 3DS got them; lost packets get
// their tiles queued for resending right away.
void Client::ProcessAcks() {
    bool valid;
    uint32_t a_seq;
    uint64_t a_mask;
    {
        std::scoped_lock lock{video_mutex};
        valid = ack_valid || ack_new;
        ack_valid = valid;
        ack_new = false;
        a_seq = ack_seq;
        a_mask = ack_mask;
    }

    const int64_t now = NowMs();
    while (!in_flight.empty()) {
        SentPacket& p = in_flight.front();
        bool received = false, lost = false;
        if (valid) {
            const int32_t d = static_cast<int32_t>(a_seq - p.seq);
            if (d >= 0) {
                received = d < 64 && ((a_mask >> d) & 1);
                lost = !received && d >= 3; // newer packets arrived, this one did not
            }
        }
        // no receipt in time (e.g. it was the last packet before a pause)
        if (!received && now - p.sent_ms > 150) {
            lost = true;
        }
        if (!received && !lost) {
            break;
        }
        if (lost) {
            for (const uint16_t t : p.tiles) {
                tile_level[t] = 255;
            }
            stat_lost++;
        } else {
            stat_received++;
        }
        in_flight.pop_front();
    }

    // never let the list grow without bound
    while (in_flight.size() > 512) {
        for (const uint16_t t : in_flight.front().tiles) {
            tile_level[t] = 255;
        }
        in_flight.pop_front();
    }
}

namespace {

// Encodes one 8x8 tile (rows kWidth apart) as compactly as possible. Returns the
// tile type and writes the payload (at most 128 bytes) into `out`.
unsigned EncodeTile(const uint16_t* src, uint8_t* out, size_t& size) {
    uint16_t pal[16];
    int n = 0;
    for (int y = 0; y < 8 && n <= 16; y++) {
        for (int x = 0; x < 8; x++) {
            const uint16_t c = src[y * kWidth + x];
            int i = 0;
            while (i < n && pal[i] != c) {
                i++;
            }
            if (i == n) {
                if (n == 16) {
                    n = 17;
                    break;
                }
                pal[n++] = c;
            }
        }
    }

    auto index_of = [&](uint16_t c) {
        int i = 0;
        while (pal[i] != c) {
            i++;
        }
        return i;
    };

    if (n == 1) {
        std::memcpy(out, &pal[0], 2);
        size = 2;
        return LINK_TILE_SOLID_T;
    }
    if (n == 2) {
        std::memcpy(out, pal, 4);
        for (int y = 0; y < 8; y++) {
            uint8_t row = 0;
            for (int x = 0; x < 8; x++) {
                if (index_of(src[y * kWidth + x])) {
                    row |= static_cast<uint8_t>(1 << x);
                }
            }
            out[4 + y] = row;
        }
        size = 12;
        return LINK_TILE_PAL2;
    }
    if (n <= 4) {
        std::memset(out, 0, 8);
        std::memcpy(out, pal, static_cast<size_t>(n) * 2);
        for (int y = 0; y < 8; y++) {
            uint16_t row = 0;
            for (int x = 0; x < 8; x++) {
                row |= static_cast<uint16_t>(index_of(src[y * kWidth + x]) << (x * 2));
            }
            std::memcpy(out + 8 + y * 2, &row, 2);
        }
        size = 24;
        return LINK_TILE_PAL4;
    }
    if (n <= 16) {
        std::memset(out, 0, 32);
        std::memcpy(out, pal, static_cast<size_t>(n) * 2);
        for (int y = 0; y < 8; y++) {
            uint32_t row = 0;
            for (int x = 0; x < 8; x++) {
                row |= static_cast<uint32_t>(index_of(src[y * kWidth + x])) << (x * 4);
            }
            std::memcpy(out + 32 + y * 4, &row, 4);
        }
        size = 64;
        return LINK_TILE_PAL16;
    }
    for (int y = 0; y < 8; y++) {
        std::memcpy(out + y * 16, &src[y * kWidth], 16);
    }
    size = 128;
    return LINK_TILE_RAW;
}

// lossy previews: average 2x2 (HALF, 4x4 result) or 4x4 (QUARTER, 2x2 result) blocks
void EncodeReduced(const uint16_t* src, int block, uint16_t* out) {
    const int n = 8 / block;
    for (int by = 0; by < n; by++) {
        for (int bx = 0; bx < n; bx++) {
            unsigned r = 0, g = 0, b = 0;
            for (int dy = 0; dy < block; dy++) {
                for (int dx = 0; dx < block; dx++) {
                    const uint16_t c = src[(by * block + dy) * kWidth + bx * block + dx];
                    r += c >> 11;
                    g += (c >> 5) & 0x3F;
                    b += c & 0x1F;
                }
            }
            const unsigned cnt = static_cast<unsigned>(block * block);
            out[by * n + bx] =
                static_cast<uint16_t>(((r / cnt) << 11) | ((g / cnt) << 5) | (b / cnt));
        }
    }
}

struct EncTile {
    int t;
    unsigned type;
    size_t size;
    uint8_t level; // 0 exact, 1 half, 2 quarter
    uint8_t data[128];
};

void MakeLevel(EncTile& e, const uint16_t* src, uint8_t level) {
    e.level = level;
    if (level == 0) {
        e.type = EncodeTile(src, e.data, e.size);
    } else {
        uint16_t px[16];
        EncodeReduced(src, level == 1 ? 2 : 4, px);
        e.type = level == 1 ? LINK_TILE_HALF : LINK_TILE_QUARTER;
        e.size = level == 1 ? 32 : 8;
        std::memcpy(e.data, px, e.size);
        // exact encodings that are already tiny are never worth reducing
        uint8_t tmp[128];
        size_t exact_size;
        const unsigned exact_type = EncodeTile(src, tmp, exact_size);
        if (exact_size <= e.size) {
            e.type = exact_type;
            e.size = exact_size;
            std::memcpy(e.data, tmp, exact_size);
            e.level = 0;
        }
    }
}

int TileBase(int t) {
    return (t / kTilesX) * 8 * kWidth + (t % kTilesX) * 8;
}

} // namespace

void Client::SendTiles(const std::vector<uint16_t>& frame) {
    // Rule: every tile whose content changed is sent in the same update, so the
    // 3DS never shows a mix of old and new. If that does not fit the budget,
    // tiles are sent coarser (half, then quarter resolution). Spare budget is
    // used to sharpen previews and to refresh a few tiles.
    constexpr int kRefreshPerFrame = 4;
    constexpr double kMinBudget = 12 * 1024, kMaxBudget = 64 * 1024;

    if (frame.size() != static_cast<size_t>(kWidth * kHeight)) {
        return;
    }
    if (video_resync.exchange(false) || shown_source.size() != frame.size()) {
        shown_source.assign(frame.size(), 0);
        tile_level.assign(kTiles, 255); // send everything
        in_flight.clear();
        {
            std::scoped_lock lock{video_mutex};
            ack_valid = ack_new = false;
        }
        if (video_seq == 0) { // start where a previous session cannot be ahead
            video_seq = static_cast<uint32_t>(NowMs() * 4);
        }
        budget_bytes = 24 * 1024;
        stat_received = stat_lost = 0;
    }

    // adapt the data rate: more while the WiFi keeps up, less when packets get lost
    if (stat_received + stat_lost >= 30) {
        const double loss = static_cast<double>(stat_lost) / (stat_received + stat_lost);
        if (loss > 0.05) {
            budget_bytes *= 0.75;
        } else if (loss < 0.02) {
            budget_bytes += 2048;
        }
        budget_bytes = std::min(kMaxBudget, std::max(kMinBudget, budget_bytes));
        stat_received = stat_lost = 0;
    }
    const size_t budget = static_cast<size_t>(budget_bytes);
    const double bytes_per_sec = std::min(6.0e6, std::max(2.0e6, budget_bytes * 120.0));

    auto content_changed = [&](int t) {
        const int base = TileBase(t);
        for (int y = 0; y < 8; y++) {
            if (std::memcmp(&frame[base + y * kWidth], &shown_source[base + y * kWidth], 16) != 0) {
                return true;
            }
        }
        return false;
    };

    // 1) mandatory: changed content or unknown state (lost packets)
    std::vector<EncTile> must;
    std::vector<uint8_t> is_must(kTiles, 0);
    size_t total = 0;
    for (int t = 0; t < kTiles; t++) {
        if (tile_level[t] != 255 && !content_changed(t)) {
            continue;
        }
        is_must[t] = 1;
        EncTile e;
        e.t = t;
        MakeLevel(e, &frame[TileBase(t)], 0);
        total += 2 + e.size;
        must.push_back(e);
    }

    // degrade the biggest tiles step by step until everything fits
    for (uint8_t level = 1; level <= 2 && total > budget; level++) {
        std::vector<size_t> idx(must.size());
        for (size_t i = 0; i < idx.size(); i++) {
            idx[i] = i;
        }
        std::sort(idx.begin(), idx.end(),
                  [&](size_t a, size_t b) { return must[a].size > must[b].size; });
        for (const size_t i : idx) {
            if (total <= budget) {
                break;
            }
            EncTile& e = must[i];
            if (e.level >= level) {
                continue;
            }
            const size_t before = e.size;
            MakeLevel(e, &frame[TileBase(e.t)], level);
            total = total - before + e.size;
        }
    }

    // 2) optional, with what is left: sharpen previews, refresh a few tiles
    std::vector<EncTile> extra;
    size_t room = total < budget ? budget - total : 0;
    std::vector<uint8_t> is_refresh(kTiles, 0);
    for (int i = 0; i < kRefreshPerFrame; i++) {
        is_refresh[(refresh_cursor + static_cast<uint32_t>(i)) % kTiles] = 1;
    }
    refresh_cursor = (refresh_cursor + kRefreshPerFrame) % kTiles;
    int last_extra = -1;
    for (int i = 0; i < kTiles && room > 12; i++) {
        const int t = static_cast<int>((scan_start + static_cast<uint32_t>(i)) % kTiles);
        if (is_must[t]) {
            continue;
        }
        if (tile_level[t] == 0 && !is_refresh[t]) {
            continue;
        }
        EncTile e;
        e.t = t;
        MakeLevel(e, &frame[TileBase(t)], 0);
        if (2 + e.size > room && tile_level[t] == 2) {
            MakeLevel(e, &frame[TileBase(t)], 1); // at least one step sharper
        }
        if (2 + e.size > room || (e.level >= tile_level[t] && !is_refresh[t])) {
            continue;
        }
        room -= 2 + e.size;
        extra.push_back(e);
        last_extra = t;
    }
    if (last_extra >= 0) {
        scan_start = static_cast<uint32_t>((last_extra + 1) % kTiles);
    }

    if (must.empty() && extra.empty()) {
        return;
    }

    // 3) pack: mandatory tiles first, then the extras
    std::vector<std::vector<uint8_t>> packets;
    std::vector<std::vector<uint16_t>> packet_tiles;
    std::vector<uint8_t> cur;
    std::vector<uint16_t> cur_tiles;
    uint16_t cur_entries = 0;
    auto finish = [&]() {
        if (!cur_entries) {
            return;
        }
        LinkFramePacket hdr = {};
        hdr.magic = LINK_FRAME_MAGIC;
        hdr.entryCount = cur_entries;
        std::memcpy(cur.data(), &hdr, sizeof(hdr));
        packets.push_back(std::move(cur));
        packet_tiles.push_back(std::move(cur_tiles));
        cur.clear();
        cur_tiles.clear();
        cur_entries = 0;
    };
    auto add = [&](const EncTile& e) {
        const size_t need = 2 + e.size;
        if (cur.empty()) {
            cur.resize(sizeof(LinkFramePacket));
        }
        if (cur.size() + need > LINK_FRAME_MAX_BYTES) {
            finish();
            cur.resize(sizeof(LinkFramePacket));
        }
        const uint16_t head = static_cast<uint16_t>(e.t | (e.type << LINK_TILE_TYPE_SHIFT));
        const size_t off = cur.size();
        cur.resize(off + need);
        std::memcpy(&cur[off], &head, 2);
        std::memcpy(&cur[off + 2], e.data, e.size);
        cur_entries++;
        cur_tiles.push_back(static_cast<uint16_t>(e.t));

        const int base = TileBase(e.t);
        for (int y = 0; y < 8; y++) {
            std::memcpy(&shown_source[base + y * kWidth], &frame[base + y * kWidth], 16);
        }
        tile_level[e.t] = e.level;
    };
    for (const EncTile& e : must) {
        add(e);
    }
    for (const EncTile& e : extra) {
        add(e);
    }
    finish();
    if (packets.empty()) {
        return;
    }

    // everything before base_seq is confirmed, or lost and repaired by this frame
    const uint32_t base_seq = in_flight.empty() ? video_seq : in_flight.front().seq;

    sockaddr_in to;
    std::memcpy(&to, video_addr, sizeof(to));
    const uint32_t frame_id = ++video_frame_id;
    const auto t0 = std::chrono::steady_clock::now();
    size_t sent_bytes = 0;
    for (size_t i = 0; i < packets.size(); i++) {
        std::vector<uint8_t>& pk = packets[i];
        LinkFramePacket hdr;
        std::memcpy(&hdr, pk.data(), sizeof(hdr));
        hdr.frameId = frame_id;
        hdr.seq = video_seq++;
        hdr.baseSeq = base_seq;
        hdr.flags = static_cast<uint16_t>((i << LINK_FRAME_INDEX_SHIFT) |
                                          (i + 1 == packets.size() ? LINK_FRAME_LAST : 0));
        std::memcpy(pk.data(), &hdr, sizeof(hdr));
        {
            // own lock: requests must not hold up the picture
            std::scoped_lock lock{udp_mutex};
            if (udp_sock == -1) {
                return;
            }
            sendto(static_cast<sock_t>(udp_sock), reinterpret_cast<const char*>(pk.data()),
                   static_cast<int>(pk.size()), 0, reinterpret_cast<sockaddr*>(&to), sizeof(to));
        }
        in_flight.push_back({hdr.seq, NowMs(), std::move(packet_tiles[i])});
        sent_bytes += pk.size();

        // pacing, so the 3DS WiFi does not get the whole frame as one burst
        const auto due = t0 + std::chrono::microseconds(
                                  static_cast<long long>(sent_bytes * 1e6 / bytes_per_sec));
        while (std::chrono::steady_clock::now() < due && !stop_requested) {
            std::this_thread::yield();
        }
    }
}

Client& Get() {
    static Client client;
    return client;
}

} // namespace Link
