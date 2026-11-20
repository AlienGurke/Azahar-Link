// Copyright Azahar-Link contributors
// Licensed under GPLv3 or any later version
// Refer to the LICENSE.txt file included.

// 3DS Link: the save of a game (and its extdata) from the 3DS into Azahar.
//
// Step 1 of the save sync: only READS from the 3DS, nothing is ever written there.
// Before Azahar's own save is replaced, both it and the copy from the 3DS are kept
// in <user>/3ds-link/backups/<title id>/<time>/. The new files are written next to
// the old ones first and only then swapped in, so a failure leaves the old save.

#pragma once

#include <functional>
#include <string>

#include "common/common_types.h"

namespace Link {

struct SaveImportResult {
    enum class Outcome {
        Imported,  // save (and extdata, if any) taken from the 3DS
        NoSave,    // the 3DS has no save for this title: nothing changed
        Failed,    // something went wrong: Azahar's save is unchanged
    };
    Outcome outcome = Outcome::Failed;
    std::string message; // for the user
    std::string backup_dir;
};

/// Blocking: call it from a worker thread. `progress` gets short texts for the user.
/// `extdata_id`: from the game's header (0 = unknown: guessed from the title id).
SaveImportResult ImportSaveFrom3DS(u64 program_id, u64 extdata_id,
                                   const std::function<void(const std::string&)>& progress);

struct SaveExportResult {
    enum class Outcome {
        Written,     // the 3DS has Azahar's save now (checked, committed)
        NothingToDo, // Azahar's save is what the 3DS has
        NoBaseline,  // the save was not loaded from the 3DS: nothing to compare with
        Conflict,    // the save on the 3DS changed since it was loaded: nothing written
        Failed,      // something went wrong: the 3DS keeps its save as it was
    };
    Outcome outcome = Outcome::Failed;
    std::string message;
    std::string backup_dir;
};

/// Step 2 of the save sync: Azahar's save (and extdata) back to the 3DS. Only allowed if the
/// 3DS still has exactly what was loaded from it. Before writing, the 3DS state is kept as a
/// backup; after writing, every file is checked on the 3DS before the save is committed
/// (savedata changes only count from that moment). Blocking.
/// `make_backup` false: no backup folder (while playing, after the first write of a session -
/// the 3DS state before is the save written last time).
SaveExportResult ExportSaveTo3DS(u64 program_id, u64 extdata_id,
                                 const std::function<void(const std::string&)>& progress,
                                 bool make_backup = true);

/// Changes whenever a file of Azahar's copy of the save or extdata changes (names, sizes,
/// times): to notice that the game saved.
u64 SaveFingerprint(u64 program_id, u64 extdata_id);

/// The system data archives (Mii graphics CFL_Res, shared fonts, region and country data,
/// ...) that Azahar lacks are copied from the 3DS's CTR-NAND, as they are stored there.
/// Blocking. Returns the number of titles copied; `message` is set when something was
/// copied or went wrong.
int CopySystemDataFrom3DS(std::string& message);

/// Folder with the backups of all games.
std::string BackupRoot();

/// Besides the newest 20 backups of a game, the first backup of each of the last
/// `backup_days` days with backups is kept (setting, default 14).
extern int backup_days;

} // namespace Link
