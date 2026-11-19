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
constexpr int kKeepBackups = 20;

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
    while (dirs.size() > static_cast<size_t>(kKeepBackups)) {
        fs::remove_all(dirs.front(), ec);
        dirs.erase(dirs.begin());
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
        Log(fmt::format("system data: {:016X}: {} (3DS code 0x{:08X}, {} entries)", id, status,
                        client.LastArcResult(), entries.size()));
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

std::string BackupRoot() {
    return FileUtil::GetUserPath(FileUtil::UserPath::UserDir) + "3ds-link/backups/";
}

SaveImportResult ImportSaveFrom3DS(u64 program_id, u64 extdata_id,
                                   const std::function<void(const std::string&)>& progress) {
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
    const fs::path cache = U8(FileUtil::GetUserPath(FileUtil::UserPath::UserDir) +
                              "3ds-link/cache/" + tid);
    Tree save;
    std::string error;
    int found = 0;
    std::string codes;
    for (const u32 media : {LINK_MEDIA_SD, LINK_MEDIA_GAMECARD}) {
        const LinkArchiveReq ar = {LINK_ARCHIVE_USER_SAVEDATA, media, low, high, 0, 0};
        save = Tree{};
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

    // the copy for next time (only read again what changed on the 3DS)
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

    PruneBackups(title_backups);
    Log(fmt::format("save: done after {:.1f} s, backup {}", seconds(), result.backup_dir));
    result.outcome = SaveImportResult::Outcome::Imported;
    const size_t kb = static_cast<size_t>((save.bytes + ext.bytes + 1023) / 1024);
    result.message = Format(Text(ext_found > 0 ? "save_done_ext" : "save_done"),
                            std::to_string(kb));
    return result;
}

} // namespace Link
