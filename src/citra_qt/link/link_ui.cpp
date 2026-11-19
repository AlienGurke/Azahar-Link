// Copyright Azahar-Link contributors
// Licensed under GPLv3 or any later version
// Refer to the LICENSE.txt file included.

#include <thread>
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

#include "citra_qt/link/link_client.h"
#include "citra_qt/link/link_save.h"
#include "citra_qt/link/link_ui.h"
#include <ctime>
#include <QDateTime>
#include <fmt/format.h>
#include "common/file_util.h"
#include "common/settings.h"
#include "core/core.h"
#include "core/loader/loader.h"

namespace Link {

namespace {
QString Q(const char* key) {
    return QString::fromStdString(Text(key));
}
} // namespace

LinkUi::LinkUi(QMainWindow* window_) : QObject(window_), window(window_) {
    instance = this;
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

    // settings: what happens on connect and when a game starts (all on by default)
    QMenu* options = menu->addMenu(Q("settings"));
    const auto add_option = [&](const char* key, const char* name, auto apply) {
        QAction* a = options->addAction(Q(key));
        a->setCheckable(true);
        const bool on = settings->value(QString::fromLatin1(name), true).toBool();
        a->setChecked(on);
        apply(on);
        connect(a, &QAction::toggled, this, [this, name, apply](bool value) {
            settings->setValue(QString::fromLatin1(name), value);
            apply(value);
        });
    };
    add_option("opt_clock", "clock", [this](bool on) { option_clock = on; });
    add_option("opt_miis", "miis", [](bool on) { Get().option_miis = on; });
    add_option("opt_sys", "systemdata", [](bool on) { Get().option_system_data = on; });
    options->addSeparator();
    {
        // writing back is off until switched on on purpose
        QAction* a = options->addAction(Q("opt_write"));
        a->setCheckable(true);
        write_back = settings->value(QStringLiteral("write_back"), false).toBool();
        a->setChecked(write_back);
        connect(a, &QAction::toggled, this, [this](bool on) {
            write_back = on;
            settings->setValue(QStringLiteral("write_back"), on);
        });
    }
    add_option("opt_write_ask", "write_ask", [](bool on) { write_ask = on; });
    QMenu* save_menu = options->addMenu(Q("opt_save"));
    QActionGroup* save_group = new QActionGroup(save_menu);
    save_mode = settings->value(QStringLiteral("save_mode"), 0).toInt();
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
    // before the renderer and the HID service go away
    Get().Disconnect();
}

int LinkUi::save_mode = 0;
LinkUi* LinkUi::instance = nullptr;
bool LinkUi::write_back = false;
bool LinkUi::write_ask = true;
u64 LinkUi::loaded_program_id = 0;
u64 LinkUi::loaded_extdata_id = 0;

void LinkUi::AfterShutdown(QWidget* parent) {
    const u64 program_id = loaded_program_id;
    const u64 extdata_id = loaded_extdata_id;
    loaded_program_id = 0;
    if (program_id == 0 || !write_back) {
        return;
    }
    if (Get().GetState() != State::Connected) {
        QMessageBox::warning(parent, Q("write_title"), Q("write_no_3ds"));
        return;
    }
    if (write_ask && QMessageBox::question(parent, Q("write_title"), Q("write_ask")) !=
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
        result = ExportSaveTo3DS(program_id, extdata_id, [&progress](const std::string& text) {
            QMetaObject::invokeMethod(
                &progress,
                [&progress, label = QString::fromStdString(text)] { progress.setLabelText(label); },
                Qt::QueuedConnection);
        });
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

void LinkUi::Poll() {
    Client& client = Get();

    // the running game, for the 3DS app
    Core::System& system = Core::System::GetInstance();
    const bool running = system.IsPoweredOn();
    if (running != game_running) {
        game_running = running;
        std::string title;
        if (running) {
            system.GetAppLoader().ReadTitle(title, true);
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
        const bool clock_changes =
            Settings::values.init_clock.GetValue() != Settings::InitClock::SystemTime ||
            std::abs(Settings::values.init_time_offset.GetValue() - offset) > 2;
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
