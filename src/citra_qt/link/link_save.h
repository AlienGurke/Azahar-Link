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
SaveImportResult ImportSaveFrom3DS(u64 program_id,
                                   const std::function<void(const std::string&)>& progress);

/// Folder with the backups of all games.
std::string BackupRoot();

} // namespace Link
