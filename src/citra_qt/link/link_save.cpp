// Copyright Azahar-Link contributors
// Licensed under GPLv3 or any later version
// Refer to the LICENSE.txt file included.

#include <algorithm>
#include <chrono>
#include <cstring>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <map>
#include <system_error>
#include <tuple>
#include <vector>

#include <fmt/format.h>

#include "citra_qt/link/link_client.h"
#include "citra_qt/link/link_save.h"
#include "common/file_util.h"
#include "core/system_titles.h"

namespace fs = std::filesystem;

namespace Link {

namespace {

// Azahar's folders (see core/file_sys/archive_source_sd_savedata.cpp, archive_extsavedata.cpp)
constexpr const char* kId = "00000000000000000000000000000000";

fs::path U8(const std::string& s) {
    return fs::path(std::u8string(s.begin(), s.end()));
}

std::string FromPath(const fs::path& p) {
    const std::u8string u = p.u8string();
    return std::string(u.begin(), u.end());
}

// one archive, read completely from the 3DS
struct Tree {
    std::map<std::string, std::vector<uint8_t>> files; // "/dir/file" -> data
    std::vector<std::string> dirs;                      // "/dir" (also empty ones)
    bool have_info = false;
    LinkFormatInfo info{};
    uint64_t bytes = 0;
    int from_cache = 0; // files taken from the copy of last time
};

uint64_t Fnv64(const std::vector<uint8_t>& data) {
    uint64_t h = LINK_FNV64_INIT;
    for (const uint8_t b : data) {
        h = (h ^ b) * LINK_FNV64_PRIME;
    }
    return h;
}

bool ReadFile(const fs::path& path, std::vector<uint8_t>& data) {
    std::error_code ec;
    const auto size = fs::file_size(path, ec);
    if (ec) {
        return false;
    }
    data.resize(static_cast<size_t>(size));
    std::ifstream f(path, std::ios::binary);
    if (size) {
        f.read(reinterpret_cast<char*>(data.data()), static_cast<std::streamsize>(size));
    }
    return static_cast<bool>(f);
}

// 1 = read, 0 = the archive does not exist (or is not accessible), -1 = error.
// `cache`: the copy from the last time; files whose size and hash on the 3DS still match it
// are taken from there instead of over the WiFi (empty = no cache).
int ReadTree(Client& client, const LinkArchiveReq& archive, Tree& tree, std::string& error,
             const std::function<void(const std::string&)>& progress, const std::string& what,
             const fs::path& cache = {}) {
    bool use_cache = !cache.empty();
    std::vector<std::string> todo = {"/"};
    bool first = true;
    while (!todo.empty()) {
        const std::string dir = todo.back();
        todo.pop_back();
        std::vector<Client::DirEntry> entries;
        uint16_t status = 0;
        // with a cache: the 3DS hashes the whole folder in one go, so only what changed
        // has to come over the WiFi
        if (!client.ArcList(archive, dir, entries, status, use_cache)) {
            error = Text("save_conn_lost");
            return -1;
        }
        if (use_cache && status == LINK_ERR_BADCMD) {
            use_cache = false; // older app: read everything
            if (!client.ArcList(archive, dir, entries, status)) {
                error = Text("save_conn_lost");
                return -1;
            }
        }
        if (first && (status == LINK_ERR_DENIED || status == LINK_ERR_NOTFOUND)) {
            return 0;
        }
        if (status == LINK_ERR_BADCMD) {
            error = Text("save_old_app");
            return -1;
        }
        if (status != LINK_OK) {
            error = fmt::format("{} ({}: {})", Text("save_read_failed"), dir, status);
            return -1;
        }
        first = false;
        for (const auto& e : entries) {
            const std::string path = (dir == "/" ? "" : dir) + "/" + e.name;
            if (e.name.empty() || e.name == "." || e.name == ".." ||
                e.name.find_first_of("/\\") != std::string::npos) {
                error = fmt::format("{} ({})", Text("save_bad_name"), path);
                return -1;
            }
            if (e.is_dir) {
                tree.dirs.push_back(path);
                todo.push_back(path);
                continue;
            }
            progress(fmt::format("{}: {}", what, path));
            std::vector<uint8_t> data;
            bool from_cache = false;
            if (use_cache) {
                std::vector<uint8_t> cached;
                if (ReadFile(cache / U8(path.substr(1)), cached) && e.size == cached.size() &&
                    e.hash == Fnv64(cached)) {
                    data = std::move(cached);
                    from_cache = true;
                    tree.from_cache++;
                }
            }
            // the size comes from the file itself (directory listings of extdata can show
            // another one); ArcRead checks that all of it arrived
            if (!from_cache) {
                if (!client.ArcRead(archive, path, data, status)) {
                    error = Text("save_conn_lost");
                    return -1;
                }
                if (status != LINK_OK) {
                    error = fmt::format("{} ({}: {})", Text("save_read_failed"), path, status);
                    return -1;
                }
            }
            tree.bytes += data.size();
            tree.files[path] = std::move(data);
        }
    }
    uint16_t status = 0;
    LinkFormatInfo info{};
    if (client.ArcInfo(archive, info, status) && status == LINK_OK) {
        tree.info = info;
        tree.have_info = true;
    }
    return 1;
}

// the folder holds exactly the files (and folders) of the tree, with the same contents
bool SameAsFolder(const Tree& tree, const fs::path& root) {
    std::error_code ec;
    if (!fs::is_directory(root, ec)) {
        return false;
    }
    size_t files = 0, dirs = 0;
    for (auto it = fs::recursive_directory_iterator(root, ec); !ec && it != fs::end(it);
         it.increment(ec)) {
        const std::string rel = "/" + FromPath(fs::relative(it->path(), root, ec).generic_u8string());
        if (it->is_directory(ec)) {
            if (std::find(tree.dirs.begin(), tree.dirs.end(), rel) == tree.dirs.end()) {
                return false;
            }
            dirs++;
            continue;
        }
        const auto f = tree.files.find(rel);
        if (f == tree.files.end() || fs::file_size(it->path(), ec) != f->second.size()) {
            return false;
        }
        std::vector<uint8_t> data;
        if (!ReadFile(it->path(), data) || data != f->second) {
            return false;
        }
        files++;
    }
    return !ec && files == tree.files.size() && dirs == tree.dirs.size();
}

bool WriteFile(const fs::path& path, const void* data, size_t size) {
    std::error_code ec;
    fs::create_directories(path.parent_path(), ec);
    std::ofstream f(path, std::ios::binary | std::ios::trunc);
    if (!f) {
        return false;
    }
    f.write(static_cast<const char*>(data), static_cast<std::streamsize>(size));
    f.close();
    return f.good() && fs::file_size(path, ec) == size && !ec;
}

// writes the tree below `root` and reads every file back to compare
bool WriteTree(const Tree& tree, const fs::path& root) {
    std::error_code ec;
    fs::create_directories(root, ec);
    if (ec) {
        return false;
    }
    for (const auto& d : tree.dirs) {
        fs::create_directories(root / U8(d.substr(1)), ec);
        if (ec) {
            return false;
        }
    }
    for (const auto& [name, data] : tree.files) {
        const fs::path p = root / U8(name.substr(1));
        if (!WriteFile(p, data.data(), data.size())) {
            return false;
        }
        std::ifstream f(p, std::ios::binary);
        std::vector<uint8_t> back(data.size());
        if (!data.empty()) {
            f.read(reinterpret_cast<char*>(back.data()), static_cast<std::streamsize>(back.size()));
        }
        if (!f || back != data) {
            return false;
        }
    }
    return true;
}

bool CopyFolder(const fs::path& from, const fs::path& to) {
    std::error_code ec;
    fs::create_directories(to, ec);
    fs::copy(from, to, fs::copy_options::recursive | fs::copy_options::overwrite_existing, ec);
    return !ec;
}

// puts `fresh` in the place of `target`: the old folder is renamed first and only removed
// once the new one is in place (it is in the backup anyway)
bool SwapIn(const fs::path& fresh, const fs::path& target) {
    std::error_code ec;
    fs::path old_dir = target;
    old_dir += U8(".3dslink-old");
    fs::remove_all(old_dir, ec);
    const bool had_old = fs::exists(target);
    if (had_old) {
        fs::rename(target, old_dir, ec);
        if (ec) {
            return false;
        }
    }
    fs::create_directories(target.parent_path(), ec);
    fs::rename(fresh, target, ec);
    if (ec) {
        if (had_old) {
            std::error_code ec2;
            fs::rename(old_dir, target, ec2); // put the old one back
        }
        return false;
    }
    fs::remove_all(old_dir, ec);
    return true;
}

void PruneBackups(const fs::path& title_dir) {
    std::error_code ec;
    std::vector<fs::path> dirs;
    for (const auto& e : fs::directory_iterator(title_dir, ec)) {
        if (e.is_directory()) {
            dirs.push_back(e.path());
        }
    }
    std::sort(dirs.begin(), dirs.end()); // names are the time: oldest first
    // kept: the newest `backup_keep`, and the first backup of each of the last
    // `backup_days` days with a backup (names start with the date, YYYY-MM-DD)
    const size_t keep_newest = static_cast<size_t>(std::max(1, backup_keep));
    std::vector<bool> keep(dirs.size(), false);
    for (size_t i = dirs.size() > keep_newest ? dirs.size() - keep_newest : 0; i < dirs.size();
         i++) {
        keep[i] = true;
    }
    std::vector<std::pair<std::string, size_t>> first_of_day; // newest day first
    for (size_t i = dirs.size(); i-- > 0;) {
        const std::string day = dirs[i].filename().string().substr(0, 10);
        if (!first_of_day.empty() && first_of_day.back().first == day) {
            first_of_day.back().second = i; // an earlier one of the same day
        } else {
            first_of_day.emplace_back(day, i);
        }
    }
    for (size_t d = 0; d < first_of_day.size() && d < static_cast<size_t>(backup_days); d++) {
        keep[first_of_day[d].second] = true;
    }
    for (size_t i = 0; i < dirs.size(); i++) {
        if (!keep[i]) {
            fs::remove_all(dirs[i], ec);
        }
    }
}

// NCCH header fields (3dbrew: NCCH)
constexpr size_t kNcchContentSize = 0x104;
constexpr size_t kNcchCryptoMethod = 0x18B;
constexpr size_t kNcchFlags7 = 0x18F; // bit 0 fixed key, bit 2 no crypto
constexpr size_t kNcchExefsOffset = 0x1A0;
constexpr size_t kNcchExefsSize = 0x1A4;
constexpr size_t kNcchExefsHashSize = 0x1A8;
constexpr size_t kNcchRomfsOffset = 0x1B0;
constexpr size_t kNcchRomfsSize = 0x1B4;
constexpr size_t kNcchRomfsHashSize = 0x1B8;

bool IsPlainNcch(const std::vector<uint8_t>& head) {
    return head.size() >= 0x200 && std::memcmp(head.data() + 0x100, "NCCH", 4) == 0 &&
           (head[kNcchFlags7] & 0x04) != 0;
}

void Put32(std::vector<uint8_t>& v, size_t at, uint32_t x) {
    std::memcpy(v.data() + at, &x, 4);
}

// An unencrypted NCCH with only a RomFS: the header of the original (ids, version...),
// 0x1000 bytes where the IVFC tree was (Azahar skips them), then the RomFS (level 3)
std::vector<uint8_t> BuildPlainNcch(const std::vector<uint8_t>& original_header,
                                    const std::vector<uint8_t>& romfs) {
    constexpr uint32_t unit = 0x200;
    // the whole RomFS image (IVFC tree in front) goes in as it is; just level 3 (the file
    // system itself, header size 0x28 first) gets 0x1000 bytes in front of it
    const bool whole_image = romfs.size() >= 4 && std::memcmp(romfs.data(), "IVFC", 4) == 0;
    const uint32_t lead = whole_image ? 0 : 0x1000;
    const uint32_t romfs_bytes =
        static_cast<uint32_t>((lead + romfs.size() + unit - 1) / unit * unit);
    std::vector<uint8_t> out(unit + romfs_bytes, 0);
    std::memcpy(out.data(), original_header.data(), unit);
    out[kNcchCryptoMethod] = 0;
    out[kNcchFlags7] = static_cast<uint8_t>((out[kNcchFlags7] & ~0x01) | 0x04);
    Put32(out, kNcchExefsOffset, 0);
    Put32(out, kNcchExefsSize, 0);
    Put32(out, kNcchExefsHashSize, 0);
    Put32(out, kNcchRomfsOffset, 1);
    Put32(out, kNcchRomfsSize, romfs_bytes / unit);
    Put32(out, kNcchRomfsHashSize, 0);
    Put32(out, kNcchContentSize, static_cast<uint32_t>(out.size() / unit));
    std::memcpy(out.data() + unit + lead, romfs.data(), romfs.size());
    return out;
}

std::string NowName() {
    const std::time_t t = std::time(nullptr);
    std::tm tm{};
#ifdef _WIN32
    localtime_s(&tm, &t);
#else
    localtime_r(&t, &tm);
#endif
    char buf[32];
    std::strftime(buf, sizeof(buf), "%Y-%m-%d_%H-%M-%S", &tm);
    return buf;
}

} // namespace

namespace {

// what the cache stands for: the media of the save and whether there is extdata
void WriteCacheInfo(const fs::path& cache, u32 media, bool has_ext) {
    const std::string text = fmt::format("media={}\nextdata={}\n", media, has_ext ? 1 : 0);
    WriteFile(cache / "info.txt", text.data(), text.size());
}

bool ReadCacheInfo(const fs::path& cache, u32& media, bool& has_ext) {
    std::vector<uint8_t> raw;
    if (!ReadFile(cache / "info.txt", raw)) {
        return false;
    }
    const std::string text(raw.begin(), raw.end());
    const auto m = text.find("media=");
    const auto e = text.find("extdata=");
    if (m == std::string::npos || e == std::string::npos) {
        return false;
    }
    media = static_cast<u32>(std::atoi(text.c_str() + m + 6));
    has_ext = text[e + 8] == '1';
    return media == LINK_MEDIA_SD || media == LINK_MEDIA_GAMECARD;
}

// a folder on the PC as a tree (all files read)
bool LoadFolder(const fs::path& root, Tree& tree) {
    std::error_code ec;
    if (!fs::is_directory(root, ec)) {
        return false;
    }
    for (auto it = fs::recursive_directory_iterator(root, ec); !ec && it != fs::end(it);
         it.increment(ec)) {
        const std::string rel =
            "/" + FromPath(fs::relative(it->path(), root, ec).generic_u8string());
        if (it->is_directory(ec)) {
            tree.dirs.push_back(rel);
            continue;
        }
        std::vector<uint8_t> data;
        if (!ReadFile(it->path(), data)) {
            return false;
        }
        tree.bytes += data.size();
        tree.files[rel] = std::move(data);
    }
    return !ec;
}

struct Listing {
    std::map<std::string, std::pair<uint64_t, uint64_t>> files; // path -> size, hash
    std::vector<std::string> dirs;
};

Listing ListingOf(const Tree& tree) {
    Listing l;
    for (const auto& [path, data] : tree.files) {
        l.files[path] = {data.size(), Fnv64(data)};
    }
    l.dirs = tree.dirs;
    std::sort(l.dirs.begin(), l.dirs.end());
    return l;
}

// what the 3DS has, with the size and hash of every file (the 3DS reads them for it)
bool List3DS(Client& client, const LinkArchiveReq& archive, Listing& out, std::string& error) {
    std::vector<std::string> todo = {"/"};
    while (!todo.empty()) {
        const std::string dir = todo.back();
        todo.pop_back();
        std::vector<Client::DirEntry> entries;
        uint16_t status = 0;
        if (!client.ArcList(archive, dir, entries, status, true)) {
            error = Text("save_conn_lost");
            return false;
        }
        if (status != LINK_OK) {
            error = fmt::format("{} ({}: {}, 0x{:08X})", Text("save_read_failed"), dir, status,
                                client.LastArcResult());
            return false;
        }
        for (const auto& e : entries) {
            const std::string path = (dir == "/" ? "" : dir) + "/" + e.name;
            if (e.is_dir) {
                out.dirs.push_back(path);
                todo.push_back(path);
            } else {
                out.files[path] = {e.size, e.hash};
            }
        }
    }
    std::sort(out.dirs.begin(), out.dirs.end());
    return true;
}

bool SameListing(const Listing& a, const Listing& b) {
    return a.files == b.files && a.dirs == b.dirs;
}

// writes `want` over what the 3DS has (`have`): new folders, changed and new files, then
// what is gone. Returns false with `error` set.
bool WriteDifferences(Client& client, const LinkArchiveReq& archive, const Tree& want,
                      const Listing& have, std::string& error,
                      const std::function<void(const std::string&)>& progress,
                      const std::string& what) {
    uint16_t status = 0;
    const auto failed = [&](const std::string& path) {
        error = fmt::format("{} ({}: {}, 0x{:08X})", Text("save_write_3ds_failed"), path, status,
                            client.LastArcResult());
        return false;
    };
    std::vector<std::string> new_dirs;
    for (const auto& d : want.dirs) {
        if (std::find(have.dirs.begin(), have.dirs.end(), d) == have.dirs.end()) {
            new_dirs.push_back(d);
        }
    }
    std::sort(new_dirs.begin(), new_dirs.end()); // parents first
    for (const auto& d : new_dirs) {
        if (!client.ArcSimple(LINK_CMD_ARCMKDIR, archive, d, status) || status != LINK_OK) {
            return failed(d);
        }
    }
    for (const auto& [path, data] : want.files) {
        const auto old = have.files.find(path);
        if (old != have.files.end() && old->second.first == data.size() &&
            old->second.second == Fnv64(data)) {
            continue; // unchanged
        }
        progress(fmt::format("{}: {}", what, path));
        const bool create = old == have.files.end() || old->second.first != data.size();
        if (!client.ArcWrite(archive, path, data, create, status) || status != LINK_OK) {
            return failed(path);
        }
    }
    for (const auto& [path, info] : have.files) {
        if (want.files.find(path) == want.files.end()) {
            if (!client.ArcSimple(LINK_CMD_ARCDELETE, archive, path, status) ||
                (status != LINK_OK && status != LINK_ERR_NOTFOUND)) {
                return failed(path);
            }
        }
    }
    std::vector<std::string> gone_dirs;
    for (const auto& d : have.dirs) {
        if (std::find(want.dirs.begin(), want.dirs.end(), d) == want.dirs.end()) {
            gone_dirs.push_back(d);
        }
    }
    std::sort(gone_dirs.rbegin(), gone_dirs.rend()); // children first
    for (const auto& d : gone_dirs) {
        LinkArchiveReq folder = archive;
        folder.offset = 1; // the folder with everything in it
        if (!client.ArcSimple(LINK_CMD_ARCDELETE, folder, d, status) ||
            (status != LINK_OK && status != LINK_ERR_NOTFOUND)) {
            return failed(d);
        }
    }
    return true;
}

} // namespace

u64 SaveFingerprint(u64 program_id, u64 extdata_id) {
    const u32 high = static_cast<u32>(program_id >> 32);
    const u32 low = static_cast<u32>(program_id & 0xFFFFFFFF);
    const u32 ext_id = extdata_id != 0 ? static_cast<u32>(extdata_id) : low >> 8;
    const std::string sdmc = FileUtil::GetUserPath(FileUtil::UserPath::SDMCDir);
    const fs::path dirs[2] = {
        U8(fmt::format("{}Nintendo 3DS/{}/{}/title/{:08x}/{:08x}/data/00000001", sdmc, kId, kId,
                       high, low)),
        U8(fmt::format("{}Nintendo 3DS/{}/{}/extdata/00000000/{:08x}/user", sdmc, kId, kId,
                       ext_id)),
    };
    u64 h = 0xcbf29ce484222325ULL;
    const auto mix = [&h](const void* data, size_t len) {
        const u8* p = static_cast<const u8*>(data);
        for (size_t i = 0; i < len; i++) {
            h = (h ^ p[i]) * 0x100000001b3ULL;
        }
    };
    for (const fs::path& dir : dirs) {
        std::error_code ec;
        for (auto it = fs::recursive_directory_iterator(dir, ec);
             !ec && it != fs::recursive_directory_iterator(); it.increment(ec)) {
            const std::string rel = it->path().lexically_relative(dir).generic_string();
            mix(rel.data(), rel.size());
            if (it->is_regular_file(ec)) {
                const u64 size = it->file_size(ec);
                const auto when = it->last_write_time(ec).time_since_epoch().count();
                mix(&size, sizeof(size));
                mix(&when, sizeof(when));
            }
        }
    }
    return h;
}

SaveExportResult ExportSaveTo3DS(u64 program_id, u64 extdata_id,
                                 const std::function<void(const std::string&)>& progress,
                                 bool make_backup) {
    SaveExportResult result;
    Client& client = Get();
    const u32 high = static_cast<u32>(program_id >> 32);
    const u32 low = static_cast<u32>(program_id & 0xFFFFFFFF);
    const u32 ext_id = extdata_id != 0 ? static_cast<u32>(extdata_id) : low >> 8;
    const std::string tid = fmt::format("{:016X}", program_id);
    const auto started = std::chrono::steady_clock::now();
    const auto seconds = [&] {
        return std::chrono::duration<double>(std::chrono::steady_clock::now() - started).count();
    };
    Log(fmt::format("write: {} back to the 3DS", tid));
    struct LogResult {
        SaveExportResult& r;
        ~LogResult() {
            Log(fmt::format("write: result {} - {}", static_cast<int>(r.outcome), r.message));
        }
    } log_result{result};

    // what the 3DS had when it was loaded (the cache), and what Azahar has now
    const fs::path cache = U8(LinkDir("Sync") + tid);
    u32 media = LINK_MEDIA_SD;
    bool has_ext = false;
    Tree base_save, base_ext;
    if (!ReadCacheInfo(cache, media, has_ext) || !LoadFolder(cache / "save", base_save) ||
        (has_ext && !LoadFolder(cache / "extdata", base_ext))) {
        result.outcome = SaveExportResult::Outcome::NoBaseline;
        result.message = Text("write_no_base");
        return result;
    }
    const std::string sdmc = FileUtil::GetUserPath(FileUtil::UserPath::SDMCDir);
    const fs::path save_dir =
        U8(fmt::format("{}Nintendo 3DS/{}/{}/title/{:08x}/{:08x}/data/00000001", sdmc, kId, kId,
                       high, low));
    const fs::path ext_dir =
        U8(fmt::format("{}Nintendo 3DS/{}/{}/extdata/00000000/{:08x}", sdmc, kId, kId, ext_id));
    Tree pc_save, pc_ext;
    if (!LoadFolder(save_dir, pc_save) || (has_ext && !LoadFolder(ext_dir / "user", pc_ext))) {
        result.message = Format(Text("save_write_failed"), FromPath(save_dir));
        return result;
    }
    const bool save_changed = !SameListing(ListingOf(pc_save), ListingOf(base_save));
    const bool ext_changed = has_ext && !SameListing(ListingOf(pc_ext), ListingOf(base_ext));
    if (!save_changed && !ext_changed) {
        result.outcome = SaveExportResult::Outcome::NothingToDo;
        result.message = Text("write_nothing");
        return result;
    }

    // --- 1. the 3DS must still have exactly what was loaded ---
    progress(Text("write_checking"));
    const LinkArchiveReq save_ar = {LINK_ARCHIVE_USER_SAVEDATA, media, low, high, 0, 0};
    const LinkArchiveReq ext_ar = {LINK_ARCHIVE_EXTDATA, LINK_MEDIA_SD, ext_id, 0, 0, 0};
    std::string error;
    Listing now_save, now_ext;
    if (!List3DS(client, save_ar, now_save, error) ||
        (has_ext && !List3DS(client, ext_ar, now_ext, error))) {
        result.message = error;
        return result;
    }
    if (!SameListing(now_save, ListingOf(base_save)) ||
        (has_ext && !SameListing(now_ext, ListingOf(base_ext)))) {
        result.outcome = SaveExportResult::Outcome::Conflict;
        result.message = Text("write_conflict");
        return result;
    }

    // --- 2. backup of the 3DS state (= the cache) and of what is written ---
    const fs::path title_backups = U8(BackupRoot() + tid);
    const fs::path backup = title_backups / U8(NowName() + "-to-3ds");
    if (make_backup) {
        progress(Text("save_backup"));
    }
    if (make_backup && (!WriteTree(base_save, backup / "3ds-save-before") ||
        (has_ext && !WriteTree(base_ext, backup / "3ds-extdata-before" / "user")) ||
        !WriteTree(pc_save, backup / "pc-save-written") ||
        (has_ext && !WriteTree(pc_ext, backup / "pc-extdata-written" / "user")))) {
        result.message = Format(Text("save_write_failed"), FromPath(backup));
        return result;
    }
    if (make_backup) {
        result.backup_dir = FromPath(backup);
    }

    // --- 3. the save: write, check every file on the 3DS, only then commit ---
    uint16_t status = 0;
    if (save_changed) {
        if (!WriteDifferences(client, save_ar, pc_save, now_save, error, progress,
                              Text("write_writing"))) {
            client.ArcSimple(LINK_CMD_ARCCOMMIT, LinkArchiveReq{save_ar.archive, save_ar.mediaType,
                                                                save_ar.lowId, save_ar.highId, 1, 0},
                             "", status); // throw the changes away
            result.message = error;
            return result;
        }
        progress(Text("write_verifying"));
        Listing check;
        if (!List3DS(client, save_ar, check, error) || !SameListing(check, ListingOf(pc_save))) {
            client.ArcSimple(LINK_CMD_ARCCOMMIT, LinkArchiveReq{save_ar.archive, save_ar.mediaType,
                                                                save_ar.lowId, save_ar.highId, 1, 0},
                             "", status);
            result.message = error.empty() ? Text("write_verify_failed") : error;
            return result;
        }
        if (!client.ArcSimple(LINK_CMD_ARCCOMMIT, save_ar, "", status) || status != LINK_OK) {
            result.message = fmt::format("{} (commit: {}, 0x{:08X})", Text("save_write_3ds_failed"),
                                         status, client.LastArcResult());
            return result;
        }
    }

    // --- 4. extdata (no commit there: written, then checked) ---
    if (ext_changed) {
        if (!WriteDifferences(client, ext_ar, pc_ext, now_ext, error, progress,
                              Text("write_writing"))) {
            result.message = error;
            return result;
        }
        Listing check;
        if (!List3DS(client, ext_ar, check, error) || !SameListing(check, ListingOf(pc_ext))) {
            result.message = error.empty() ? Text("write_verify_failed") : error;
            return result;
        }
        client.ArcSimple(LINK_CMD_ARCCOMMIT, ext_ar, "", status); // closes it
    }

    // --- 5. the cache is now what the 3DS has ---
    std::error_code ec;
    for (const auto& [tree, name, ok] : {std::tuple{&pc_save, "save", true},
                                         std::tuple{&pc_ext, "extdata", has_ext}}) {
        if (!ok) {
            continue;
        }
        const fs::path dst = cache / name;
        fs::path tmp = dst;
        tmp += U8(".new");
        fs::remove_all(tmp, ec);
        if (!WriteTree(*tree, tmp) || !SwapIn(tmp, dst)) {
            fs::remove_all(tmp, ec);
            fs::remove_all(cache, ec); // never keep a cache that may not match
            break;
        }
    }
    PruneBackups(title_backups);
    result.outcome = SaveExportResult::Outcome::Written;
    result.message = Text("write_done");
    Log(fmt::format("write: done after {:.1f} s", seconds()));
    return result;
}

int CopySystemDataFrom3DS(std::string& message) {
    Client& client = Get();
    // the data archives Azahar knows (core/system_titles.cpp), of every region
    std::vector<u64> ids;
    for (u32 region = 0; region < Core::NUM_SYSTEM_TITLE_REGIONS; region++) {
        for (const auto set : {Core::SystemTitleSet::Minimal, Core::SystemTitleSet::Old3ds,
                               Core::SystemTitleSet::New3ds}) {
            for (const u64 id : Core::GetSystemTitleIds(set, region)) {
                const u32 high = static_cast<u32>(id >> 32);
                if ((high == 0x0004009B || high == 0x000400DB) &&
                    std::find(ids.begin(), ids.end(), id) == ids.end()) {
                    ids.push_back(id);
                }
            }
        }
    }

    const std::string nand = FileUtil::GetUserPath(FileUtil::UserPath::NANDDir);
    const LinkArchiveReq ctrnand = {LINK_ARCHIVE_NAND_CTR_FS, LINK_MEDIA_NOPATH, 0, 0, 0, 0};
    int copied = 0;
    for (const u64 id : ids) {
        const u32 high = static_cast<u32>(id >> 32);
        const u32 low = static_cast<u32>(id & 0xFFFFFFFF);
        const fs::path content =
            U8(fmt::format("{}{}/title/{:08x}/{:08x}/content", nand, kId, high, low));
        // already there? A .tmd is what Azahar looks for, and its contents must be
        // unencrypted (Azahar reads no encrypted NCCH; copies of an earlier version were)
        std::error_code ec;
        bool has_tmd = false, usable = true;
        for (const auto& e : fs::directory_iterator(content, ec)) {
            if (e.path().extension() == ".tmd") {
                has_tmd = true;
            } else if (e.path().extension() == ".app") {
                std::vector<uint8_t> head(0x200);
                std::ifstream f(e.path(), std::ios::binary);
                f.read(reinterpret_cast<char*>(head.data()), 0x200);
                if (!f || !IsPlainNcch(head)) {
                    usable = false;
                }
            }
        }
        if (has_tmd && usable) {
            continue;
        }

        const std::string dir = fmt::format("/title/{:08x}/{:08x}/content", high, low);
        std::vector<Client::DirEntry> entries;
        uint16_t status = 0;
        if (!client.ArcList(ctrnand, dir, entries, status)) {
            Log("system data: connection gone");
            return copied; // connection gone
        }
        if (client.LastArcResult() != 0xC8804478) { // not on this 3DS: nothing to say
            Log(fmt::format("system data: {:016X}: {} (3DS code 0x{:08X}, {} entries)", id,
                            status, client.LastArcResult(), entries.size()));
        }
        if (status == LINK_ERR_BADCMD) {
            message = Text("sys_old_app");
            return copied;
        }
        if (status == LINK_ERR_DENIED) {
            message = fmt::format("{} (0x{:08X})", Text("sys_denied"), client.LastArcResult());
            return copied;
        }
        if (status != LINK_OK) {
            continue; // not on this 3DS (another region)
        }

        // the title's .tmd as it is; its content (encrypted on the NAND) is rebuilt
        // unencrypted from the RomFS the 3DS reads for us
        Tree tree;
        bool content_done = false;
        for (const auto& e : entries) {
            if (e.is_dir) {
                continue;
            }
            const std::string file = dir + "/" + e.name;
            std::vector<uint8_t> data;
            const bool is_app = e.name.size() > 4 && e.name.substr(e.name.size() - 4) == ".app";
            if (is_app && content_done) {
                continue; // data archives have one content; only that one can be rebuilt
            }
            if (!(is_app ? client.ArcReadPart(ctrnand, file, 0, 0x200, data, status)
                         : client.ArcRead(ctrnand, file, data, status))) {
                return copied;
            }
            if (status != LINK_OK) {
                message = fmt::format("{} ({}: {})", Text("save_read_failed"), file, status);
                return copied;
            }
            if (is_app) {
                std::vector<uint8_t> romfs;
                if (!client.ReadSystemRomfs(id, romfs, status)) {
                    return copied;
                }
                Log(fmt::format("system data: {:016X} RomFS: {} ({} bytes, starts {:02X}{:02X}{:02X}"
                                "{:02X}, 3DS code 0x{:08X})",
                                id, status, romfs.size(), romfs.size() > 3 ? romfs[0] : 0,
                                romfs.size() > 3 ? romfs[1] : 0, romfs.size() > 3 ? romfs[2] : 0,
                                romfs.size() > 3 ? romfs[3] : 0, client.LastArcResult()));
                if (status == LINK_ERR_BADCMD) {
                    message = Text("sys_old_app");
                    return copied;
                }
                if (status != LINK_OK || data.size() < 0x200 || romfs.size() < 0x28) {
                    message = fmt::format("{} ({:016X} RomFS: {})", Text("save_read_failed"), id,
                                          status);
                    return copied;
                }
                data = BuildPlainNcch(data, romfs);
                content_done = true;
            }
            tree.files["/" + e.name] = std::move(data);
        }
        if (tree.files.empty() || !content_done) {
            continue;
        }
        fs::path fresh = content;
        fresh += U8(".3dslink-new");
        fs::remove_all(fresh, ec);
        if (!WriteTree(tree, fresh) || !SwapIn(fresh, content)) {
            fs::remove_all(fresh, ec);
            message = Format(Text("save_write_failed"), FromPath(content));
            return copied;
        }
        copied++;
        Log(fmt::format("system data: {:016X} copied ({} files)", id, tree.files.size()));
    }
    if (copied > 0 && message.empty()) {
        message = Format(Text("sys_done"), std::to_string(copied));
    }
    return copied;
}

int backup_days = 7;
int backup_keep = 3;

std::string BackupRoot() {
    return LinkDir("Backups");
}

SaveImportResult ImportSaveFrom3DS(u64 program_id, u64 extdata_id,
                                   const std::function<void(const std::string&)>& progress,
                                   const std::function<int(const std::string&)>& ask) {
    SaveImportResult result;
    Client& client = Get();
    const u32 high = static_cast<u32>(program_id >> 32);
    const u32 low = static_cast<u32>(program_id & 0xFFFFFFFF);
    const std::string tid = fmt::format("{:016X}", program_id);
    const auto started = std::chrono::steady_clock::now();
    const auto seconds = [&] {
        return std::chrono::duration<double>(std::chrono::steady_clock::now() - started).count();
    };
    Log(fmt::format("save: loading {} from the 3DS (extdata id {:X})", tid, extdata_id));
    struct LogResult {
        SaveImportResult& r;
        ~LogResult() {
            Log(fmt::format("save: result {} - {}", static_cast<int>(r.outcome), r.message));
        }
    } log_result{result};

    // --- 1. read everything from the 3DS (nothing is written there) ---
    const fs::path cache = U8(LinkDir("Sync") + tid);
    Tree save;
    std::string error;
    int found = 0;
    std::string codes;
    u32 save_media = LINK_MEDIA_SD;
    for (const u32 media : {LINK_MEDIA_SD, LINK_MEDIA_GAMECARD}) {
        const LinkArchiveReq ar = {LINK_ARCHIVE_USER_SAVEDATA, media, low, high, 0, 0};
        save = Tree{};
        save_media = media;
        found = ReadTree(client, ar, save, error, progress, Text("save_reading"),
                         cache / "save");
        if (found != 0) {
            break;
        }
        codes += fmt::format("{}{} 0x{:08X}", codes.empty() ? "" : ", ",
                             media == LINK_MEDIA_SD ? "SD" : "Card", client.LastArcResult());
    }
    if (found < 0) {
        result.message = error;
        return result;
    }
    if (found == 0) {
        // why: is the game installed at all (else another version / region is)?
        std::vector<uint64_t> sd, card;
        uint16_t st = 0;
        std::string installed = "?";
        if (client.Titles(sd, card, st) && st == LINK_OK) {
            const bool on_sd = std::find(sd.begin(), sd.end(), program_id) != sd.end();
            const bool on_card = std::find(card.begin(), card.end(), program_id) != card.end();
            installed = on_sd ? Text("yes_sd") : on_card ? Text("yes_card") : Text("no");
            // the list for checking which version is on the 3DS
            std::error_code ec;
            fs::create_directories(U8(BackupRoot()), ec);
            std::ofstream list(U8(BackupRoot() + "3ds-titles.txt"));
            for (const u64 id : sd) {
                list << fmt::format("SD   {:016X}\n", id);
            }
            for (const u64 id : card) {
                list << fmt::format("Card {:016X}\n", id);
            }
        }
        result.outcome = SaveImportResult::Outcome::NoSave;
        result.message = Format(Text("save_none"), tid) + "\n\n" +
                         Format(Text("save_diag"), installed, codes);
        return result;
    }

    // extdata: the id in the game's header; without it, the middle of the title id (as
    // save managers guess it - wrong for some games, e.g. Pokemon Rumble World uses 0x1646)
    const u32 ext_id = extdata_id != 0 ? static_cast<u32>(extdata_id) : low >> 8;
    Tree ext;
    const LinkArchiveReq ext_ar = {LINK_ARCHIVE_EXTDATA, LINK_MEDIA_SD, ext_id, 0, 0, 0};
    const int ext_found = ReadTree(client, ext_ar, ext, error, progress, Text("save_reading_ext"),
                                   cache / "extdata");
    if (ext_found < 0) {
        result.message = error;
        return result;
    }

    Log(fmt::format("save: read {} files ({} unchanged, from the cache), extdata {} files ({} "
                    "unchanged), {} KB, {:.1f} s",
                    save.files.size(), save.from_cache, ext.files.size(), ext.from_cache,
                    (save.bytes + ext.bytes) / 1024, seconds()));

    // --- 2. backups: the copy from the 3DS and Azahar's save as it is now ---
    progress(Text("save_backup"));
    const std::string sdmc = FileUtil::GetUserPath(FileUtil::UserPath::SDMCDir);
    const fs::path save_dir =
        U8(fmt::format("{}Nintendo 3DS/{}/{}/title/{:08x}/{:08x}/data/00000001", sdmc, kId, kId,
                       high, low));
    fs::path save_meta = save_dir;
    save_meta += U8(".metadata");
    const fs::path ext_dir =
        U8(fmt::format("{}Nintendo 3DS/{}/{}/extdata/00000000/{:08x}", sdmc, kId, kId, ext_id));

    // Azahar has exactly this already (the usual case when nothing was played on the 3DS
    // and nothing on the PC): nothing to back up, nothing to write
    if (SameAsFolder(save, save_dir) && fs::exists(save_meta) &&
        (ext_found == 0 || (SameAsFolder(ext, ext_dir / "user") && fs::exists(ext_dir / "metadata"))) &&
        fs::exists(cache / "save")) {
        WriteCacheInfo(cache, save_media, ext_found > 0);
        result.outcome = SaveImportResult::Outcome::Imported;
        result.message = Text("save_same");
        Log(fmt::format("save: Azahar has the same save already, {:.1f} s", seconds()));
        return result;
    }

    // --- 1b. played on the PC since the saves were last the same? The cache is what the 3DS
    // had then (after every load and every write). Then the user decides. ---
    bool keep_pc = false;
    Tree pc_now, pc_now_ext;
    if (ask && LoadFolder(save_dir, pc_now) && !pc_now.files.empty()) {
        const bool with_ext = ext_found > 0 && LoadFolder(ext_dir / "user", pc_now_ext);
        const auto same = [](const Tree& a, const Tree& b) {
            return SameListing(ListingOf(a), ListingOf(b));
        };
        u32 last_media = 0;
        bool last_has_ext = false;
        Tree last, last_ext;
        const bool have_last = ReadCacheInfo(cache, last_media, last_has_ext) &&
                               LoadFolder(cache / "save", last) &&
                               (!last_has_ext || LoadFolder(cache / "extdata", last_ext));
        const bool ext_counts = with_ext && last_has_ext;
        const bool pc_unchanged =
            have_last && same(pc_now, last) && (!ext_counts || same(pc_now_ext, last_ext));
        if (!pc_unchanged) {
            const bool ds_unchanged =
                have_last && same(save, last) && (!ext_counts || same(ext, last_ext));
            // when the PC save was last written
            std::string when = "?";
            std::error_code ec;
            fs::file_time_type newest{};
            for (auto it = fs::recursive_directory_iterator(save_dir, ec);
                 !ec && it != fs::recursive_directory_iterator(); it.increment(ec)) {
                newest = std::max(newest, it->last_write_time(ec));
            }
            if (newest != fs::file_time_type{}) {
                const auto sys = std::chrono::time_point_cast<std::chrono::system_clock::duration>(
                    newest - fs::file_time_type::clock::now() + std::chrono::system_clock::now());
                const std::time_t t = std::chrono::system_clock::to_time_t(sys);
                std::tm tm{};
#ifdef _WIN32
                localtime_s(&tm, &t);
#else
                localtime_r(&t, &tm);
#endif
                char buf[32];
                std::strftime(buf, sizeof(buf), "%d.%m.%Y %H:%M", &tm);
                when = buf;
            }
            const std::string question =
                Text(!have_last ? "ask_differ" : ds_unchanged ? "ask_newer" : "ask_both") + "\n" +
                Format(Text("ask_pcdate"), when) + "\n\n" + Text("ask_note") + "\n" +
                Text("ask_usepc") + "\n" + Text("ask_use3ds");
            Log(fmt::format("save: PC save changed since the last sync ({})",
                            !have_last ? "no record" : ds_unchanged ? "3DS unchanged" : "both"));
            const int choice = ask(question);
            if (choice < 0) {
                result.message = Text("ask_cancelled");
                return result;
            }
            keep_pc = choice == 0;
            Log(std::string("save: the user chose the ") + (keep_pc ? "PC save" : "3DS save"));
        }
    }

    const fs::path title_backups = U8(BackupRoot() + tid);
    const fs::path backup = title_backups / U8(NowName());
    std::error_code ec;
    if (!WriteTree(save, backup / "3ds-save")) {
        result.message = Format(Text("save_write_failed"), FromPath(backup));
        return result;
    }
    if (ext_found > 0 && !WriteTree(ext, backup / "3ds-extdata" / "user")) {
        result.message = Format(Text("save_write_failed"), FromPath(backup));
        return result;
    }
    if (fs::exists(save_dir) && !CopyFolder(save_dir, backup / "pc-save")) {
        result.message = Format(Text("save_write_failed"), FromPath(backup));
        return result;
    }
    if (fs::exists(save_meta)) {
        fs::copy_file(save_meta, backup / "pc-save.metadata", fs::copy_options::overwrite_existing,
                      ec);
    }
    if (ext_found > 0 && fs::exists(ext_dir) && !CopyFolder(ext_dir, backup / "pc-extdata")) {
        result.message = Format(Text("save_write_failed"), FromPath(backup));
        return result;
    }
    result.backup_dir = FromPath(backup);

    // the copy for next time (only read again what changed on the 3DS) - also the base
    // a write to the 3DS checks against
    const auto write_cache = [&] {
        for (const auto& [tree, name, ok] :
             {std::tuple{&save, "save", true}, std::tuple{&ext, "extdata", ext_found > 0}}) {
            const fs::path dst = cache / name;
            fs::path tmp = dst;
            tmp += U8(".new");
            fs::remove_all(tmp, ec);
            if (!ok || !WriteTree(*tree, tmp) || !SwapIn(tmp, dst)) {
                fs::remove_all(tmp, ec);
                fs::remove_all(dst, ec); // never keep a cache that may not match
            }
        }
        WriteCacheInfo(cache, save_media, ext_found > 0);
    };
    if (keep_pc) {
        // Azahar keeps its save; the 3DS state is the base for writing it there
        write_cache();
        PruneBackups(title_backups);
        result.outcome = SaveImportResult::Outcome::KeptPC;
        result.message = Text("save_kept_pc");
        Log(fmt::format("save: PC save kept after {:.1f} s, backup {}", seconds(),
                        result.backup_dir));
        return result;
    }

    // --- 3. into Azahar: written next to the old files first, then swapped ---
    progress(Text("save_writing"));
    fs::path save_new = save_dir;
    save_new += U8(".3dslink-new");
    fs::remove_all(save_new, ec);
    if (!WriteTree(save, save_new) || !SwapIn(save_new, save_dir)) {
        fs::remove_all(save_new, ec);
        result.message = Format(Text("save_write_failed"), FromPath(save_dir));
        return result;
    }
    // Azahar's record of how the save was formatted: kept if there is one (the game made
    // it), else the one of the 3DS, so the game does not format the save again
    if (!fs::exists(save_meta)) {
        LinkFormatInfo info = save.info;
        if (!save.have_info) {
            info = {};
            info.totalSize = 0x100000;
            info.directories = 64;
            info.files = 64;
        }
        WriteFile(save_meta, &info, sizeof(info));
    }

    if (ext_found > 0) {
        fs::path ext_new = ext_dir;
        ext_new += U8(".3dslink-new");
        fs::remove_all(ext_new, ec);
        bool ok = WriteTree(ext, ext_new / "user");
        fs::create_directories(ext_new / "boss", ec);
        // keep Azahar's metadata and icon; else take the format of the 3DS
        for (const char* keep : {"metadata", "icon"}) {
            if (ok && fs::exists(ext_dir / keep)) {
                fs::copy_file(ext_dir / keep, ext_new / keep, fs::copy_options::overwrite_existing,
                              ec);
                ok = !ec;
            }
        }
        if (ok && !fs::exists(ext_new / "metadata")) {
            LinkFormatInfo info = ext.info;
            if (!ext.have_info) {
                info = {};
                info.totalSize = 0;
                info.directories = 64;
                info.files = 64;
            }
            ok = WriteFile(ext_new / "metadata", &info, sizeof(info));
        }
        if (ok && fs::exists(ext_dir / "boss")) {
            ok = CopyFolder(ext_dir / "boss", ext_new / "boss");
        }
        if (!ok || !SwapIn(ext_new, ext_dir)) {
            fs::remove_all(ext_new, ec);
            result.message = Format(Text("save_write_failed"), FromPath(ext_dir));
            return result;
        }
    }

    write_cache();
    PruneBackups(title_backups);
    Log(fmt::format("save: done after {:.1f} s, backup {}", seconds(), result.backup_dir));
    result.outcome = SaveImportResult::Outcome::Imported;
    const size_t kb = static_cast<size_t>((save.bytes + ext.bytes + 1023) / 1024);
    result.message = Format(Text(ext_found > 0 ? "save_done_ext" : "save_done"),
                            std::to_string(kb));
    return result;
}

std::vector<SaveBackup> ListBackups(u64 program_id) {
    std::vector<SaveBackup> out;
    const fs::path root = U8(BackupRoot() + fmt::format("{:016X}", program_id));
    std::error_code ec;
    std::vector<fs::path> dirs;
    for (const auto& e : fs::directory_iterator(root, ec)) {
        if (e.is_directory()) {
            dirs.push_back(e.path());
        }
    }
    std::sort(dirs.rbegin(), dirs.rend()); // names are the time: newest first
    // in each backup, the newer state first
    struct Kind {
        const char* save;
        const char* ext;
        const char* where;
    };
    static constexpr Kind kKinds[] = {
        {"pc-save-written", "pc-extdata-written/user", "PC -> 3DS"},
        {"3ds-save-before", "3ds-extdata-before/user", "3DS"},
        {"3ds-save", "3ds-extdata/user", "3DS"},
        {"pc-save", "pc-extdata/user", "PC"},
    };
    std::vector<uint64_t> seen;
    for (const fs::path& dir : dirs) {
        const std::string name = dir.filename().string(); // YYYY-MM-DD_HH-MM-SS[-to-3ds]
        std::string when = name;
        if (name.size() >= 16) {
            when = name.substr(8, 2) + "." + name.substr(5, 2) + "." + name.substr(0, 4) + " " +
                   name.substr(11, 2) + ":" + name.substr(14, 2);
        }
        for (const Kind& k : kKinds) {
            Tree t;
            if (!LoadFolder(dir / k.save, t) || t.files.empty()) {
                continue;
            }
            uint64_t h = LINK_FNV64_INIT;
            for (const auto& [path, data] : t.files) {
                h = (h ^ Fnv64(std::vector<uint8_t>(path.begin(), path.end()))) * LINK_FNV64_PRIME;
                h = (h ^ Fnv64(data)) * LINK_FNV64_PRIME;
            }
            if (std::find(seen.begin(), seen.end(), h) != seen.end()) {
                continue; // the same save as a newer entry
            }
            seen.push_back(h);
            SaveBackup b;
            b.save_dir = FromPath(dir / k.save);
            if (fs::is_directory(dir / U8(k.ext), ec)) {
                b.ext_dir = FromPath(dir / U8(k.ext));
            }
            b.label = when + "  " + k.where;
            out.push_back(std::move(b));
        }
    }
    return out;
}

std::string RestoreBackup(u64 program_id, u64 extdata_id, const SaveBackup& backup,
                          const std::function<void(const std::string&)>& progress) {
    const u32 high = static_cast<u32>(program_id >> 32);
    const u32 low = static_cast<u32>(program_id & 0xFFFFFFFF);
    const u32 ext_id = extdata_id != 0 ? static_cast<u32>(extdata_id) : low >> 8;
    Log(fmt::format("restore: {:016X} from {}", program_id, backup.save_dir));
    Tree save, ext;
    if (!LoadFolder(U8(backup.save_dir), save) || save.files.empty() ||
        (!backup.ext_dir.empty() && !LoadFolder(U8(backup.ext_dir), ext))) {
        return Format(Text("save_write_failed"), backup.save_dir);
    }
    // 1. what the 3DS has now: into Azahar and the backups, and the base for writing
    const SaveImportResult r = ImportSaveFrom3DS(program_id, extdata_id, progress);
    if (r.outcome == SaveImportResult::Outcome::NoSave) {
        return Text("restore_no_save");
    }
    if (r.outcome != SaveImportResult::Outcome::Imported) {
        return r.message;
    }
    // 2. the backup into Azahar (written next to the old files, then swapped)
    const std::string sdmc = FileUtil::GetUserPath(FileUtil::UserPath::SDMCDir);
    const fs::path save_dir =
        U8(fmt::format("{}Nintendo 3DS/{}/{}/title/{:08x}/{:08x}/data/00000001", sdmc, kId, kId,
                       high, low));
    const fs::path ext_dir =
        U8(fmt::format("{}Nintendo 3DS/{}/{}/extdata/00000000/{:08x}", sdmc, kId, kId, ext_id));
    std::error_code ec;
    fs::path save_new = save_dir;
    save_new += U8(".3dslink-new");
    fs::remove_all(save_new, ec);
    if (!WriteTree(save, save_new) || !SwapIn(save_new, save_dir)) {
        fs::remove_all(save_new, ec);
        return Format(Text("save_write_failed"), FromPath(save_dir));
    }
    if (!backup.ext_dir.empty() && fs::is_directory(ext_dir, ec)) {
        fs::path ext_new = ext_dir;
        ext_new += U8(".3dslink-new");
        fs::remove_all(ext_new, ec);
        bool ok = WriteTree(ext, ext_new / "user");
        for (const char* keep : {"metadata", "icon"}) {
            if (ok && fs::exists(ext_dir / keep)) {
                fs::copy_file(ext_dir / keep, ext_new / keep, fs::copy_options::overwrite_existing,
                              ec);
                ok = !ec;
            }
        }
        if (ok && fs::exists(ext_dir / "boss")) {
            ok = CopyFolder(ext_dir / "boss", ext_new / "boss");
        }
        if (!ok || !SwapIn(ext_new, ext_dir)) {
            fs::remove_all(ext_new, ec);
            return Format(Text("save_write_failed"), FromPath(ext_dir));
        }
    }
    // 3. to the 3DS (checked like every write)
    const SaveExportResult w = ExportSaveTo3DS(program_id, extdata_id, progress, false);
    if (w.outcome == SaveExportResult::Outcome::Written ||
        w.outcome == SaveExportResult::Outcome::NothingToDo) {
        Log("restore: done");
        return "";
    }
    return w.message;
}

} // namespace Link
