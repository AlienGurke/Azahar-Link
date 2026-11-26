// Copyright Azahar-Link contributors
// Licensed under GPLv3 or any later version
// Refer to the LICENSE.txt file included.

#include <thread>
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstring>

#include <QAction>
#include <QActionGroup>
#include <QDesktopServices>
#include <QDir>
#include <QEventLoop>
#include <QMessageBox>
#include <QProgressDialog>
#include <QPushButton>
#include <QCoreApplication>
#include <QUrl>
#include <QInputDialog>
#include <QLabel>
#include <QMainWindow>
#include <QMenu>
#include <QMenuBar>
#include <QSettings>
#include <QStatusBar>
#include <QTimer>

#include <set>
#include <QFileInfo>
#include <QStandardItemModel>
#include "citra_qt/game_list.h"
#include "citra_qt/game_list_p.h"
#include "citra_qt/uisettings.h"
#include "citra_qt/bootmanager.h"
#include "citra_qt/link/link_client.h"
#include "citra_qt/link/link_osd.h"
#include "citra_qt/link/link_save.h"
#include "citra_qt/link/link_ui.h"
#include <QDialog>
#include <QDialogButtonBox>
#include <QScreen>
#include <QTextBrowser>
#include <QVBoxLayout>
#include <ctime>
#include <QDateTime>
#include <fmt/format.h>
#include "core/hle/service/cfg/cfg.h"
#include "core/hle/service/hid/hid.h"
#include "common/file_util.h"
#include "common/settings.h"
#include "core/core.h"
#include "core/hle/kernel/shared_page.h"
#include "core/loader/loader.h"

namespace Link {

namespace {
QString Q(const char* key) {
    return QString::fromStdString(Text(key));
}

// text for the 3DS tab: no tabs or line breaks inside the fields
std::string Field(const QString& s) {
    std::string out = s.toStdString();
    for (char& c : out) {
        if (c == '\t' || c == '\n' || c == '\r') {
            c = ' ';
        }
    }
    return out;
}

void CollectGames(QStandardItem* parent, std::set<QString>& seen, std::string& out,
                  std::map<u64, std::string>& names) {
    for (int r = 0; r < parent->rowCount(); ++r) {
        QStandardItem* item = parent->child(r, 0);
        if (!item) {
            continue;
        }
        const QString path = item->data(GameListItemPath::FullPathRole).toString();
        if (!path.isEmpty() && !seen.count(path)) {
            seen.insert(path);
            const u64 tid = item->data(GameListItemPath::ProgramIdRole).toULongLong();
            QString name = item->data(GameListItemPath::TitleRole).toString();
            if (name.isEmpty()) {
                name = QFileInfo(path).completeBaseName();
            }
            out += fmt::format("{:016X}\t{}\t{}\n", tid, Field(path), Field(name));
            if (tid) {
                names[tid] = Field(name);
            }
        }
        if (item->hasChildren()) {
            CollectGames(item, seen, out, names);
        }
    }
}
} // namespace

// Azahar's game list goes to the 3DS tab of the app
void LinkUi::UpdateLibrary() {
    if (!game_list) {
        return;
    }
    std::set<QString> seen;
    std::string text;
    std::map<u64, std::string> names;
    QStandardItemModel* model = game_list->GetModel();
    if (model) {
        CollectGames(model->invisibleRootItem(), seen, text, names);
    }
    library_names = std::move(names);
    Get().SetLibrary(text);
}

LinkUi::LinkUi(QMainWindow* window_) : QObject(window_), window(window_) {
    instance = this;
    game_list = window->findChild<GameList*>();
    stop_action = window->findChild<QAction*>(QStringLiteral("action_Stop"));
    if (game_list) {
        connect(game_list, &GameList::PopulatingCompleted, this, [this] { UpdateLibrary(); });
        QTimer::singleShot(0, this, [this] { UpdateLibrary(); }); // in case it is done already
    }
    // the link's settings (azahar-link.ini, before: 3ds-link.ini - renamed once)
    const std::string config_dir = FileUtil::GetUserPath(FileUtil::UserPath::ConfigDir);
    const std::string path = config_dir + "azahar-link.ini";
    if (!FileUtil::Exists(path) && FileUtil::Exists(config_dir + "3ds-link.ini")) {
        const bool ok = FileUtil::Rename(config_dir + "3ds-link.ini", path);
        Link::Log(std::string("folder: config/3ds-link.ini -> azahar-link.ini: ") +
                  (ok ? "ok" : "failed"));
    }
    settings = new QSettings(QString::fromStdString(path), QSettings::IniFormat, this);

    // the language of the link's texts (3DS Link > Language; until chosen: Azahar's)
    SetLanguage(settings->value(QStringLiteral("language")).toString().toStdString());

    // The menu goes right before "Help". Same groups as in melonDS-Link: connect, the
    // actions, folders - then the settings by topic - then language, debug, help.
    QMenu* menu = new QMenu(Q("menu"), window);
    menus.push_back({menu, "menu"});
    QMenu* help = window->menuBar()->findChild<QMenu*>(QStringLiteral("menu_Help"));
    if (help) {
        window->menuBar()->insertMenu(help->menuAction(), menu);
    } else {
        window->menuBar()->addMenu(menu);
    }
    const auto add_action = [this](QMenu* m, const char* key) {
        QAction* a = m->addAction(Q(key));
        texts.push_back({a, key});
        return a;
    };
    const auto add_menu = [this](QMenu* m, const char* key) {
        QMenu* sub = m->addMenu(Q(key));
        menus.push_back({sub, key});
        return sub;
    };
    // a setting that is on or off, stored under `name`
    const auto add_check = [this, add_action](QMenu* m, const char* key, const char* name,
                                              auto apply, bool def = true) {
        QAction* a = add_action(m, key);
        a->setCheckable(true);
        const bool on = settings->value(QString::fromLatin1(name), def).toBool();
        a->setChecked(on);
        apply(on);
        connect(a, &QAction::toggled, this, [this, name, apply](bool value) {
            settings->setValue(QString::fromLatin1(name), value);
            apply(value);
        });
        return a;
    };

    connect_action = add_action(menu, "connect");
    disconnect_action = add_action(menu, "disconnect");
    menu->addSeparator();
    QAction* mii_action = add_action(menu, "miisync");
    connect(mii_action, &QAction::triggered, this, [] { Get().RequestMiiSync(); });
    QAction* repair_action = add_action(menu, "repair");
    connect(repair_action, &QAction::triggered, this, [this] {
        const u64 tid = settings->value(QStringLiteral("last_title"), 0).toULongLong();
        if (tid == 0) {
            QMessageBox::information(window, Q("write_title"), Q("repair_none"));
            return;
        }
        const QString name = QStringLiteral("%1").arg(tid, 16, 16, QLatin1Char('0')).toUpper();
        if (QMessageBox::question(window, Q("write_title"),
                                  Q("repair_ask").replace(QStringLiteral("%1"), name)) !=
            QMessageBox::Yes) {
            return;
        }
        const LinkArchiveReq ar = {LINK_ARCHIVE_USER_SAVEDATA, LINK_MEDIA_SD,
                                   static_cast<uint32_t>(tid & 0xFFFFFFFF),
                                   static_cast<uint32_t>(tid >> 32), 0, 0};
        uint16_t status = 0;
        const bool sent = Get().ArcSimple(LINK_CMD_SECUREDEL, ar, "", status);
        Log(fmt::format("repair: secure value of {:016X}: {} (3DS code 0x{:08X})", tid,
                        sent ? status : 999, Get().LastArcResult()));
        if (sent && status == LINK_OK) {
            QMessageBox::information(window, Q("write_title"), Q("repair_done"));
        } else {
            QMessageBox::warning(window, Q("write_title"),
                                 Q("repair_failed")
                                     .replace(QStringLiteral("%1"),
                                              QString::number(sent ? status : 999)));
        }
    });
    {
        // the folders (as in melonDS-Link)
        QMenu* folders = add_menu(menu, "folders");
        const auto add_folder = [this, add_action, folders](const char* key, auto dir) {
            QAction* a = add_action(folders, key);
            connect(a, &QAction::triggered, this, [dir] {
                const QString d = QString::fromStdString(dir());
                QDir().mkpath(d);
                QDesktopServices::openUrl(QUrl::fromLocalFile(d));
            });
        };
        add_folder("folder_backups", [] { return BackupRoot(); });
        add_folder("folder_debug", [] { return LinkDir("Debug"); });
    }

    // the settings, by topic
    menu->addSeparator();
    QMenu* conn_menu = add_menu(menu, "group_connection");
    QMenu* clock_menu = add_menu(menu, "group_clock");
    QMenu* video_menu = add_menu(menu, "group_video");
    QMenu* pc_menu = add_menu(menu, "group_pc");
    QMenu* save_menu = add_menu(menu, "group_saves");

    // defaults like melonDS Link: the save comes from the 3DS when a game starts and goes
    // back whenever the game saves and when it stops, without questions. Earlier versions
    // started more carefully; their settings are switched over once.
    if (settings->value(QStringLiteral("defaults_version"), 0).toInt() < 2) {
        settings->setValue(QStringLiteral("write_back"), true);
        settings->setValue(QStringLiteral("write_live"), true);
        settings->setValue(QStringLiteral("write_ask"), false);
        settings->setValue(QStringLiteral("save_mode"), 1);
        settings->setValue(QStringLiteral("defaults_version"), 2);
    }

    // connection: on start, pauses, what comes from the 3DS on every connect
    auto_action = add_check(conn_menu, "autoconnect", "autoconnect", [](bool) {}, false);
    add_check(conn_menu, "opt_pause_loss", "pause_on_loss", [this](bool on) { pause_on_loss = on; });
    add_check(conn_menu, "opt_pause_lid", "pause_on_lid", [this](bool on) {
        pause_on_lid = on;
        // not paused when closed: the 3DS stays awake and the game runs on
        Get().SetOptions(on ? 0 : LINK_OPT_KEEP_AWAKE);
    });
    conn_menu->addSeparator();
    // the clock: time jump (like RTChanger on the 3DS), "remove the lock", 3DS clock
    jump_days = settings->value(QStringLiteral("time_jump_days"), 0).toInt();
    penalty_step = settings->value(QStringLiteral("penalty_step"), 0).toInt();
    penalty_days = std::max(1, settings->value(QStringLiteral("penalty_days"), 3).toInt());
    SharedPage::SetLinkTimeJump(static_cast<s64>(jump_days) * 86400);
    Get().SetTimeJump(static_cast<int64_t>(jump_days) * 86400);
    connect(clock_menu, &QMenu::aboutToShow, this, [this, clock_menu] { BuildClockMenu(clock_menu); });
    clock_menu->addAction(QString()); // (filled when it opens)
    ApplyClockOption(settings->value(QStringLiteral("clock"), true).toBool());
    add_check(conn_menu, "opt_miis", "miis", [](bool on) { Get().option_miis = on; });
    add_check(conn_menu, "opt_sys", "systemdata", [](bool on) { Get().option_system_data = on; });
    add_check(conn_menu, "opt_pc_input", "pc_input",
              [](bool on) { Service::HID::LinkController::SetPcInputAllowed(on); });

    // the picture on the 3DS
    add_check(video_menu, "opt_h264", "h264", [](bool on) { Get().option_h264 = on; });
    add_check(video_menu, "opt_tile20", "tile20", [](bool on) { Get().option_tile20 = on; },
              false);
    video_menu->addSeparator();
    {
        // WiFi priority of the bottom picture (the 3DS's buttons are always the top class)
        QMenu* prio = add_menu(video_menu, "wifi_prio");
        auto* group = new QActionGroup(prio);
        const int cur = std::clamp(settings->value(QStringLiteral("wifi_prio"), 0).toInt(), 0, 2);
        static const char* const keys[3] = {"wifi_prio_0", "wifi_prio_1", "wifi_prio_2"};
        for (const int i : {0, 2, 1}) { // from the highest to the lowest
            QAction* a = add_action(prio, keys[i]);
            a->setCheckable(true);
            a->setChecked(i == cur);
            group->addAction(a);
            connect(a, &QAction::triggered, this, [this, i] {
                settings->setValue(QStringLiteral("wifi_prio"), i);
                Get().wifi_prio = i;
            });
        }
        Get().wifi_prio = cur;
    }

    // the picture on the PC: black while closed, the 3DS battery and WiFi
    add_check(pc_menu, "opt_blackout", "black_when_closed", [this](bool on) { blackout = on; },
              false);
    pc_menu->addSeparator();
    {
        status_mode = std::clamp(settings->value(QStringLiteral("status_mode"), 0).toInt(), 0, 8);
        status_battery = settings->value(QStringLiteral("status_battery"), true).toBool();
        status_wifi = settings->value(QStringLiteral("status_wifi"), true).toBool();
        status_big_only = settings->value(QStringLiteral("status_big_only"), true).toBool();
        QMenu* sm = add_menu(pc_menu, "status_menu");
        connect(sm, &QMenu::aboutToShow, this, [this, sm] {
            sm->clear();
            auto* group = new QActionGroup(sm);
            // menu order; 8 = wandering over a bigger area (the same numbers as melonDS-Link)
            static const int order[] = {0, 1, 2, 8, 3, 4, 5, 6, 7};
            static const int mins[] = {5, 10, 20, 30, 60};
            for (const int m : order) {
                const QString label =
                    m == 0   ? Q("status_off")
                    : m == 1 ? Q("status_always")
                    : m == 2 ? Q("status_moving")
                    : m == 8 ? Q("status_wander")
                             : Q("status_every").replace(QStringLiteral("%1"),
                                                         QString::number(mins[m - 3]));
                QAction* a = sm->addAction(label);
                a->setCheckable(true);
                a->setChecked(m == status_mode);
                group->addAction(a);
                connect(a, &QAction::triggered, this, [this, m] {
                    status_mode = m;
                    settings->setValue(QStringLiteral("status_mode"), m);
                });
                if (m == 8) {
                    sm->addSeparator();
                }
            }
            sm->addSeparator();
            const auto line = [this, sm](const char* key, bool& value, const char* name) {
                QAction* a = sm->addAction(Q(key));
                a->setCheckable(true);
                a->setChecked(value);
                connect(a, &QAction::toggled, this, [this, &value, name](bool on) {
                    value = on;
                    settings->setValue(QString::fromLatin1(name), on);
                });
            };
            line("status_show_battery", status_battery, "status_battery");
            line("status_show_wifi", status_wifi, "status_wifi");
            sm->addSeparator();
            line("status_big_only", status_big_only, "status_big_only");
        });
    }

    // saves and backups
    {
        QMenu* start_menu = add_menu(save_menu, "opt_save");
        QActionGroup* save_group = new QActionGroup(start_menu);
        save_mode = settings->value(QStringLiteral("save_mode"), 1).toInt();
        int mode = 0;
        for (const char* key : {"opt_save_ask", "opt_save_always", "opt_save_never"}) {
            QAction* a = add_action(start_menu, key);
            a->setCheckable(true);
            a->setChecked(mode == save_mode);
            save_group->addAction(a);
            connect(a, &QAction::triggered, this, [this, mode] {
                save_mode = mode;
                settings->setValue(QStringLiteral("save_mode"), mode);
            });
            mode++;
        }
    }
    add_check(save_menu, "opt_write", "write_back", [](bool on) { write_back = on; });
    add_check(save_menu, "opt_write_live", "write_live", [](bool on) { write_live = on; });
    add_check(save_menu, "opt_write_ask", "write_ask", [](bool on) { write_ask = on; });
    save_menu->addSeparator();
    {
        // backups: how many of the newest, and for how many days played the first one
        backup_keep = std::max(1, settings->value(QStringLiteral("backup_keep"), 3).toInt());
        backup_days = settings->value(QStringLiteral("backup_days"), 7).toInt();
        QAction* keep = save_menu->addAction(QString());
        QAction* days = save_menu->addAction(QString());
        const auto label = [keep, days] {
            keep->setText(Q("opt_backup_keep").replace(QStringLiteral("%1"),
                                                       QString::number(backup_keep)));
            days->setText(Q("opt_backup_days").replace(QStringLiteral("%1"),
                                                       QString::number(backup_days)));
        };
        label();
        relabel.push_back(label);
        connect(keep, &QAction::triggered, this, [this, label] {
            bool ok = false;
            const int n = QInputDialog::getInt(window, Q("backups_title"),
                                               Q("opt_backup_keep_ask"), backup_keep, 1, 1000,
                                               1, &ok);
            if (ok) {
                backup_keep = n;
                settings->setValue(QStringLiteral("backup_keep"), n);
                label();
            }
        });
        connect(days, &QAction::triggered, this, [this, label] {
            bool ok = false;
            const int n = QInputDialog::getInt(window, Q("backups_title"),
                                               Q("opt_backup_days_ask"), backup_days, 0, 3650,
                                               1, &ok);
            if (ok) {
                backup_days = n;
                settings->setValue(QStringLiteral("backup_days"), n);
                label();
            }
        });
    }

    {
        // which messages Azahar shows (all by default; errors always)
        static const struct {
            Note note;
            const char* name;
            const char* key;
        } kNotes[] = {
            {Note::Connection, "no_note_connection", "notes_connection"},
            {Note::Save, "no_note_save", "notes_save"},
            {Note::Pause, "no_note_pause", "notes_pause"},
            {Note::Battery, "no_note_battery", "notes_battery"},
        };
        for (const auto& n : kNotes) {
            SetNoteShown(n.note, !settings->value(QString::fromLatin1(n.name), false).toBool());
        }
        QMenu* notes = add_menu(menu, "notes_menu");
        connect(notes, &QMenu::aboutToShow, this, [this, notes] {
            notes->clear();
            for (const auto& n : kNotes) {
                QAction* a = notes->addAction(Q(n.key));
                a->setCheckable(true);
                a->setChecked(NoteShown(n.note));
                const Note note = n.note;
                const char* name = n.name;
                connect(a, &QAction::toggled, this, [this, note, name](bool on) {
                    SetNoteShown(note, on);
                    settings->setValue(QString::fromLatin1(name), !on);
                });
            }
        });
    }

    menu->addSeparator();
    {
        QMenu* lang = add_menu(menu, "language");
        connect(lang, &QMenu::aboutToShow, this, [this, lang] {
            lang->clear();
            auto* group = new QActionGroup(lang);
            for (const auto& [code, name] :
                 {std::pair<const char*, const char*>{"en", "English"}, {"de", "Deutsch"}}) {
                QAction* a = lang->addAction(QString::fromUtf8(name));
                a->setCheckable(true);
                a->setChecked(Language() == code);
                group->addAction(a);
                const std::string c = code;
                connect(a, &QAction::triggered, this, [this, c] {
                    SetLanguage(c);
                    settings->setValue(QStringLiteral("language"), QString::fromStdString(c));
                    Retranslate();
                });
            }
        });
    }
    {
        // Debug: the log on/off (on by default) - as in melonDS-Link
        QMenu* debug = add_menu(menu, "debug");
        add_check(debug, "log_write", "log", [](bool on) { SetLogging(on); });
    }
    QAction* help_action = add_action(menu, "help");
    connect(help_action, &QAction::triggered, this, [this] { ShowHelp(); });
    disconnect_action->setEnabled(false);

    connect(connect_action, &QAction::triggered, this, &LinkUi::OnConnect);
    connect(disconnect_action, &QAction::triggered, this, &LinkUi::OnDisconnect);
    connect(auto_action, &QAction::toggled, this, &LinkUi::OnAutoConnect);

    status_label = new QLabel(window);
    status_label->setContentsMargins(4, 0, 4, 0);
    status_label->setVisible(false);
    window->statusBar()->addPermanentWidget(status_label);

    timer = new QTimer(this);
    connect(timer, &QTimer::timeout, this, &LinkUi::Poll);
    timer->start(250);

    const QString host = settings->value(QStringLiteral("host")).toString();
    if (auto_action->isChecked() && !host.isEmpty()) {
        // (a time jump: asked once the window is there)
        QTimer::singleShot(0, this, [this, host] {
            if (JumpNeutralForConnect()) {
                Get().Connect(host.toStdString());
            }
        });
    }
}

LinkUi::~LinkUi() {
    if (live_thread.joinable()) {
        live_thread.join();
    }
    // before the renderer and the HID service go away
    Get().Disconnect();
}

int LinkUi::save_mode = 0;
bool LinkUi::write_live = false;
bool LinkUi::live_ok = false;
bool LinkUi::live_wrote = false;
u64 LinkUi::live_fp = 0;
qint64 LinkUi::live_changed_at = 0;
qint64 LinkUi::live_last_write = 0;
std::optional<u64> LinkUi::link_user_offset;
LinkUi* LinkUi::instance = nullptr;
bool LinkUi::write_back = false;
bool LinkUi::write_ask = true;
u64 LinkUi::loaded_program_id = 0;
u64 LinkUi::loaded_extdata_id = 0;

void LinkUi::AfterShutdown(QWidget* parent) {
    if (instance) {
        instance->FinishLiveWrite(); // a write while playing must be done first
        // a time jump is still on: before playing on the 3DS, undo it / remove the lock
        if (instance->jump_days != 0 && Get().GetState() == State::Connected) {
            QMessageBox::information(parent, Q("group_clock"),
                                     Q("jump_reminder").replace(QStringLiteral("%1"),
                                                                JumpText(instance->jump_days)));
        }
    }
    const u64 program_id = loaded_program_id;
    const u64 extdata_id = loaded_extdata_id;
    loaded_program_id = 0;
    const bool live_session = live_ok;
    live_ok = false;
    // (the log: after a game started from the 3DS while another ran, the last save of that
    // one was not written back once - the next start then asked which save counts)
    Log(fmt::format("game ended: program {:016X}, write back {}, live {}, connected {}", program_id,
                    write_back, live_session, Get().GetState() == State::Connected));
    if (program_id == 0 || !write_back) {
        return;
    }
    if (Get().GetState() != State::Connected) {
        QMessageBox::warning(parent, Q("write_title"), Q("write_no_3ds"));
        return;
    }
    if (write_ask && !(write_live && live_session) &&
        QMessageBox::question(parent, Q("write_title"), Q("write_ask")) !=
                         QMessageBox::Yes) {
        return;
    }

    QProgressDialog progress(Q("write_checking"), QString(), 0, 0, parent);
    progress.setWindowTitle(Q("write_title"));
    progress.setWindowModality(Qt::WindowModal);
    progress.setMinimumDuration(0);
    progress.setCancelButton(nullptr);
    progress.setMinimumWidth(420);
    progress.show();

    SaveExportResult result;
    QEventLoop loop;
    std::thread worker([&] {
        result = ExportSaveTo3DS(
            program_id, extdata_id,
            [&progress](const std::string& text) {
                QMetaObject::invokeMethod(
                    &progress,
                    [&progress, label = QString::fromStdString(text)] {
                        progress.setLabelText(label);
                    },
                    Qt::QueuedConnection);
            },
            !live_wrote); // after writes while playing, the 3DS state before is already kept
        live_wrote = false;
        QMetaObject::invokeMethod(&loop, "quit", Qt::QueuedConnection);
    });
    loop.exec();
    worker.join();
    QCoreApplication::processEvents();
    progress.close();

    const QString message = QString::fromStdString(result.message);
    switch (result.outcome) {
    case SaveExportResult::Outcome::Written:
    case SaveExportResult::Outcome::NothingToDo:
        if (instance && NoteShown(Note::Save)) {
            instance->ShowNote(message);
        }
        break;
    case SaveExportResult::Outcome::NoBaseline:
    case SaveExportResult::Outcome::Conflict:
        QMessageBox::warning(parent, Q("write_title"), message);
        break;
    case SaveExportResult::Outcome::Failed:
    default:
        QMessageBox::critical(parent, Q("write_title"),
                              Q("write_failed").replace(QStringLiteral("%1"), message));
        break;
    }
}

bool LinkUi::BeforeBoot(QWidget* parent, const QString& filename) {
    if (Get().GetState() != State::Connected || save_mode == 2) {
        return true;
    }
    u64 program_id = 0;
    u64 extdata_id = 0;
    const auto loader = Loader::GetLoader(filename.toStdString());
    if (!loader || loader->ReadProgramId(program_id) != Loader::ResultStatus::Success ||
        program_id == 0) {
        return true;
    }
    if (loader->ReadExtdataId(extdata_id) != Loader::ResultStatus::Success) {
        extdata_id = 0;
    }

    if (save_mode == 0) {
        QMessageBox ask(QMessageBox::Question, Q("save_ask_title"), Q("save_ask"),
                        QMessageBox::NoButton, parent);
        QPushButton* load = ask.addButton(Q("save_ask_load"), QMessageBox::AcceptRole);
        QPushButton* pc = ask.addButton(Q("save_ask_pc"), QMessageBox::RejectRole);
        ask.addButton(QMessageBox::Cancel);
        ask.setDefaultButton(load);
        ask.exec();
        if (ask.clickedButton() == pc) {
            return true;
        }
        if (ask.clickedButton() != load) {
            return false; // cancelled: no start
        }
    }

    QProgressDialog progress(Q("save_reading"), QString(), 0, 0, parent);
    progress.setWindowTitle(Q("save_progress_title"));
    progress.setWindowModality(Qt::WindowModal);
    progress.setMinimumDuration(0);
    progress.setCancelButton(nullptr);
    progress.setMinimumWidth(420);
    progress.show();

    std::string game_title;
    loader->ReadTitle(game_title, true);
    if (game_title.empty()) {
        game_title = fmt::format("{:016X}", program_id);
    }

    SaveImportResult result;
    SaveExportResult kept_write;
    QEventLoop loop;
    std::thread worker([&] {
        const auto show = [&progress](const std::string& text) {
            QMetaObject::invokeMethod(
                &progress,
                [&progress, label = QString::fromStdString(text)] {
                    progress.setLabelText(label);
                },
                Qt::QueuedConnection);
        };
        // played on the PC since the last sync: the question comes on the 3DS
        const auto ask = [&](const std::string& question) -> int {
            Client& c = Get();
            static std::atomic<uint32_t> next_id{
                static_cast<uint32_t>(QDateTime::currentMSecsSinceEpoch())};
            const uint32_t id = next_id++;
            show(Text("ask_on_3ds"));
            if (!c.Ask(id, game_title + "\n\n" + question)) {
                return 1; // no question possible: the 3DS save as always
            }
            const auto t0 = std::chrono::steady_clock::now();
            while (true) {
                int choice = 0;
                if (c.TakeAnswer(id, choice)) {
                    return choice == 0 ? 0 : 1;
                }
                if (c.GetState() != State::Connected) {
                    return -1;
                }
                if (std::chrono::steady_clock::now() - t0 > std::chrono::minutes(10)) {
                    c.CancelAsk(id); // nobody answers: the 3DS save (the PC one is a backup)
                    return 1;
                }
                std::this_thread::sleep_for(std::chrono::milliseconds(100));
            }
        };
        result = ImportSaveFrom3DS(program_id, extdata_id, show, ask);
        if (result.outcome == SaveImportResult::Outcome::KeptPC) {
            // the PC save goes to the 3DS right away (the 3DS state is in the backups)
            kept_write = ExportSaveTo3DS(program_id, extdata_id, show, false);
        }
        QMetaObject::invokeMethod(&loop, "quit", Qt::QueuedConnection);
    });
    loop.exec();
    worker.join();
    QCoreApplication::processEvents(); // drop the last label updates before closing
    progress.close();

    QString message = QString::fromStdString(result.message);
    if (result.outcome == SaveImportResult::Outcome::KeptPC) {
        if (kept_write.outcome == SaveExportResult::Outcome::Written ||
            kept_write.outcome == SaveExportResult::Outcome::NothingToDo) {
            message = Q("save_kept_pc_done");
        } else {
            QMessageBox::warning(parent, Q("write_title"),
                                 Q("write_failed").replace(
                                     QStringLiteral("%1"),
                                     QString::fromStdString(kept_write.message)));
        }
    }
    switch (result.outcome) {
    case SaveImportResult::Outcome::KeptPC:
    case SaveImportResult::Outcome::Imported:
        loaded_program_id = program_id; // its save may go back to the 3DS when it stops
        loaded_extdata_id = extdata_id;
        live_ok = true;
        live_wrote = false;
        live_fp = SaveFingerprint(program_id, extdata_id);
        live_changed_at = 0;
        live_last_write = 0;
        if (instance) {
            instance->settings->setValue(QStringLiteral("last_title"),
                                         static_cast<qulonglong>(program_id));
        }
        if (instance && NoteShown(Note::Save)) {
            instance->ShowNote(message);
        }
        return true;
    case SaveImportResult::Outcome::NoSave:
        QMessageBox::information(parent, Q("save_failed_title"), message);
        return true;
    case SaveImportResult::Outcome::Failed:
    default:
        return QMessageBox::question(parent, Q("save_failed_title"),
                                     Q("save_failed").replace(QStringLiteral("%1"), message)) ==
               QMessageBox::Yes;
    }
}

void LinkUi::OnConnect() {
    bool ok = false;
    const QString last = settings->value(QStringLiteral("host")).toString();
    const QString host = QInputDialog::getText(window, Q("ask_ip_title"), Q("ask_ip"),
                                               QLineEdit::Normal, last, &ok)
                             .trimmed();
    if (!ok || host.isEmpty()) {
        return;
    }
    settings->setValue(QStringLiteral("host"), host);
    if (!JumpNeutralForConnect()) {
        return;
    }
    Get().Connect(host.toStdString());
}

// the clock of the 3DS for Azahar's games (3DS Link > Clock)
void LinkUi::ApplyClockOption(bool on) {
    option_clock = on;
    // the user time offset of the 3DS, kept from the last connect, so games started
    // without the 3DS see the same raw clock too; off: Azahar's own offset again
    const QVariant stored = settings->value(QStringLiteral("user_time_offset"));
    if (on && stored.isValid()) {
        link_user_offset = static_cast<u64>(stored.toULongLong());
    } else {
        link_user_offset.reset();
    }
    Service::CFG::SetLinkUserTimeOffset(link_user_offset);
}

// "1 day", "+2 days", "-3 days"
QString LinkUi::JumpText(int days) {
    const QString n = (days > 0 ? QStringLiteral("+") : QString()) + QString::number(days);
    return Q(std::abs(days) == 1 ? "jump_day" : "jump_days").replace(QStringLiteral("%1"), n);
}

// the game clock jumps: at once, also in the game that runs; noted for the next start and
// shown on the 3DS
void LinkUi::SetJump(int days) {
    if (days == jump_days) {
        return;
    }
    jump_days = days;
    settings->setValue(QStringLiteral("time_jump_days"), days);
    SharedPage::SetLinkTimeJump(static_cast<s64>(days) * 86400);
    Get().SetTimeJump(static_cast<int64_t>(days) * 86400);
    Log("clock: time jump " + std::to_string(days) + " days");
    ShowNote(days == 0 ? Q("jump_real") : Q("jump_now").replace(QStringLiteral("%1"), JumpText(days)));
}

// connecting with a time jump: undo it first (the 3DS would get saves of another day)
bool LinkUi::JumpNeutralForConnect() {
    if (jump_days == 0) {
        return true;
    }
    if (QMessageBox::question(window, Q("group_clock"),
                              Q("jump_connect").replace(QStringLiteral("%1"), JumpText(jump_days))) !=
        QMessageBox::Yes) {
        return false;
    }
    penalty_step = 0;
    settings->setValue(QStringLiteral("penalty_step"), 0);
    SetJump(0);
    return true;
}

void LinkUi::BuildClockMenu(QMenu* m) {
    m->clear();
    QAction* info = m->addAction(jump_days == 0 ? Q("jump_none")
                                                : Q("jump_info").replace(QStringLiteral("%1"),
                                                                         JumpText(jump_days)));
    info->setEnabled(false);
    m->addSeparator();
    connect(m->addAction(Q("jump_fwd")), &QAction::triggered, this, [this] { SetJump(jump_days + 1); });
    connect(m->addAction(Q("jump_back")), &QAction::triggered, this, [this] { SetJump(jump_days - 1); });
    connect(m->addAction(Q("jump_set")), &QAction::triggered, this, [this] {
        bool ok = false;
        const int d = QInputDialog::getInt(window, Q("group_clock"), Q("jump_set_ask"), jump_days,
                                           -3650, 3650, 1, &ok);
        if (ok) {
            SetJump(d);
        }
    });
    QAction* real = m->addAction(Q("jump_reset"));
    real->setEnabled(jump_days != 0);
    connect(real, &QAction::triggered, this, [this] {
        penalty_step = 0;
        settings->setValue(QStringLiteral("penalty_step"), 0);
        SetJump(0);
    });
    m->addSeparator();
    // "remove the lock" (the game's penalty after a jump back): N days behind the real time,
    // start the game once (the lock appears), then back to the real time - the lock is gone
    if (penalty_step == 0) {
        QAction* a = m->addAction(Q("penalty_1").replace(QStringLiteral("%1"), QString::number(penalty_days)));
        connect(a, &QAction::triggered, this, [this] {
            if (QMessageBox::question(window, Q("group_clock"),
                                      Q("penalty_1_ask").replace(QStringLiteral("%1"),
                                                                 QString::number(penalty_days))) !=
                QMessageBox::Yes) {
                return;
            }
            penalty_step = 1;
            settings->setValue(QStringLiteral("penalty_step"), 1);
            SetJump(-penalty_days);
        });
    } else {
        QAction* a = m->addAction(Q("penalty_2"));
        connect(a, &QAction::triggered, this, [this] {
            penalty_step = 0;
            settings->setValue(QStringLiteral("penalty_step"), 0);
            SetJump(0);
            ShowNote(Q("penalty_done"));
        });
    }
    QAction* days = m->addAction(Q("penalty_days").replace(QStringLiteral("%1"), QString::number(penalty_days)));
    connect(days, &QAction::triggered, this, [this] {
        bool ok = false;
        const int d = QInputDialog::getInt(window, Q("group_clock"), Q("penalty_days_ask"),
                                           penalty_days, 1, 30, 1, &ok);
        if (ok) {
            penalty_days = d;
            settings->setValue(QStringLiteral("penalty_days"), d);
        }
    });
    m->addSeparator();
    QAction* clock = m->addAction(Q("opt_clock"));
    clock->setCheckable(true);
    clock->setChecked(option_clock);
    connect(clock, &QAction::toggled, this, [this](bool on) {
        settings->setValue(QStringLiteral("clock"), on);
        ApplyClockOption(on);
    });
}

void LinkUi::OnDisconnect() {
    Get().Disconnect();
}

void LinkUi::OnAutoConnect(bool on) {
    settings->setValue(QStringLiteral("autoconnect"), on);
}

// the texts again, in the language just chosen
void LinkUi::Retranslate() {
    for (auto& [a, key] : texts) {
        a->setText(Q(key));
    }
    for (auto& [m, key] : menus) {
        m->setTitle(Q(key));
    }
    for (auto& f : relabel) {
        f();
    }
}

void LinkUi::ShowHelp() {
    // "# heading" and "- point" lines into a simple page (as in melonDS-Link)
    QString html;
    bool in_list = false;
    for (const QString& line : Q("help_text").split(QLatin1Char('\n'))) {
        if (line.startsWith(QStringLiteral("# "))) {
            if (in_list) {
                html += QStringLiteral("</ul>");
                in_list = false;
            }
            html += QStringLiteral("<h3>") + line.mid(2).toHtmlEscaped() + QStringLiteral("</h3>");
        } else if (line.startsWith(QStringLiteral("- "))) {
            if (!in_list) {
                html += QStringLiteral("<ul>");
                in_list = true;
            }
            html += QStringLiteral("<li>") + line.mid(2).toHtmlEscaped() + QStringLiteral("</li>");
        }
    }
    if (in_list) {
        html += QStringLiteral("</ul>");
    }
    QDialog dlg(window);
    dlg.setWindowTitle(Q("help_title"));
    dlg.resize(680, 620);
    auto* lay = new QVBoxLayout(&dlg);
    auto* view = new QTextBrowser(&dlg);
    view->setHtml(html);
    lay->addWidget(view);
    auto* bb = new QDialogButtonBox(QDialogButtonBox::Close, &dlg);
    connect(bb, &QDialogButtonBox::rejected, &dlg, &QDialog::reject);
    lay->addWidget(bb);
    dlg.exec();
}

// Azahar's picture: the render window of the game (the main window holds it, or it is a
// window of its own)
QWidget* LinkUi::RenderWidget() {
    return window->findChild<GRenderWindow*>();
}

bool LinkUi::WindowBig() {
    QWidget* r = RenderWidget();
    QWidget* top = r && r->isVisible() ? r->window() : window;
    // fullscreen, maximised, or at least three quarters of the screen wide or high
    if (top->isFullScreen() || top->isMaximized()) {
        return true;
    }
    QScreen* scr = top->screen();
    if (!scr) {
        return true;
    }
    const QRect area = scr->availableGeometry();
    const QRect mine = top->frameGeometry();
    return mine.width() >= area.width() * 3 / 4 || mine.height() >= area.height() * 3 / 4;
}

// a message: on the picture while a game runs (the status bar is gone in fullscreen),
// otherwise in the status bar
void LinkUi::ShowNote(const QString& text) {
    QWidget* r = RenderWidget();
    if (osd && r == osd_render && r->isVisible() && Core::System::GetInstance().IsPoweredOn()) {
        osd->AddMessage(text);
        return;
    }
    queued.append(text);
}

// the console and the running game, as in melonDS-Link: pause when the 3DS goes away or is
// closed (and continue when it is back), battery hints, battery/WiFi and black on the picture
void LinkUi::Console() {
    Client& client = Get();
    Core::System& system = Core::System::GetInstance();
    const State st = client.GetState();
    const Client::ConsoleState cs = client.GetConsoleState();
    const bool running = system.IsPoweredOn();
    const bool paused = running && system.frame_limiter.IsFrameAdvancing();
    const auto pause = [this] { QMetaObject::invokeMethod(window, "OnPauseGame"); };
    const auto resume = [this] {
        QMetaObject::invokeMethod(window, "OnResumeGame", Q_ARG(bool, false));
    };

    // the picture's boxes: made for the render window of the moment
    QWidget* r = RenderWidget();
    if (r != osd_render) {
        osd.reset(r ? new Osd(r) : nullptr);
        osd_render = r;
    }

    // the 3DS went away (app closed, WiFi gone) in the middle of a game: pause, so nothing
    // happens without a controller. Not when disconnected on purpose; closed: see below
    if (last_state == State::Connected && st == State::Connecting && pause_on_loss &&
        running && !paused && !cs.lid_closed) {
        pause();
        paused_by_loss = true;
        if (NoteShown(Note::Pause)) {
            ShowNote(Q("pause_loss"));
        }
    }
    if (st == State::Disconnected) {
        paused_by_loss = paused_by_lid = false;
    }
    last_state = st;
    // back in the game on the 3DS: continue
    if (client.TakeResumeRequest() && paused_by_loss) {
        paused_by_loss = false;
        if (running && paused) {
            resume();
            if (NoteShown(Note::Pause)) {
                ShowNote(Q("pause_resumed"));
            }
        }
    }
    // closed: pause (setting) - the 3DS says so before it goes to sleep
    if (pause_on_lid && cs.lid_closed && st != State::Disconnected && running && !paused &&
        !paused_by_lid) {
        pause();
        paused_by_lid = true;
        if (NoteShown(Note::Pause)) {
            ShowNote(Q("pause_lid"));
        }
    }
    if (paused_by_lid && !cs.lid_closed && st == State::Connected) {
        paused_by_lid = false;
        paused_by_loss = false;
        if (running && paused) {
            resume();
            if (NoteShown(Note::Pause)) {
                ShowNote(Q("pause_resumed"));
            }
        }
    }

    // battery hints at 30/20/10 % (on the PC and on the 3DS)
    if (cs.battery >= 0 && st == State::Connected) {
        if (cs.charging || cs.battery > battery_warned + 5) {
            battery_warned = 101;
        }
        for (const int th : {30, 20, 10}) {
            if (!cs.charging && cs.battery <= th && battery_warned > th) {
                const int level = cs.battery <= 10 ? 10 : cs.battery <= 20 ? 20 : 30;
                battery_warned = level;
                if (NoteShown(Note::Battery)) {
                    const char* key = level <= 10 ? "battery_10" : level <= 20 ? "battery_20" : "battery_30";
                    ShowNote(Q(key).replace(QStringLiteral("%1"), QString::number(cs.battery)));
                    const std::string t3 = Format(Text(level <= 10 ? "battery_10_3ds" : "battery_low_3ds"),
                                                  std::to_string(cs.battery), "");
                    std::thread([t3] { Get().SetStatusText(t3); }).detach();
                }
                break;
            }
        }
    }

    if (!osd) {
        return;
    }
    // battery/WiFi in the top right corner: always (fixed or moving, kind to OLEDs) or for a
    // few seconds every 5..60 minutes; only in fullscreen or a big window (setting)
    int shown = 0;
    if (cs.known && st == State::Connected && running) {
        if (status_mode == 1 || status_mode == 2) {
            shown = status_mode;
        } else if (status_mode == 8) {
            shown = 3;
        } else if (status_mode >= 3) {
            static const int mins[] = {5, 10, 20, 30, 60};
            const qint64 now_s = QDateTime::currentSecsSinceEpoch();
            if (client.ConnectionCount() != status_conn ||
                now_s - status_shown_at >= mins[status_mode - 3] * 60) {
                status_conn = client.ConnectionCount();
                status_shown_at = now_s;
            }
            if (now_s - status_shown_at < 6) {
                shown = 2;
            }
        }
        if (shown && status_big_only && !WindowBig()) {
            shown = 0;
        }
    }
    QStringList lines;
    const bool status_shown = shown != 0;
    if (jump_days != 0 && running && (!status_big_only || WindowBig())) {
        // a time jump is on: always a line (fixed), so it is not forgotten
        lines.append(Q("jump_info").replace(QStringLiteral("%1"), JumpText(jump_days)));
        if (!shown) {
            shown = 1;
        }
    }
    if (status_shown) {
        if (status_battery) {
            QString bat = cs.battery >= 0 ? QStringLiteral("%1 %").arg(cs.battery) : QStringLiteral("?");
            if (cs.charging) {
                bat += QStringLiteral(" ") + Q("status_charging");
            }
            lines.append(Q("osd_battery").replace(QStringLiteral("%1"), bat));
        }
        if (status_wifi) {
            static const char* const keys[] = {"wifi0", "wifi1", "wifi2", "wifi3"};
            const QString w = cs.wifi >= 0 ? Q(keys[cs.wifi]) : QStringLiteral("?");
            lines.append(Q("osd_wifi").replace(QStringLiteral("%1"), w));
        }
    }
    osd->SetStatus(lines, shown);
    // closed and paused: everything black if wanted (nothing lit on an OLED)
    osd->SetBlackout(blackout && paused_by_lid && running);
    osd->Update();
}

void LinkUi::ShowQueued() {
    const qint64 now = QDateTime::currentMSecsSinceEpoch();
    if (queued.isEmpty() || now < shown_until) {
        return;
    }
    window->statusBar()->showMessage(queued.takeFirst(), 7000);
    shown_until = now + 7000;
}

// While playing: when the game saved (Azahar's save files changed and then stayed the same
// for a few seconds), the save goes to the 3DS in the background - the same checked write
// as when the game stops. On a conflict or error it stops for this game.
void LinkUi::LiveWrite() {
    if (live_thread.joinable() && !live_busy) {
        live_thread.join();
        const SaveExportResult r = live_result;
        if (r.outcome == SaveExportResult::Outcome::Written) {
            live_wrote = true;
            if (NoteShown(Note::Save)) {
                ShowNote(Q("live_done"));
            }
        } else if (r.outcome != SaveExportResult::Outcome::NothingToDo) {
            live_ok = false; // nothing more while playing; the end of the game asks again
            queued.append(Q("live_stopped").replace(QStringLiteral("%1"),
                                                    QString::fromStdString(r.message)));
        }
    }
    if (!write_back || !write_live || !live_ok || loaded_program_id == 0 || live_busy ||
        Get().GetState() != State::Connected || !Core::System::GetInstance().IsPoweredOn()) {
        return;
    }
    const qint64 now = QDateTime::currentMSecsSinceEpoch();
    if (now - live_checked_at < 2000) {
        return;
    }
    live_checked_at = now;
    const u64 fp = SaveFingerprint(loaded_program_id, loaded_extdata_id);
    if (fp != live_fp) {
        live_fp = fp; // changed: wait until it stays like this
        live_changed_at = now;
        return;
    }
    if (live_changed_at == 0 || now - live_changed_at < 5000 || now - live_last_write < 20000) {
        return;
    }
    live_changed_at = 0;
    live_last_write = now;
    live_busy = true;
    const u64 pid = loaded_program_id, ext = loaded_extdata_id;
    const bool backup = !live_wrote; // the first write of a session keeps the 3DS state
    live_thread = std::thread([this, pid, ext, backup] {
        live_result = ExportSaveTo3DS(
            pid, ext, [](const std::string&) {}, backup);
        live_busy = false;
    });
}

void LinkUi::FinishLiveWrite() {
    if (live_thread.joinable()) {
        live_thread.join();
        if (live_result.outcome == SaveExportResult::Outcome::Written) {
            live_wrote = true;
        }
        live_busy = false;
    }
}

// wishes from the 3DS: start a game from the list, end the game, send the list again
void LinkUi::HandleRemote() {
    static bool busy = false; // starting a game can show dialogs (and run this timer again)
    if (busy) {
        return;
    }
    Client& client = Get();
    Core::System& system = Core::System::GetInstance();
    if (client.TakeLibraryRequest() && game_list) {
        game_list->PopulateAsync(UISettings::values.game_dirs); // then UpdateLibrary
        client.ResendLibrary();
    }
    if (client.TakeStopRequest() && system.IsPoweredOn() && stop_action) {
        busy = true;
        stop_action->trigger();
        busy = false;
    }
    std::string bak_path;
    if (client.TakeBackupListRequest(bak_path)) {
        // the backups of a game in the 3DS list: sent from a worker
        std::thread([this, bak_path] {
            u64 pid = 0, ext = 0;
            const auto loader = Loader::GetLoader(bak_path);
            if (!loader || loader->ReadProgramId(pid) != Loader::ResultStatus::Success) {
                pid = 0;
            }
            if (loader && loader->ReadExtdataId(ext) != Loader::ResultStatus::Success) {
                ext = 0;
            }
            std::vector<SaveBackup> list;
            if (pid != 0) {
                list = ListBackups(pid);
            }
            if (list.size() > 14) {
                list.resize(14); // what the 3DS shows
            }
            uint32_t token = 0;
            std::string text = bak_path;
            {
                std::scoped_lock lock{bak_mutex};
                token = ++bak_token;
                bak_entries = list;
                bak_program_id = pid;
                bak_extdata_id = ext;
            }
            for (const SaveBackup& b : list) {
                text += "\n" + b.label;
            }
            Get().SendBackupList(token, text);
        }).detach();
    }
    int bak_index = 0;
    uint32_t bak_tok = 0;
    if (client.TakeRestoreRequest(bak_path, bak_index, bak_tok)) {
        SaveBackup entry;
        u64 pid = 0, ext = 0;
        {
            std::scoped_lock lock{bak_mutex};
            if (bak_tok == bak_token && bak_index >= 0 &&
                bak_index < static_cast<int>(bak_entries.size())) {
                entry = bak_entries[bak_index];
                pid = bak_program_id;
                ext = bak_extdata_id;
            }
        }
        if (system.IsPoweredOn() || live_busy || restoring || pid == 0) {
            const std::string why = pid == 0 ? Text("restore_gone") : Text("restore_running");
            std::thread([why] { Get().SetStatusText(why); }).detach();
            window->statusBar()->showMessage(QString::fromStdString(why), 7000);
        } else {
            restoring = true;
            std::thread([this, entry, pid, ext] {
                const std::string err = RestoreBackup(pid, ext, entry, [](const std::string&) {});
                Client& c = Get();
                if (err.empty()) {
                    c.SetStatusText(Text("restore_done3ds"));
                    c.NotifyUser(Text("restore_done"));
                } else {
                    c.SetStatusText(Text("restore_failed") + ": " + err);
                    c.NotifyUser(Text("restore_failed") + ": " + err);
                }
                restoring = false;
            }).detach();
        }
    }
    std::string path;
    if (client.TakeLaunchRequest(path)) {
        const QString game = QString::fromStdString(path);
        if (!game_list || !QFileInfo::exists(game)) {
            Log("3DS: cannot start " + path + " (not found)");
            client.LaunchFailed();
            return;
        }
        busy = true;
        if (system.IsPoweredOn() && stop_action) {
            Log("3DS: another game - ending the running one first");
            stop_action->trigger(); // the 3DS asked before switching
            Log(std::string("3DS: running one ended: ") + (system.IsPoweredOn() ? "no" : "yes"));
        }
        emit game_list->GameChosen(game);
        busy = false;
        if (!system.IsPoweredOn()) {
            client.LaunchFailed(); // cancelled or failed: the 3DS stops waiting
        }
    }
}

void LinkUi::Poll() {
    Client& client = Get();
    HandleRemote();
    LiveWrite();

    // the running game, for the 3DS app
    Core::System& system = Core::System::GetInstance();
    const bool running = system.IsPoweredOn();
    if (running != game_running) {
        game_running = running;
        std::string title;
        if (running) {
            // the name the 3DS list shows, so the 3DS knows it is the game it picked
            u64 pid = 0;
            system.GetAppLoader().ReadProgramId(pid);
            const auto it = library_names.find(pid);
            if (it != library_names.end()) {
                title = it->second;
            } else {
                system.GetAppLoader().ReadTitle(title, true);
            }
        }
        client.SetGameName(title);
    }

    // the 3DS clock: Azahar's clock offset is set so its games see the same time
    int64_t delta = 0;
    if (client.TakeConsoleClock(delta) && option_clock) {
        // Azahar's console clock (kernel/shared_page.cpp) is the UTC time + 1 h in daylight
        // saving time + the offset, counted from 1 Jan 2000 in local time (std::mktime) -
        // which adds the time zone. The same sums here, so the offset makes it the 3DS clock.
        const std::time_t now = std::time(nullptr);
        const std::tm* local = std::localtime(&now);
        const int64_t dst = (local && local->tm_isdst > 0) ? 3600 : 0;
        std::tm epoch_tm{};
        epoch_tm.tm_mday = 1;
        epoch_tm.tm_year = 100;
        epoch_tm.tm_isdst = 0;
        const int64_t zone = 946684800LL - static_cast<int64_t>(std::mktime(&epoch_tm));
        const int64_t offset = delta - dst - zone;
        uint64_t user_offset = 0;
        const bool has_user_offset = client.UserTimeOffset(user_offset);
        const std::optional<u64> new_user_offset =
            has_user_offset ? std::optional<u64>(user_offset) : std::nullopt;
        const bool offset_changes = new_user_offset != link_user_offset;
        link_user_offset = new_user_offset;
        Service::CFG::SetLinkUserTimeOffset(new_user_offset);
        if (new_user_offset) {
            settings->setValue(QStringLiteral("user_time_offset"),
                               static_cast<qulonglong>(*new_user_offset));
        }
        const bool clock_changes =
            Settings::values.init_clock.GetValue() != Settings::InitClock::SystemTime ||
            std::abs(Settings::values.init_time_offset.GetValue() - offset) > 2 ||
            (offset_changes && system.IsPoweredOn());
        if (clock_changes) {
            Settings::values.init_clock = Settings::InitClock::SystemTime;
            Settings::values.init_time_offset = offset;
            const QString when = QDateTime::fromSecsSinceEpoch(
                                     QDateTime::currentSecsSinceEpoch() + delta, Qt::UTC)
                                     .toString(QStringLiteral("dd.MM.yyyy HH:mm"));
            const std::string key = system.IsPoweredOn() ? "clock_restart" : "clock_set";
            queued.append(Q(key.c_str()).replace(QStringLiteral("%1"), when));
        }
    }

    std::string msg;
    while (client.PopMessage(msg)) {
        ShowNote(QString::fromStdString(msg));
    }
    ShowQueued();
    Console();

    const State state = client.GetState();
    disconnect_action->setEnabled(state != State::Disconnected);
    const QString jump = jump_days == 0 ? QString()
                                        : Q("jump_info").replace(QStringLiteral("%1"), JumpText(jump_days));
    if (state == State::Disconnected) {
        status_label->setText(jump);
        status_label->setVisible(!jump.isEmpty());
        return;
    }

    QString text;
    if (state == State::Connecting) {
        text = Q("status_connecting");
    } else {
        const Client::ConsoleState console = client.GetConsoleState();
        text = console.lid_closed ? Q("status_closed") : Q("status_on");
        if (console.battery >= 0) {
            text += QStringLiteral(" · ") +
                    Q("battery").replace(QStringLiteral("%1"), QString::number(console.battery));
            if (console.charging) {
                text += QStringLiteral(" (") + Q("charging") + QStringLiteral(")");
            }
        }
    }
    if (!jump.isEmpty()) {
        text += QStringLiteral(" · ") + jump;
    }
    status_label->setText(text);
    status_label->setVisible(true);
}

} // namespace Link
