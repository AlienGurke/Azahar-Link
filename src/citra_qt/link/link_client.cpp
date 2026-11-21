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
#include <fmt/format.h>

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
        {"settings", {"Settings", "Einstellungen"}},
        {"opt_clock", {"Use the clock of the 3DS", "Uhrzeit vom 3DS übernehmen"}},
        {"opt_miis", {"Take the Miis of the 3DS", "Miis vom 3DS übernehmen"}},
        {"opt_sys", {"Take missing system data from the 3DS (Mii graphics, fonts)",
                     "Fehlende Systemdaten vom 3DS übernehmen (Mii-Grafik, Schriften)"}},
        {"opt_save", {"Save when a game starts", "Spielstand beim Spielstart"}},
        {"opt_save_ask", {"Ask", "Fragen"}},
        {"opt_save_always", {"Always load from the 3DS", "Immer vom 3DS laden"}},
        {"opt_save_never", {"Never (use the PC save)", "Nie (Spielstand vom PC nutzen)"}},
        {"opt_write", {"Write the save back to the 3DS when the game stops",
                       "Spielstand beim Beenden auf den 3DS zurückschreiben"}},
        {"opt_write_ask", {"Ask before writing to the 3DS", "Vor dem Schreiben auf den 3DS fragen"}},
        {"opt_write_live", {"Also send it to the 3DS whenever the game saves",
                            "Auch bei jedem Speichern im Spiel an den 3DS senden"}},
        {"opt_backup_days", {"Daily backups: keep %1 days...", "Tägliche Backups: %1 Tage behalten..."}},
        {"opt_backup_days_title", {"Daily backups", "Tägliche Backups"}},
        {"opt_backup_days_ask", {"Besides the newest 20 backups of a game, the first backup of "
                                 "each day is kept for this many days (0 = none):",
                                 "Neben den neuesten 20 Backups eines Spiels wird das erste "
                                 "Backup jedes Tages so viele Tage aufgehoben (0 = keine):"}},
        {"opt_h264", {"Bottom screen as H.264 on a New 3DS (smoother)",
                      "Unteres Bild als H.264 auf dem New 3DS (flüssiger)"}},
        {"live_done", {"Save sent to the 3DS (checked)", "Spielstand an den 3DS gesendet (geprüft)"}},
        {"live_stopped", {"Sending to the 3DS while playing stopped: %1",
                          "Senden an den 3DS während des Spielens angehalten: %1"}},
        {"write_title", {"Save to the 3DS", "Spielstand auf den 3DS"}},
        {"write_ask", {"Write the save of this game back to the 3DS?\n\nThe save on the 3DS is "
                       "kept as a backup on the PC first.",
                       "Den Spielstand dieses Spiels auf den 3DS zurückschreiben?\n\nDer "
                       "Spielstand auf dem 3DS wird vorher am PC gesichert."}},
        {"write_no_3ds", {"The 3DS is not connected: the save was NOT written back to it.",
                          "Der 3DS ist nicht verbunden: Der Spielstand wurde NICHT auf ihn "
                          "zurückgeschrieben."}},
        {"write_no_base", {"The save was not loaded from the 3DS, so it is not written back.",
                           "Der Spielstand wurde nicht vom 3DS geladen, darum wird er nicht "
                           "zurückgeschrieben."}},
        {"write_nothing", {"The 3DS has this save already", "Der 3DS hat diesen Spielstand schon"}},
        {"write_checking", {"Checking the save on the 3DS...", "Prüfe den Spielstand auf dem 3DS..."}},
        {"write_conflict", {"The save on the 3DS changed since it was loaded (played on the 3DS "
                            "meanwhile?). Nothing was written; both saves are unchanged.",
                            "Der Spielstand auf dem 3DS hat sich seit dem Laden geändert "
                            "(zwischendurch auf dem 3DS gespielt?). Es wurde nichts geschrieben, "
                            "beide Spielstände sind unverändert."}},
        {"write_writing", {"Writing", "Schreibe"}},
        {"write_verifying", {"Checking what was written...", "Prüfe das Geschriebene..."}},
        {"write_verify_failed", {"what was written did not check out; the 3DS keeps its save",
                                 "das Geschriebene stimmte nicht; der 3DS behält seinen Spielstand"}},
        {"save_write_3ds_failed", {"writing to the 3DS failed", "Schreiben auf den 3DS fehlgeschlagen"}},
        {"write_done", {"Save written back to the 3DS (checked)",
                        "Spielstand auf den 3DS zurückgeschrieben (geprüft)"}},
        {"write_failed", {"The save could not be written to the 3DS:\n%1",
                          "Der Spielstand konnte nicht auf den 3DS geschrieben werden:\n%1"}},
        {"repair", {"Repair the save on the 3DS (\"save damaged\")...",
                    "Spielstand auf dem 3DS reparieren („Speicherdaten defekt“)..."}},
        {"repair_ask", {"Delete the secure value of the save of %1 on the 3DS?\n\nUse this when "
                        "the game on the 3DS calls a save written back from Azahar damaged. The "
                        "save itself is not changed.",
                        "Den Prüfwert (Secure Value) des Spielstands von %1 auf dem 3DS "
                        "löschen?\n\nDas hilft, wenn das Spiel auf dem 3DS einen von Azahar "
                        "zurückgeschriebenen Spielstand als defekt meldet. Der Spielstand selbst "
                        "wird nicht verändert."}},
        {"repair_none", {"No game yet: load a save from the 3DS first.",
                         "Noch kein Spiel: zuerst einen Spielstand vom 3DS laden."}},
        {"repair_done", {"Done - start the game on the 3DS", "Erledigt - jetzt das Spiel auf dem 3DS starten"}},
        {"repair_failed", {"Did not work (%1). Is the 3DS connected and the app up to date?",
                           "Hat nicht geklappt (%1). Ist der 3DS verbunden und die App aktuell?"}},
        {"backups_open", {"Open the save backups folder", "Ordner mit den Spielstand-Sicherungen öffnen"}},
        {"miisync", {"Take the Miis from the 3DS now", "Miis jetzt vom 3DS übernehmen"}},
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

void Client::SetLibrary(const std::string& text) {
    std::scoped_lock lock{library_mutex};
    if (text != library_text) {
        library_text = text;
        library_dirty = true;
    }
}

bool Client::TakeLaunchRequest(std::string& path) {
    if (!launch_wanted.exchange(false)) {
        return false;
    }
    std::scoped_lock lock{library_mutex};
    path = launch_path;
    return true;
}

// the game list for the 3DS tab (never waits for a busy connection)
void Client::SendLibrary() {
    if (library_unsupported) {
        library_dirty = false;
        return;
    }
    std::string text;
    {
        std::scoped_lock lock{library_mutex};
        text = library_text;
    }
    if (text.size() > LINK_MAX_PAYLOAD) {
        text.resize(text.rfind('\n', LINK_MAX_PAYLOAD - 1) + 1);
    }
    library_dirty = false;
    uint16_t status = 0;
    const int res = TryRequest(LINK_CMD_LIBRARY, text.data(), static_cast<uint32_t>(text.size()),
                               status);
    if (res != 1) {
        library_dirty = true; // try again in a moment
    } else if (status == LINK_ERR_BADCMD) {
        library_unsupported = true;
        Log("library: the 3DS app is too old for the 3DS game list");
    } else {
        Log("library: sent " + std::to_string(text.size()) + " bytes");
    }
}

// tell the 3DS which game runs (never waits for a busy connection)
void Client::SendSession() {
    std::vector<uint8_t> payload(1, 0);
    if (launch_failed.exchange(false)) {
        payload[0] = LINK_SESSION_LAUNCH_DONE;
    }
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
    // saves and games going over the WiFi make picture packets get lost more often
    if (cmd != LINK_CMD_POLLEVENT && cmd != LINK_CMD_PING) {
        busy_ms = NowMs();
    }
    const bool ok = RequestLocked(cmd, payload, len, status, out);
    if (cmd != LINK_CMD_POLLEVENT && cmd != LINK_CMD_PING) {
        busy_ms = NowMs();
    }
    return ok;
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
    if (ev.type == LINK_EVENT_LAUNCH) {
        // a game picked on the 3DS: the path from our own game list
        const size_t len = std::min<size_t>(ev.romPathLen, out.size() - sizeof(ev));
        {
            std::scoped_lock lock{library_mutex};
            launch_path.assign(reinterpret_cast<const char*>(out.data() + sizeof(ev)), len);
        }
        launch_wanted = true;
        Log("3DS: start " + std::string(reinterpret_cast<const char*>(out.data() + sizeof(ev)), len));
    } else if (ev.type == LINK_EVENT_STOPGAME) {
        stop_game_wanted = true;
        Log("3DS: end the game");
    } else if (ev.type == LINK_EVENT_LIBRARY) {
        library_wanted = true;
    }
    // other events (screenshots, backups...) are melonDS features
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
    uint64_t user_offset = 0;
    if (out.size() >= 16) {
        std::memcpy(&user_offset, out.data() + 8, 8);
    }
    const bool has_user_offset = out.size() >= 16 && user_offset != LINK_TIME_UNKNOWN;
    clock_user_offset = user_offset;
    clock_has_user_offset = has_user_offset;
    clock_delta = console - pc;
    clock_new = true;
    Log("clock: 3DS minus PC (UTC) = " + std::to_string(console - pc) + " s" +
        (has_user_offset ? ", user time offset " + std::to_string(static_cast<int64_t>(user_offset))
                         : std::string(", user time offset unknown (old app)")));
    if (out.size() >= 40) {
        // the clock values next to each other, to find out what they mean
        uint64_t v[5] = {};
        std::memcpy(v, out.data(), sizeof(v));
        const auto as_text = [](uint64_t ms) {
            if (ms == LINK_TIME_UNKNOWN) {
                return std::string("?");
            }
            const std::time_t t = static_cast<std::time_t>(ms / 1000 - 2208988800ULL);
            std::tm tm{};
#ifdef _WIN32
            gmtime_s(&tm, &t);
#else
            gmtime_r(&t, &tm);
#endif
            char buf[32];
            std::strftime(buf, sizeof(buf), "%d.%m.%Y %H:%M:%S", &tm);
            return std::string(buf);
        };
        const auto diff = [](uint64_t a, uint64_t b) {
            if (a == LINK_TIME_UNKNOWN || b == LINK_TIME_UNKNOWN) {
                return std::string("?");
            }
            char buf[64];
            const int64_t d = static_cast<int64_t>(a - b);
            std::snprintf(buf, sizeof(buf), "%lld ms (%.2f h)", static_cast<long long>(d),
                          static_cast<double>(d) / 3'600'000.0);
            return std::string(buf);
        };
        Log("clock: shown " + as_text(v[0]) + " | RTC registers " + as_text(v[2]) +
            " | RTC as the system reads it " + as_text(v[3]));
        Log("clock: shown - RTC registers = " + diff(v[0], v[2]) +
            ", shown - system RTC = " + diff(v[0], v[3]) +
            ", system RTC - RTC registers = " + diff(v[3], v[2]));
        Log("clock: block 0x30001 = " +
            (v[1] == LINK_TIME_UNKNOWN ? std::string("?")
                                       : std::to_string(static_cast<int64_t>(v[1]))) +
            ", block 0x30002 = " +
            (v[4] == LINK_TIME_UNKNOWN ? std::string("?")
                                       : std::to_string(static_cast<int64_t>(v[4]))));
    }
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

bool Client::ArcWrite(const LinkArchiveReq& archive, const std::string& path,
                      const std::vector<uint8_t>& data, bool create, uint16_t& status) {
    size_t pos = 0;
    do {
        const size_t len = std::min<size_t>(data.size() - pos, LINK_ARCZ_MAX_CHUNK);
        LinkArcWriteReq w{};
        w.archive = archive;
        w.archive.offset = static_cast<uint32_t>(pos);
        w.archive.length = static_cast<uint32_t>(len);
        w.fileSize = static_cast<uint32_t>(data.size());
        w.pathLen = static_cast<uint16_t>(path.size());
        w.flags = (create && pos == 0) ? LINK_AW_CREATE : 0;
        std::vector<uint8_t> req(sizeof(w));
        std::memcpy(req.data(), &w, sizeof(w));
        req.insert(req.end(), path.begin(), path.end());
        req.insert(req.end(), data.begin() + static_cast<std::ptrdiff_t>(pos),
                   data.begin() + static_cast<std::ptrdiff_t>(pos + len));
        std::vector<uint8_t> out;
        if (!Request(LINK_CMD_ARCWRITE, req.data(), static_cast<uint32_t>(req.size()), status,
                     &out)) {
            return false;
        }
        NoteArcResult(status, out);
        if (status != LINK_OK) {
            return true;
        }
        pos += len;
    } while (pos < data.size());
    return true;
}

bool Client::ArcSimple(uint16_t cmd, const LinkArchiveReq& archive, const std::string& path,
                       uint16_t& status) {
    std::vector<uint8_t> req(sizeof(archive));
    std::memcpy(req.data(), &archive, sizeof(archive));
    req.insert(req.end(), path.begin(), path.end());
    std::vector<uint8_t> out;
    if (!Request(cmd, req.data(), static_cast<uint32_t>(req.size()), status, &out)) {
        return false;
    }
    NoteArcResult(status, out);
    return true;
}

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

        {
            // the bottom screen: H.264 if the 3DS has the hardware decoder (New 3DS)
            uint16_t st = 0;
            std::vector<uint8_t> caps;
            const bool have_caps = Request(LINK_CMD_VIDEOCAPS, nullptr, 0, st, &caps) &&
                                   st == LINK_OK && caps.size() >= 4;
            const bool supported =
                H264Encoder::Available() && have_caps && (caps[0] & LINK_VIDCAP_H264);
            tile_rs = have_caps && (caps[0] & LINK_VIDCAP_TILE_RS);
            tile_ts = have_caps && (caps[0] & LINK_VIDCAP_TILE_TS);
            const bool can = supported && option_h264;
            h264_supported = supported;
            h264_failed = false;
            h264_capable = can;
            h264_rs = supported && (caps[0] & LINK_VIDCAP_H264_RS);
            h264_silent = 0;
            linkGfInit();
            video_resync = true;
            Log(std::string("video: ") + (can ? "H.264 (New 3DS hardware decoder)"
                                              : H264Encoder::Available()
                                                    ? "tiles (the 3DS has no H.264 decoder, or "
                                                      "it is switched off)"
                                                    : "tiles (this build has no H.264)"));
        }
        session_dirty = true;
        library_dirty = true;
        library_unsupported = false;
        mii_sync_wanted = false;
        ReadConsoleClock();
        if (option_miis) {
            SyncMiis(false);
        }
        if (option_system_data) {
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
                if (library_dirty) {
                    SendLibrary();
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
                if (pkt.flags & LINK_INPUT_NEEDKEY) {
                    h264_key_wanted = true;
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
        u64 emu_frame = 0;
        const u64 prev_seq = frame_seq;
        const bool got_frame = VideoCore::GetLinkCapture().Take(
            frame, frame_seq, std::chrono::milliseconds(30), &emu_frame);
        if (got_frame && prev_seq != 0 && frame_seq > prev_seq + 1 && frame_seq - prev_seq < 100) {
            stats_skipped += frame_seq - prev_seq - 1; // came while the last one went out
        }
        if (got_frame) {
            h264_emu_frame = emu_frame;
        }
        if (!have_video_addr || !video_wanted || lid_closed) {
            continue;
        }
        // statistics: how many frames apart the picture changes
        if (got_frame && frame != last_frame) {
            const uint64_t gap = emu_frame - h264_prev_emu_frame;
            if (h264_prev_emu_frame != 0 && gap >= 1 && gap <= 6) {
                stats_src_gaps[std::min<uint64_t>(gap, 4) - 1]++;
            }
            h264_prev_emu_frame = emu_frame;
        }

        ProcessAcks();

        // the option switched while connected: from the next picture on
        const bool want_h264 = h264_supported && option_h264 && !h264_failed;
        if (want_h264 != h264_capable) {
            h264_capable = want_h264;
            video_resync = true;
            // send the picture on the screen at once, even if nothing moves
            if (want_h264) {
                h264_force_key = true;
            } else {
                tile_level.assign(kTiles, 255);
            }
            Log(want_h264 ? "video: switched to H.264" : "video: switched to tiles");
        }
        VideoStats(NowMs());
        if (h264_capable) {
            // the 3DS lost a frame it could not rebuild: a key frame at once. It keeps asking
            // until one arrived, so not again before the last one could have arrived (it goes out
            // in ~25 ms, the answer comes back in ~15-30 ms)
            if (h264_key_wanted.exchange(false) && NowMs() - h264_key_ms >= 70 &&
                !h264_force_key) {
                h264_force_key = true;
                stats_key_asks++;
            }
            // the colours: once the picture stood still a while, compare what the 3DS shows
            if (calib_state < 2 && calib_tries < 6 && !got_frame && !h264_force_key &&
                !h264_loss_since_key && !last_frame.empty() && NowMs() - h264_changed_ms > 1500 &&
                NowMs() - h264_key_ms > 500 && NowMs() - calib_last_ms > 3000) {
                CalibrateColors();
            }
            if (got_frame) {
                last_frame = frame;
                SendH264(frame);
            } else if (!last_frame.empty() && h264_force_key) {
                SendH264(last_frame); // a packet got lost while nothing moves: repair it
            }
            continue;
        }
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
                if (p.h264 || p.fec) {
                    // the 3DS may still rebuild it from the parity at the end of its frame:
                    // only lost once packets after the whole frame were confirmed
                    lost = !received && static_cast<int32_t>(a_seq - p.frame_end) >= 2;
                } else {
                    lost = !received && d >= 3; // newer packets arrived, this one did not
                }
            }
        }
        // no receipt in time (e.g. it was the last packet before a pause). The 3DS WiFi
        // sometimes holds its packets back a while: H.264 waits longer, since a wrong
        // "lost" costs a key frame there
        if (!received && now - p.sent_ms > (p.h264 ? 400 : p.fec ? 250 : 150)) {
            lost = true;
        }
        if (!received && !lost) {
            break;
        }
        if (lost) {
            for (const uint16_t t : p.tiles) {
                tile_level[t] = 255;
            }
            if (p.h264 && !p.parity) {
                h264_force_key = true; // the 3DS could not repair it: it waits for a key frame
            }
            if (p.h264) {
                h264_loss_since_key = true;
            }
            stat_lost++;
            stats_lost++;
        } else {
            stat_received++;
            stats_received++;
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
    // 3DS never shows a mix of old and new. The data rate follows the WiFi (less when
    // packets get lost); a frame may take up to kMaxFrameSeconds of it. Big changes
    // (fast scrolling) therefore come sharp, at a lower frame rate - frames that
    // arrive meanwhile are skipped, their changes go with the next one. Only if a
    // frame would take longer are tiles sent coarser (half, then quarter
    // resolution). Spare budget sharpens previews and refreshes a few tiles.
    constexpr int kRefreshPerFrame = 4;
    constexpr double kMinRate = 0.5e6, kMaxRate = 4.0e6; // bytes per second
    constexpr double kMaxFrameSeconds = 0.09;

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
        budget_bytes = 1.5e6; // the data rate, bytes per second
        stat_received = stat_lost = 0;
    }

    // adapt the data rate: more while the WiFi keeps up, less when packets get lost
    if (stat_received + stat_lost >= 30) {
        const double loss = static_cast<double>(stat_lost) / (stat_received + stat_lost);
        if (loss > 0.05) {
            budget_bytes *= 0.8;
        } else if (loss < 0.02) {
            budget_bytes += 50e3;
        }
        budget_bytes = std::min(kMaxRate, std::max(kMinRate, budget_bytes));
        stat_received = stat_lost = 0;
    }
    const double bytes_per_sec = budget_bytes;
    const size_t budget = static_cast<size_t>(bytes_per_sec * kMaxFrameSeconds);

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
    const auto send_t0 = std::chrono::steady_clock::now();

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
    if (tile_ts) {
        // when the picture was made: the 3DS shows them as evenly paced as they were made
        uint32_t made_ms = static_cast<uint32_t>(static_cast<double>(h264_emu_frame) *
                                                 (4481136.0 / 268111856.0) * 1000.0) &
                           0x7FFFFFFFu;
        if (made_ms == 0) {
            made_ms = 1;
        }
        // nothing changed (only sharper previews, refreshes): the time of the picture it
        // belongs to - the 3DS then does not count it as a new picture in its pacing
        if (must.empty() && tile_last_made != 0) {
            made_ms = tile_last_made;
        }
        tile_last_made = made_ms;
        cur.resize(sizeof(LinkFramePacket) + 6);
        const uint16_t head = static_cast<uint16_t>(LINK_TILE_META << LINK_TILE_TYPE_SHIFT);
        std::memcpy(&cur[sizeof(LinkFramePacket)], &head, 2);
        std::memcpy(&cur[sizeof(LinkFramePacket) + 2], &made_ms, 4);
        cur_entries = 1;
    }
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
    const size_t k = packets.size();
    // Reed-Solomon parity (see LinkProto.h): about half extra, at least 2, at most 15
    const bool busy = NowMs() - busy_ms < 1000;
    const size_t groups =
        !tile_rs || k > LINK_TILE_MAX_PACKETS
            ? 0
            : std::min<size_t>(LINK_TILE_MAX_PARITY,
                               std::max<size_t>(2, busy ? k : (k + 1) / 2));
    const uint32_t first_seq = video_seq;
    for (size_t i = 0; i < k; i++) {
        std::vector<uint8_t>& pk = packets[i];
        LinkFramePacket hdr;
        std::memcpy(&hdr, pk.data(), sizeof(hdr));
        hdr.frameId = frame_id;
        hdr.seq = video_seq++;
        hdr.baseSeq = base_seq;
        hdr.flags = static_cast<uint16_t>((i << LINK_FRAME_INDEX_SHIFT) |
                                          (i + 1 == k ? LINK_FRAME_LAST : 0));
        std::memcpy(pk.data(), &hdr, sizeof(hdr));
    }
    std::vector<uint8_t> parity(groups * LINK_FRAME_MAX_BYTES, 0);
    for (size_t j = 0; j < groups; j++) {
        uint8_t* par = &parity[j * LINK_FRAME_MAX_BYTES];
        for (size_t i = 0; i < k; i++) {
            const uint8_t c = linkCauchy(static_cast<int>(j), static_cast<int>(i),
                                         static_cast<int>(groups));
            uint8_t tab[256];
            for (int x = 0; x < 256; x++) {
                tab[x] = linkGfMul(c, static_cast<uint8_t>(x));
            }
            const std::vector<uint8_t>& pk = packets[i];
            for (size_t b = 0; b < pk.size(); b++) {
                par[b] ^= tab[pk[b]];
            }
        }
    }
    const auto t0 = std::chrono::steady_clock::now();
    size_t sent_bytes = 0;
    std::vector<uint8_t> ppk;
    for (size_t i = 0; i < k + groups; i++) {
        const bool is_parity = i >= k;
        const std::vector<uint8_t>* out = &packets[std::min(i, k - 1)];
        if (is_parity) {
            LinkFramePacket hdr = {};
            hdr.magic = LINK_TILEPAR_MAGIC;
            hdr.frameId = frame_id;
            hdr.seq = first_seq;
            hdr.baseSeq = static_cast<uint32_t>((k << 8) | (groups << 4) | (i - k));
            ppk.resize(sizeof(hdr) + LINK_FRAME_MAX_BYTES);
            std::memcpy(ppk.data(), &hdr, sizeof(hdr));
            std::memcpy(ppk.data() + sizeof(hdr), &parity[(i - k) * LINK_FRAME_MAX_BYTES],
                        LINK_FRAME_MAX_BYTES);
            out = &ppk;
        }
        {
            // own lock: requests must not hold up the picture
            std::scoped_lock lock{udp_mutex};
            if (udp_sock == -1) {
                return;
            }
            sendto(static_cast<sock_t>(udp_sock), reinterpret_cast<const char*>(out->data()),
                   static_cast<int>(out->size()), 0, reinterpret_cast<sockaddr*>(&to), sizeof(to));
        }
        if (!is_parity) {
            SentPacket sp;
            sp.seq = first_seq + static_cast<uint32_t>(i);
            sp.sent_ms = NowMs();
            sp.tiles = std::move(packet_tiles[i]);
            sp.fec = groups > 0;
            sp.frame_end = first_seq + static_cast<uint32_t>(k - 1);
            in_flight.push_back(std::move(sp));
        }
        sent_bytes += out->size();

        // pacing, so the 3DS WiFi does not get the whole frame as one burst
        const auto due = t0 + std::chrono::microseconds(
                                  static_cast<long long>(sent_bytes * 1e6 / bytes_per_sec));
        while (std::chrono::steady_clock::now() < due && !stop_requested) {
            std::this_thread::yield();
        }
    }
    const double took_ms =
        std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - send_t0)
            .count();
    stats_send_ms += took_ms;
    stats_send_max_ms = std::max(stats_send_max_ms, took_ms);
    stats_frames++;
    stats_bytes += sent_bytes;
}

// every 10 s a line in the log: how the bottom screen goes
void Client::VideoStats(int64_t now) {
    if (stats_since == 0) {
        stats_since = now;
        return;
    }
    const int64_t span = now - stats_since;
    if (span < 10000) {
        return;
    }
    if (stats_frames > 0) {
        const double secs = span / 1000.0;
        const double loss = stats_received + stats_lost
                                ? 100.0 * stats_lost / (stats_received + stats_lost)
                                : 0.0;
        Log(fmt::format("video: {} {:.1f} frames/s, {:.1f} KB per frame, {:.0f} KB/s, {:.1f} % lost{}",
                        h264_capable ? "H.264" : "tiles", stats_frames / secs,
                        stats_bytes / 1024.0 / stats_frames, stats_bytes / 1024.0 / secs, loss,
                        h264_capable ? fmt::format(", {} key frames ({} asked for by the 3DS), target {} kbit/s",
                                                   stats_keys, stats_key_asks, h264_bitrate / 1000)
                                     : fmt::format(", rate {:.0f} KB/s", budget_bytes / 1024)));
        if (!h264_capable) {
            uint16_t st = 0;
            std::vector<uint8_t> out;
            if (TryRequest(LINK_CMD_VIDEOSTATS, nullptr, 0, st, &out) == 1 && st == LINK_OK &&
                out.size() >= 80) {
                uint32_t v[21] = {};
                std::memcpy(v, out.data(), std::min(out.size(), sizeof(v)));
                Log(fmt::format("video: 3DS shown {} (by the safety net {}), rebuilt packets {}, "
                                "shown late {}, uneven {}, waited {} ms on average, stood 1/2/3/4+ "
                                "refreshes {}/{}/{}/{}, 3DS loop too slow {}, made 1/2/3/4-6 "
                                "frames apart in Azahar {}/{}/{}/{}, Azahar pictures skipped {}, "
                                "sending took {:.1f} ms on average ({:.1f} at most)",
                                v[18], v[19], v[20], v[10], v[11], v[12], v[14], v[15], v[16],
                                v[17], v[13], stats_src_gaps[0], stats_src_gaps[1],
                                stats_src_gaps[2], stats_src_gaps[3], stats_skipped,
                                stats_frames ? stats_send_ms / stats_frames : 0.0,
                                stats_send_max_ms));
            }
        }
        if (h264_capable) {
            // what the 3DS saw (never waits for a busy connection)
            uint16_t st = 0;
            std::vector<uint8_t> out;
            if (TryRequest(LINK_CMD_VIDEOSTATS, nullptr, 0, st, &out) == 1 && st == LINK_OK &&
                out.size() >= 16) {
                uint32_t v[4];
                std::memcpy(v, out.data(), sizeof(v));
                std::string extra;
                if (out.size() >= 24) {
                    uint32_t w[2];
                    std::memcpy(w, out.data() + 16, sizeof(w));
                    extra = fmt::format(", decoder behind {}, socket buffer {} KB", w[0],
                                        w[1] / 1024);
                }
                if (out.size() >= 36) {
                    uint32_t f[3];
                    std::memcpy(f, out.data() + 24, sizeof(f));
                    extra += f[0] ? fmt::format(", freezes {} (together {} ms, longest {} ms)",
                                                f[0], f[1], f[2])
                                  : std::string(", no freezes");
                }
                if (out.size() >= 52) {
                    uint32_t g[4];
                    std::memcpy(g, out.data() + 36, sizeof(g));
                    extra += fmt::format(", shown despite a loss {}, shown late {}, uneven {}, "
                                         "waited {} ms on average",
                                         g[0], g[1], g[2], g[3]);
                }
                if (out.size() >= 56) {
                    uint32_t slow;
                    std::memcpy(&slow, out.data() + 52, sizeof(slow));
                    extra += fmt::format(", 3DS loop too slow {}", slow);
                }
                if (out.size() >= 72) {
                    uint32_t q[4];
                    std::memcpy(q, out.data() + 56, sizeof(q));
                    extra += fmt::format(", stood 1/2/3/4+ refreshes on the 3DS {}/{}/{}/{}", q[0],
                                         q[1], q[2], q[3]);
                }
                extra += fmt::format(", made 1/2/3/4-6 frames apart in Azahar {}/{}/{}/{}",
                                     stats_src_gaps[0], stats_src_gaps[1], stats_src_gaps[2],
                                     stats_src_gaps[3]);
                Log(fmt::format("video: 3DS shown {}, dropped (waiting for a key frame) {}, "
                                "rebuilt packets {}, decoder errors {}{}",
                                v[0], v[1], v[2], v[3], extra));
                // pictures went out, but the 3DS showed none (e.g. an app that does not
                // fit): twice in a row, and the picture comes as tiles again
                if (v[0] == 0 && stats_frames >= 5) {
                    if (++h264_silent >= 2) {
                        Log("video: the 3DS shows no H.264 picture - tiles instead");
                        h264_failed = true;
                        h264_capable = false;
                        video_resync = true;
                    }
                } else {
                    h264_silent = 0;
                }
            }
        }
    }
    stats_since = now;
    std::fill(std::begin(stats_src_gaps), std::end(stats_src_gaps), 0u);
    stats_send_ms = stats_send_max_ms = 0;
    stats_skipped = 0;
    stats_frames = stats_bytes = stats_keys = stats_key_asks = stats_received = stats_lost = 0;
}

namespace {
// the picture turned for the 3DS framebuffer (coded column c, row r = screen x r,
// y 239 - c) as I420. `m` turns R, G, B (0..255) into Y, U, V: row i = coefficients for
// R, G, B and the constant (BT.601 TV range, see color_matrix)
void ToRotatedI420(const std::vector<uint16_t>& src, std::vector<uint8_t>& yuv,
                   const float (&m)[3][4]) {
    constexpr int W = LINK_H264_WIDTH, H = LINK_H264_HEIGHT;
    yuv.resize(W * H * 3 / 2);
    uint8_t* Y = yuv.data();
    uint8_t* U = Y + W * H;
    uint8_t* V = U + (W / 2) * (H / 2);
    // fixed point (16 bits after the point), +0.5 for rounding
    int k[3][4];
    for (int i = 0; i < 3; i++) {
        for (int j = 0; j < 3; j++) {
            k[i][j] = static_cast<int>(m[i][j] * 65536.0f + (m[i][j] < 0 ? -0.5f : 0.5f));
        }
        k[i][3] = static_cast<int>(m[i][3] * 65536.0f) + 32768;
    }
    const auto clamp8 = [](int v) {
        return static_cast<uint8_t>(v < 0 ? 0 : v > 255 ? 255 : v);
    };
    const auto rgb = [&](int r, int c, int& R, int& G, int& B) {
        const uint16_t p = src[(239 - c) * kWidth + r];
        R = ((p >> 11) & 31) * 255 / 31;
        G = ((p >> 5) & 63) * 255 / 63;
        B = (p & 31) * 255 / 31;
    };
    for (int r = 0; r < H; r += 2) {
        for (int c = 0; c < W; c += 2) {
            int sr = 0, sg = 0, sb = 0;
            for (int dy = 0; dy < 2; dy++) {
                for (int dx = 0; dx < 2; dx++) {
                    int R, G, B;
                    rgb(r + dy, c + dx, R, G, B);
                    Y[(r + dy) * W + c + dx] =
                        clamp8((k[0][0] * R + k[0][1] * G + k[0][2] * B + k[0][3]) >> 16);
                    sr += R;
                    sg += G;
                    sb += B;
                }
            }
            // the average of the four (x4, so /4 in the shift)
            U[(r / 2) * (W / 2) + c / 2] =
                clamp8(((k[1][0] * sr + k[1][1] * sg + k[1][2] * sb) / 4 + k[1][3]) >> 16);
            V[(r / 2) * (W / 2) + c / 2] =
                clamp8(((k[2][0] * sr + k[2][1] * sg + k[2][2] * sb) / 4 + k[2][3]) >> 16);
        }
    }
}

// solves a 4x4 system (Gauss-Jordan); false if (nearly) singular
bool Solve4(double a[4][5]) {
    for (int c = 0; c < 4; c++) {
        int piv = c;
        for (int r = c + 1; r < 4; r++) {
            if (std::abs(a[r][c]) > std::abs(a[piv][c])) {
                piv = r;
            }
        }
        if (std::abs(a[piv][c]) < 1e-9) {
            return false;
        }
        for (int j = 0; j < 5; j++) {
            std::swap(a[c][j], a[piv][j]);
        }
        for (int r = 0; r < 4; r++) {
            if (r == c) {
                continue;
            }
            const double f = a[r][c] / a[c][c];
            for (int j = c; j < 5; j++) {
                a[r][j] -= f * a[c][j];
            }
        }
    }
    for (int r = 0; r < 4; r++) {
        a[r][4] /= a[r][r];
    }
    return true;
}
} // namespace

// New 3DS: the bottom screen as H.264. Unchanged pictures are not sent; a lost
// packet makes the next frame a key frame (the 3DS shows nothing broken meanwhile).
void Client::SendH264(const std::vector<uint16_t>& frame) {
    // spread out a bit (the 3DS WiFi does not like bursts), but quick enough that a frame
    // with its parity is there within ~15 ms
    constexpr double kPaceBytesPerSec = 2.0e6;
    if (frame.size() != static_cast<size_t>(kWidth * kHeight)) {
        return;
    }
    if (video_resync.exchange(false) || !h264.IsOpen()) {
        if (!h264.IsOpen() &&
            !h264.Open(LINK_H264_WIDTH, LINK_H264_HEIGHT, 60, h264_bitrate)) {
            Log("video: the H.264 encoder did not start - tiles instead");
            h264_failed = true;
            h264_capable = false;
            video_resync = true;
            return;
        }
        in_flight.clear();
        {
            std::scoped_lock lock{video_mutex};
            ack_valid = ack_new = false;
        }
        if (video_seq == 0) {
            video_seq = static_cast<uint32_t>(NowMs() * 4);
        }
        h264_last.clear();
        h264_force_key = true;
        h264_second_key = true; // the 3DS decoder needs two (see below)
        stat_received = stat_lost = 0;
    }

    // the data rate follows the WiFi
    if (stat_received + stat_lost >= 60) {
        const double loss = static_cast<double>(stat_lost) / (stat_received + stat_lost);
        // losses here are mostly the WiFi, not too much data (they come at 20 KB/s as
        // well): only heavy loss lowers the rate
        int rate = h264_bitrate;
        if (loss > 0.08) {
            rate = static_cast<int>(rate * 0.8);
        } else if (loss < 0.04) {
            rate += 250'000;
        }
        rate = std::clamp(rate, 2'000'000, 8'000'000);
        if (rate != h264_bitrate) {
            h264_bitrate = rate;
            h264.SetBitrate(rate);
        }
        stat_received = stat_lost = 0;
    }

    bool force = h264_force_key.exchange(false);
    const int64_t now_ms = NowMs();
    // the emulated time of the picture (the 3DS refreshes 59.83 times a second): even
    // when Azahar finishes or reads back a frame a bit earlier or later
    const int64_t frame_ms = static_cast<int64_t>(static_cast<double>(h264_emu_frame) *
                                                  (4481136.0 / 268111856.0) * 1000.0);
    if (!force && frame == h264_last) {
        // nothing new on the screen. Once it stood still for a quarter second, one key frame:
        // whatever got lost on the way, the 3DS then shows exactly this picture
        // (only if something got lost since the last key frame - else the 3DS has it)
        if (h264_settled || !h264_loss_since_key || now_ms - h264_changed_ms < 250) {
            return;
        }
        force = true;
    }
    if (frame != h264_last) {
        h264_changed_ms = now_ms;
        h264_settled = false;
    }
    if (force) {
        h264_settled = true;
    }
    h264_last = frame;
    ToRotatedI420(frame, h264_yuv, color_matrix);
    bool key = false;
    if (!h264.Encode(h264_yuv.data(), force, NowMs(), h264_out, key) || h264_out.empty()) {
        if (force) {
            h264_force_key = true; // try again with the next picture
        }
        return;
    }

    sockaddr_in to;
    std::memcpy(&to, video_addr, sizeof(to));
    const uint32_t frame_id = ++video_frame_id;
    const size_t parts = (h264_out.size() + LINK_H264_PART - 1) / LINK_H264_PART;
    if (parts > 160) {
        h264_force_key = true; // too big for the 3DS buffer: should never happen
        return;
    }
    const auto t0 = std::chrono::steady_clock::now();
    size_t sent_bytes = 0;
    std::vector<uint8_t> pk;
    // repair packets: the XOR of every 1st/2nd data packet (see LinkProto.h)
    // Reed-Solomon parity: any `groups` lost packets of the frame can be rebuilt on the
    // 3DS. About half extra (small frames: as many as data packets): the losses come in
    // bursts, and the WiFi has room (the log showed ~1 % lost at 440 KB/s)
    // Key frames (big, and the 3DS stands still until one arrives whole) get up to 15
    // While a save goes over the WiFi as well (the log showed most losses then), as many
    // as there are data packets (up to 15)
    const bool busy = NowMs() - busy_ms < 1000;
    const size_t max_groups = key || busy ? LINK_H264_MAX_PARITY : 12;
    const size_t groups = !h264_rs ? 0
                          : parts <= 2 || busy
                              ? std::min<size_t>(max_groups, std::max<size_t>(2, parts))
                              : std::min<size_t>(max_groups, std::max<size_t>(2, (parts + 1) / 2));
    // when the picture was made: the 3DS shows them as evenly paced as they were made
    uint32_t made_ms = static_cast<uint32_t>(frame_ms) & 0x7FFFFFFFu;
    if (made_ms == 0) {
        made_ms = 1;
    }
    h264_last_made = made_ms;
    const size_t last_len = h264_out.size() - (parts - 1) * LINK_H264_PART;
    std::vector<uint8_t> parity(groups * LINK_H264_PART, 0);
    for (size_t j = 0; j < groups; j++) {
        uint8_t* par = &parity[j * LINK_H264_PART];
        for (size_t i = 0; i < parts; i++) {
            const uint8_t c = linkCauchy(static_cast<int>(j), static_cast<int>(i),
                                         static_cast<int>(groups));
            uint8_t tab[256];
            for (int x = 0; x < 256; x++) {
                tab[x] = linkGfMul(c, static_cast<uint8_t>(x));
            }
            const size_t off = i * LINK_H264_PART;
            const size_t len = std::min<size_t>(LINK_H264_PART, h264_out.size() - off);
            for (size_t k = 0; k < len; k++) {
                par[k] ^= tab[h264_out[off + k]];
            }
        }
    }
    for (size_t i = 0; i < parts + groups; i++) {
        const bool is_parity = i >= parts;
        const size_t off = i * LINK_H264_PART;
        const size_t len =
            is_parity ? LINK_H264_PART : std::min<size_t>(LINK_H264_PART, h264_out.size() - off);
        LinkFramePacket hdr = {};
        hdr.magic = LINK_H264_MAGIC;
        hdr.frameId = frame_id;
        hdr.seq = video_seq++;
        hdr.baseSeq = is_parity ? static_cast<uint32_t>(LINK_H264_PARITY | (parts << 8) |
                                                         (groups << 4) | (i - parts))
                                : made_ms;
        hdr.entryCount = static_cast<uint16_t>(is_parity ? last_len : len);
        hdr.flags = static_cast<uint16_t>((i << LINK_FRAME_INDEX_SHIFT) |
                                          (i + 1 == parts ? LINK_FRAME_LAST : 0) |
                                          (key ? LINK_FRAME_KEY : 0));
        pk.resize(sizeof(hdr) + len);
        std::memcpy(pk.data(), &hdr, sizeof(hdr));
        std::memcpy(pk.data() + sizeof(hdr),
                    is_parity ? &parity[(i - parts) * LINK_H264_PART] : &h264_out[off], len);
        {
            std::scoped_lock lock{udp_mutex};
            if (udp_sock == -1) {
                return;
            }
            sendto(static_cast<sock_t>(udp_sock), reinterpret_cast<const char*>(pk.data()),
                   static_cast<int>(pk.size()), 0, reinterpret_cast<sockaddr*>(&to), sizeof(to));
        }
        SentPacket sp;
        sp.seq = hdr.seq;
        sp.sent_ms = NowMs();
        sp.h264 = true;
        sp.parity = is_parity;
        sp.frame_end = hdr.seq + static_cast<uint32_t>(parts + groups - 1 - i);
        in_flight.push_back(std::move(sp));
        sent_bytes += pk.size();
        const auto due = t0 + std::chrono::microseconds(
                                  static_cast<long long>(sent_bytes * 1e6 / kPaceBytesPerSec));
        while (std::chrono::steady_clock::now() < due && !stop_requested) {
            std::this_thread::yield();
        }
    }
    stats_frames++;
    stats_bytes += sent_bytes;
    if (key) {
        stats_keys++;
        h264_key_ms = NowMs();
        h264_loss_since_key = false;
        // the hardware decoder of the 3DS shows everything grey (and later frames broken)
        // if its first key frame is all it got: right after the first one, a second
        // (Moonlight-N3DS does the same)
        if (h264_second_key) {
            h264_second_key = false;
            h264_force_key = true;
        }
    }
}

// The hardware decoder of the 3DS turns Y, U, V into R, G, B its own way (the colours
// came out paler than in tiles mode). Compare the picture it shows with the one sent: a fit
// over plain areas gives its conversion, and the encoder uses the inverse from then on.
void Client::CalibrateColors() {
    constexpr int W = LINK_H264_WIDTH, H = LINK_H264_HEIGHT;
    calib_last_ms = NowMs();
    uint16_t st = 0;
    std::vector<uint8_t> out;
    const int res = TryRequest(LINK_CMD_VIDEOPROBE, nullptr, 0, st, &out);
    if (res != 1) {
        return; // the connection is busy: later
    }
    calib_tries++;
    if (st == LINK_ERR_BADCMD) {
        calib_state = 2; // an older 3DS app
        return;
    }
    if (st != LINK_OK || out.size() < 4 + static_cast<size_t>(W * H * 2) ||
        h264_last.size() != static_cast<size_t>(kWidth * kHeight) ||
        h264_yuv.size() < static_cast<size_t>(W * H * 3 / 2)) {
        return;
    }
    uint32_t ts;
    std::memcpy(&ts, out.data(), 4);
    if (ts != h264_last_made) {
        Log("video: colours - the 3DS still shows an older picture, later");
        return;
    }
    std::vector<uint16_t> dec(W * H);
    std::memcpy(dec.data(), out.data() + 4, dec.size() * 2);
    {
        // for looking at it later: "LCP1", the picture sent (320x240 RGB565, rows top to
        // bottom), the I420 made of it (240x320), the picture the 3DS shows (240x320
        // RGB565), the colour matrix used
        const std::string path = FileUtil::GetUserPath(FileUtil::UserPath::UserDir) +
                                 "3ds-link/colour-probe.bin";
        std::string blob = "LCP1";
        blob.append(reinterpret_cast<const char*>(h264_last.data()), h264_last.size() * 2);
        blob.append(reinterpret_cast<const char*>(h264_yuv.data()), W * H * 3 / 2);
        blob.append(reinterpret_cast<const char*>(dec.data()), dec.size() * 2);
        blob.append(reinterpret_cast<const char*>(color_matrix), sizeof(color_matrix));
        FileUtil::WriteStringToFile(false, path, blob); // binary
    }
    const uint8_t* Yp = h264_yuv.data();
    const uint8_t* Up = Yp + W * H;
    const uint8_t* Vp = Up + (W / 2) * (H / 2);
    const auto src = [&](int r, int c) { return h264_last[(239 - c) * kWidth + r]; };
    const auto expand = [](uint16_t p, int ch) {
        return ch == 0 ? ((p >> 11) & 31) * 255 / 31
                       : ch == 1 ? ((p >> 5) & 63) * 255 / 63 : (p & 31) * 255 / 31;
    };

    double xtx[4][4] = {}, xty[3][4] = {}, yty[3] = {};
    double su = 0, suu = 0, sv = 0, svv = 0, err[3] = {};
    long n = 0;
    for (int r = 2; r + 3 < H; r += 2) {
        for (int c = 2; c + 3 < W; c += 2) {
            // where the colour (U, V) hardly changes around this 2x2 block (brightness may):
            // the decoder's smoothing of the half-resolution colour does not matter there
            const int bi = (r / 2) * (W / 2) + c / 2;
            bool flat = true;
            for (const int nb : {bi - 1, bi + 1, bi - W / 2, bi + W / 2}) {
                if (std::abs(Up[nb] - Up[bi]) > 2 || std::abs(Vp[nb] - Vp[bi]) > 2) {
                    flat = false;
                }
            }
            if (!flat) {
                continue;
            }
            const double u = Up[(r / 2) * (W / 2) + c / 2], v = Vp[(r / 2) * (W / 2) + c / 2];
            for (int dy = 0; dy < 2; dy++) {
                for (int dx = 0; dx < 2; dx++) {
                    const uint16_t d = dec[(r + dy) * W + c + dx];
                    const int rgb[3] = {expand(d, 0), expand(d, 1), expand(d, 2)};
                    if (rgb[0] == 0 || rgb[0] == 255 || rgb[1] == 0 || rgb[1] == 255 ||
                        rgb[2] == 0 || rgb[2] == 255) {
                        continue; // clipped: says nothing about the conversion
                    }
                    const double x[4] = {static_cast<double>(Yp[(r + dy) * W + c + dx]), u, v, 1.0};
                    for (int i = 0; i < 4; i++) {
                        for (int j = 0; j < 4; j++) {
                            xtx[i][j] += x[i] * x[j];
                        }
                    }
                    for (int ch = 0; ch < 3; ch++) {
                        for (int i = 0; i < 4; i++) {
                            xty[ch][i] += x[i] * rgb[ch];
                        }
                        yty[ch] += static_cast<double>(rgb[ch]) * rgb[ch];
                        err[ch] += std::abs(rgb[ch] - expand(src(r + dy, c + dx), ch));
                    }
                    su += u;
                    suu += u * u;
                    sv += v;
                    svv += v * v;
                    n++;
                }
            }
        }
    }
    if (n < 2000) {
        Log(fmt::format("video: colours - too few areas of even colour on the screen ({}), later", n));
        return;
    }
    Log(fmt::format("video: colours - average difference shown vs sent: R {:.1f}, G {:.1f}, "
                    "B {:.1f} ({} pixels)",
                    err[0] / n, err[1] / n, err[2] / n, n));
    if (calib_state == 1) {
        calib_state = 2; // checked after the correction: done
        return;
    }
    const double sdu = std::sqrt(std::max(0.0, suu / n - (su / n) * (su / n)));
    const double sdv = std::sqrt(std::max(0.0, svv / n - (sv / n) * (sv / n)));
    if (sdu < 6 || sdv < 6) {
        Log("video: colours - too little colour on the screen to measure, later");
        return;
    }
    double coef[3][4];
    double rms[3];
    for (int ch = 0; ch < 3; ch++) {
        double a[4][5];
        for (int i = 0; i < 4; i++) {
            for (int j = 0; j < 4; j++) {
                a[i][j] = xtx[i][j];
            }
            a[i][4] = xty[ch][i];
        }
        if (!Solve4(a)) {
            Log("video: colours - could not measure, later");
            return;
        }
        double sse = yty[ch];
        for (int i = 0; i < 4; i++) {
            coef[ch][i] = a[i][4];
            sse -= coef[ch][i] * xty[ch][i];
        }
        rms[ch] = std::sqrt(std::max(0.0, sse / n));
    }
    // as R = a*Y + b*(U-128) + c*(V-128) + d (the standard: R = Y + 1.402 (V-128),
    // G = Y - 0.344 (U-128) - 0.714 (V-128), B = Y + 1.772 (U-128))
    const char* names = "RGB";
    for (int ch = 0; ch < 3; ch++) {
        Log(fmt::format("video: colours - the 3DS makes {} = {:.3f} Y {:+.3f} (U-128) {:+.3f} "
                        "(V-128) {:+.1f}, off by {:.1f} on average",
                        names[ch], coef[ch][0], coef[ch][1], coef[ch][2],
                        coef[ch][3] + 128 * (coef[ch][1] + coef[ch][2]), rms[ch]));
    }
    if (rms[0] > 10 || rms[1] > 10 || rms[2] > 10) {
        Log("video: colours - the measurement is too rough, later");
        return;
    }
    // the inverse: Y, U, V = L^-1 (R, G, B - d)
    double L[3][3], inv[3][3];
    for (int i = 0; i < 3; i++) {
        for (int j = 0; j < 3; j++) {
            L[i][j] = coef[i][j];
        }
    }
    const double det = L[0][0] * (L[1][1] * L[2][2] - L[1][2] * L[2][1]) -
                       L[0][1] * (L[1][0] * L[2][2] - L[1][2] * L[2][0]) +
                       L[0][2] * (L[1][0] * L[2][1] - L[1][1] * L[2][0]);
    if (std::abs(det) < 0.05) {
        Log("video: colours - the measurement makes no sense, kept as it is");
        calib_state = 2;
        return;
    }
    for (int i = 0; i < 3; i++) {
        for (int j = 0; j < 3; j++) {
            const int i1 = (j + 1) % 3, i2 = (j + 2) % 3, j1 = (i + 1) % 3, j2 = (i + 2) % 3;
            inv[i][j] = (L[i1][j1] * L[i2][j2] - L[i1][j2] * L[i2][j1]) / det;
        }
    }
    float m[3][4];
    for (int i = 0; i < 3; i++) {
        double off = 0;
        for (int j = 0; j < 3; j++) {
            m[i][j] = static_cast<float>(inv[i][j]);
            off -= inv[i][j] * coef[j][3];
            if (std::abs(inv[i][j]) > 4.0) {
                Log("video: colours - the measurement makes no sense, kept as it is");
                calib_state = 2;
                return;
            }
        }
        m[i][3] = static_cast<float>(off);
    }
    std::memcpy(color_matrix, m, sizeof(m));
    Log(fmt::format("video: colours corrected - encoder Y = {:.3f} R {:+.3f} G {:+.3f} B {:+.1f}, "
                    "U = {:.3f} R {:+.3f} G {:+.3f} B {:+.1f}, V = {:.3f} R {:+.3f} G {:+.3f} B {:+.1f}",
                    m[0][0], m[0][1], m[0][2], m[0][3], m[1][0], m[1][1], m[1][2], m[1][3],
                    m[2][0], m[2][1], m[2][2], m[2][3]));
    calib_state = 1;
    calib_tries = 0;
    h264_force_key = true; // the picture again, with the right colours
}

Client& Get() {
    static Client client;
    return client;
}

} // namespace Link
