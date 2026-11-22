// Copyright Azahar-Link contributors
// Licensed under GPLv3 or any later version
// Refer to the LICENSE.txt file included.

#include <thread>
#include <algorithm>

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
#include "citra_qt/link/link_client.h"
#include "citra_qt/link/link_save.h"
#include "citra_qt/link/link_ui.h"
#include <ctime>
#include <QDateTime>
#include <fmt/format.h>
#include "core/hle/service/cfg/cfg.h"
#include "common/file_util.h"
#include "common/settings.h"
#include "core/core.h"
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
    const std::string path =
        FileUtil::GetUserPath(FileUtil::UserPath::ConfigDir) + "3ds-link.ini";
    settings = new QSettings(QString::fromStdString(path), QSettings::IniFormat, this);

    // the menu goes right before "Help"
    QMenu* menu = new QMenu(Q("menu"), window);
    QMenu* help = window->menuBar()->findChild<QMenu*>(QStringLiteral("menu_Help"));
    if (help) {
        window->menuBar()->insertMenu(help->menuAction(), menu);
    } else {
        window->menuBar()->addMenu(menu);
    }
    connect_action = menu->addAction(Q("connect"));
    disconnect_action = menu->addAction(Q("disconnect"));
    QAction* mii_action = menu->addAction(Q("miisync"));
    connect(mii_action, &QAction::triggered, this, [] { Get().RequestMiiSync(); });
    QAction* repair_action = menu->addAction(Q("repair"));
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
    QAction* backups_action = menu->addAction(Q("backups_open"));
    connect(backups_action, &QAction::triggered, this, [] {
        const QString dir = QString::fromStdString(BackupRoot());
        QDir().mkpath(dir);
        QDesktopServices::openUrl(QUrl::fromLocalFile(dir));
    });
    menu->addSeparator();
    auto_action = menu->addAction(Q("autoconnect"));
    auto_action->setCheckable(true);
    auto_action->setChecked(settings->value(QStringLiteral("autoconnect"), false).toBool());

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

    // settings: what happens on connect and when a game starts (all on by default)
    QMenu* options = menu->addMenu(Q("settings"));
    const auto add_option = [&](const char* key, const char* name, auto apply, bool def = true) {
        QAction* a = options->addAction(Q(key));
        a->setCheckable(true);
        const bool on = settings->value(QString::fromLatin1(name), def).toBool();
        a->setChecked(on);
        apply(on);
        connect(a, &QAction::toggled, this, [this, name, apply](bool value) {
            settings->setValue(QString::fromLatin1(name), value);
            apply(value);
        });
    };
    add_option("opt_clock", "clock", [this](bool on) {
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
    });
    add_option("opt_miis", "miis", [](bool on) { Get().option_miis = on; });
    add_option("opt_sys", "systemdata", [](bool on) { Get().option_system_data = on; });
    add_option("opt_h264", "h264", [](bool on) { Get().option_h264 = on; });
    add_option("opt_tile20", "tile20", [](bool on) { Get().option_tile20 = on; }, false);
    {
        // WiFi priority of the bottom picture (the 3DS's buttons are always the top class)
        QMenu* prio = options->addMenu(Q("wifi_prio"));
        auto* group = new QActionGroup(prio);
        const int cur = std::clamp(settings->value(QStringLiteral("wifi_prio"), 0).toInt(), 0, 2);
        static const char* const keys[3] = {"wifi_prio_0", "wifi_prio_1", "wifi_prio_2"};
        for (int i = 0; i < 3; i++) {
            QAction* a = prio->addAction(Q(keys[i]));
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
    options->addSeparator();
    {
        // writing back is off until switched on on purpose
        QAction* a = options->addAction(Q("opt_write"));
        a->setCheckable(true);
        write_back = settings->value(QStringLiteral("write_back"), true).toBool();
        a->setChecked(write_back);
        connect(a, &QAction::toggled, this, [this](bool on) {
            write_back = on;
            settings->setValue(QStringLiteral("write_back"), on);
        });
    }
    add_option("opt_write_ask", "write_ask", [](bool on) { write_ask = on; });
    {
        // while playing: every save of the game goes to the 3DS too (needs writing back on)
        QAction* a = options->addAction(Q("opt_write_live"));
        a->setCheckable(true);
        write_live = settings->value(QStringLiteral("write_live"), true).toBool();
        a->setChecked(write_live);
        connect(a, &QAction::toggled, this, [this](bool on) {
            write_live = on;
            settings->setValue(QStringLiteral("write_live"), on);
        });
    }
    QMenu* save_menu = options->addMenu(Q("opt_save"));
    QActionGroup* save_group = new QActionGroup(save_menu);
    save_mode = settings->value(QStringLiteral("save_mode"), 1).toInt();
    int mode = 0;
    for (const char* key : {"opt_save_ask", "opt_save_always", "opt_save_never"}) {
        QAction* a = save_menu->addAction(Q(key));
        a->setCheckable(true);
        a->setChecked(mode == save_mode);
        save_group->addAction(a);
        connect(a, &QAction::triggered, this, [this, mode] {
            save_mode = mode;
            settings->setValue(QStringLiteral("save_mode"), mode);
        });
        mode++;
    }
    {
        // daily backups: how many days keep their first backup
        backup_days = settings->value(QStringLiteral("backup_days"), 14).toInt();
        QAction* a = options->addAction(Q("opt_backup_days").replace(
            QStringLiteral("%1"), QString::number(backup_days)));
        connect(a, &QAction::triggered, this, [this, a] {
            bool ok = false;
            const int days = QInputDialog::getInt(window, Q("opt_backup_days_title"),
                                                  Q("opt_backup_days_ask"), backup_days, 0,
                                                  3650, 1, &ok);
            if (ok) {
                backup_days = days;
                settings->setValue(QStringLiteral("backup_days"), days);
                a->setText(Q("opt_backup_days").replace(QStringLiteral("%1"),
                                                        QString::number(days)));
            }
        });
    }
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
        Get().Connect(host.toStdString());
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
    }
    const u64 program_id = loaded_program_id;
    const u64 extdata_id = loaded_extdata_id;
    loaded_program_id = 0;
    const bool live_session = live_ok;
    live_ok = false;
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
        if (auto* main = qobject_cast<QMainWindow*>(parent)) {
            main->statusBar()->showMessage(message, 10000);
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

    SaveImportResult result;
    QEventLoop loop;
    std::thread worker([&] {
        result = ImportSaveFrom3DS(program_id, extdata_id, [&progress](const std::string& text) {
            QMetaObject::invokeMethod(
                &progress,
                [&progress, label = QString::fromStdString(text)] {
                    progress.setLabelText(label);
                },
                Qt::QueuedConnection);
        });
        QMetaObject::invokeMethod(&loop, "quit", Qt::QueuedConnection);
    });
    loop.exec();
    worker.join();
    QCoreApplication::processEvents(); // drop the last label updates before closing
    progress.close();

    const QString message = QString::fromStdString(result.message);
    switch (result.outcome) {
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
        if (auto* main = qobject_cast<QMainWindow*>(parent)) {
            main->statusBar()->showMessage(message, 10000);
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
    Get().Connect(host.toStdString());
}

void LinkUi::OnDisconnect() {
    Get().Disconnect();
}

void LinkUi::OnAutoConnect(bool on) {
    settings->setValue(QStringLiteral("autoconnect"), on);
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
            window->statusBar()->showMessage(Q("live_done"), 5000);
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
            stop_action->trigger(); // the 3DS asked before switching
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
        queued.append(QString::fromStdString(msg));
    }
    ShowQueued();

    const State state = client.GetState();
    disconnect_action->setEnabled(state != State::Disconnected);
    if (state == State::Disconnected) {
        status_label->setVisible(false);
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
    status_label->setText(text);
    status_label->setVisible(true);
}

} // namespace Link
