// Play time on this PC (3DS Link): every session of a game is written to
// %APPDATA%\3DS-Link\activity\azahar.bin, shared with melonDS-Link (melonds.bin) and read by
// Dashboard-Link. Format: docs/pc-activity.md in the 3DS-Link repository.

#pragma once

#include <string>
#include "common/common_types.h"

namespace Link::Activity {

/// Recording on/off ("Record play time on this PC", on by default).
void SetEnabled(bool on);
bool Enabled();

/// The game runs (first start or after a pause). The same game after a pause goes on in the
/// same session.
void Resume(u64 title_id, const std::string& name);
/// Paused: the time stops counting.
void Pause();
/// The game ended: the session is closed.
void End();

/// The folder (created when needed).
std::string Folder();

} // namespace Link::Activity
