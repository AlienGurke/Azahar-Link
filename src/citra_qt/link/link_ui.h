// Copyright Azahar-Link contributors
// Licensed under GPLv3 or any later version
// Refer to the LICENSE.txt file included.

// 3DS Link in Azahar's main window: the "3DS Link" menu, a status bar entry
// and the messages of the client. Its settings live in their own file
// (3ds-link.ini in Azahar's config folder), so Azahar's config stays untouched.

#pragma once

#include <QObject>
#include <QStringList>
#include "common/common_types.h"
#include <atomic>
#include <optional>

#include <map>
#include <string>
#include <thread>
#include <mutex>
#include <vector>
#include "citra_qt/link/link_save.h"

class GameList;
class QAction;
class QLabel;
class QMainWindow;
class QSettings;
class QTimer;
class QWidget;

namespace Link {

class LinkUi : public QObject {
    Q_OBJECT

public:
    explicit LinkUi(QMainWindow* window);
    ~LinkUi() override;

    /// Called before a game boots. If the 3DS is connected, offers to load the game's
    /// save from it (read only). Returns false if the user cancelled the start.
    static bool BeforeBoot(QWidget* parent, const QString& filename);

    /// Called after a game stopped: writes its save back to the 3DS if that is switched on
    /// and the save was loaded from the 3DS when the game started.
    static void AfterShutdown(QWidget* parent);

private:
    void OnConnect();
    void OnDisconnect();
    void OnAutoConnect(bool on);
    void Poll();
    void UpdateLibrary();
    void HandleRemote();
    void LiveWrite();
    void FinishLiveWrite();

    QMainWindow* window;
    QSettings* settings;
    QAction* connect_action;
    QAction* disconnect_action;
    QAction* auto_action;
    QLabel* status_label;
    QTimer* timer;
    GameList* game_list = nullptr;
    QAction* stop_action = nullptr;
    std::map<u64, std::string> library_names; // program id -> name in the 3DS list
    bool game_running = false;
    bool option_clock = true;
    /// how a game's save is handled when it starts: 0 ask, 1 always from the 3DS, 2 never
    static int save_mode;
    static std::optional<u64> link_user_offset; // of the 3DS, for the next boot
    static LinkUi* instance;
    static bool write_back;     // write the save back when the game stops (off by default)
    static bool write_ask;      // ask before that
    static bool write_live;     // also whenever the game saves (while playing)
    static bool live_ok;        // writes while playing go on for this game
    static bool live_wrote;     // something was written while playing (backup made)
    static u64 live_fp;         // Azahar's save files as last seen
    static qint64 live_changed_at, live_last_write;
    qint64 live_checked_at = 0;
    std::thread live_thread;
    std::atomic<bool> live_busy{false};
    SaveExportResult live_result;
    // the game that runs, if its save came from the 3DS
    static u64 loaded_program_id;
    static u64 loaded_extdata_id;
    // save backups the 3DS asked for (list sent with a token; a restore names its index)
    std::mutex bak_mutex;
    uint32_t bak_token = 0;
    std::vector<SaveBackup> bak_entries;
    u64 bak_program_id = 0, bak_extdata_id = 0;
    std::atomic<bool> restoring{false};
    // messages are shown one after the other, each for a few seconds
    QStringList queued;
    qint64 shown_until = 0;
    void ShowQueued();
};

} // namespace Link
