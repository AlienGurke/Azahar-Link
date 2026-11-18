// Copyright Azahar-Link contributors
// Licensed under GPLv3 or any later version
// Refer to the LICENSE.txt file included.

#include <QAction>
#include <QInputDialog>
#include <QLabel>
#include <QMainWindow>
#include <QMenu>
#include <QMenuBar>
#include <QSettings>
#include <QStatusBar>
#include <QTimer>

#include "citra_qt/link/link_client.h"
#include "citra_qt/link/link_ui.h"
#include "common/file_util.h"
#include "core/core.h"
#include "core/loader/loader.h"

namespace Link {

namespace {
QString Q(const char* key) {
    return QString::fromStdString(Text(key));
}
} // namespace

LinkUi::LinkUi(QMainWindow* window_) : QObject(window_), window(window_) {
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
    menu->addSeparator();
    auto_action = menu->addAction(Q("autoconnect"));
    auto_action->setCheckable(true);
    auto_action->setChecked(settings->value(QStringLiteral("autoconnect"), false).toBool());
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

    std::string msg;
    while (client.PopMessage(msg)) {
        window->statusBar()->showMessage(QString::fromStdString(msg), 5000);
    }

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
