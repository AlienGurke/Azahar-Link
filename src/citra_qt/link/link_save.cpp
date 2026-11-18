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
#include <vector>

#include <fmt/format.h>

#include "citra_qt/link/link_client.h"
#include "citra_qt/link/link_save.h"
#include "common/file_util.h"

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
};

// 1 = read, 0 = the archive does not exist (or is not accessible), -1 = error
int ReadTree(Client& client, const LinkArchiveReq& archive, Tree& tree, std::string& error,
             const std::function<void(const std::string&)>& progress, const std::string& what) {
    std::vector<std::string> todo = {"/"};
    bool first = true;
    while (!todo.empty()) {
        const std::string dir = todo.back();
        todo.pop_back();
        std::vector<Client::DirEntry> entries;
        uint16_t status = 0;
        if (!client.ArcList(archive, dir, entries, status)) {
            error = Text("save_conn_lost");
            return -1;
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
            if (!client.ArcRead(archive, path, data, status)) {
                error = Text("save_conn_lost");
                return -1;
            }
            if (status != LINK_OK || data.size() != e.size) {
                error = fmt::format("{} ({}: {})", Text("save_read_failed"), path, status);
                return -1;
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

std::string BackupRoot() {
    return FileUtil::GetUserPath(FileUtil::UserPath::UserDir) + "3ds-link/backups/";
}

SaveImportResult ImportSaveFrom3DS(u64 program_id,
                                   const std::function<void(const std::string&)>& progress) {
    SaveImportResult result;
    Client& client = Get();
    const u32 high = static_cast<u32>(program_id >> 32);
    const u32 low = static_cast<u32>(program_id & 0xFFFFFFFF);
    const std::string tid = fmt::format("{:016X}", program_id);

    // --- 1. read everything from the 3DS (nothing is written there) ---
    Tree save;
    std::string error;
    int found = 0;
    for (const u32 media : {LINK_MEDIA_SD, LINK_MEDIA_GAMECARD}) {
        const LinkArchiveReq ar = {LINK_ARCHIVE_USER_SAVEDATA, media, low, high, 0, 0};
        save = Tree{};
        found = ReadTree(client, ar, save, error, progress, Text("save_reading"));
        if (found != 0) {
            break;
        }
    }
    if (found < 0) {
        result.message = error;
        return result;
    }
    if (found == 0) {
        result.outcome = SaveImportResult::Outcome::NoSave;
        result.message = Format(Text("save_none"), tid);
        return result;
    }

    // extdata: its id is the middle of the title id (as save managers like Checkpoint do)
    const u32 ext_id = low >> 8;
    Tree ext;
    const LinkArchiveReq ext_ar = {LINK_ARCHIVE_EXTDATA, LINK_MEDIA_SD, ext_id, 0, 0, 0};
    const int ext_found = ReadTree(client, ext_ar, ext, error, progress, Text("save_reading_ext"));
    if (ext_found < 0) {
        result.message = error;
        return result;
    }

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

    PruneBackups(title_backups);
    result.outcome = SaveImportResult::Outcome::Imported;
    const size_t kb = static_cast<size_t>((save.bytes + ext.bytes + 1023) / 1024);
    result.message = Format(Text(ext_found > 0 ? "save_done_ext" : "save_done"),
                            std::to_string(kb));
    return result;
}

} // namespace Link
