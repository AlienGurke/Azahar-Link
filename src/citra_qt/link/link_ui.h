// Copyright Azahar-Link contributors
// Licensed under GPLv3 or any later version
// Refer to the LICENSE.txt file included.

// 3DS Link in Azahar's main window: the "3DS Link" menu, a status bar entry
// and the messages of the client. Its settings live in their own file
// (3ds-link.ini in Azahar's config folder), so Azahar's config stays untouched.

#pragma once

#include <QObject>
#include <QStringList>

class QAction;
class QLabel;
class QMainWindow;
class QSettings;
class QTimer;

namespace Link {

class LinkUi : public QObject {
    Q_OBJECT

public:
    explicit LinkUi(QMainWindow* window);
    ~LinkUi() override;

private:
    void OnConnect();
    void OnDisconnect();
    void OnAutoConnect(bool on);
    void Poll();

    QMainWindow* window;
    QSettings* settings;
    QAction* connect_action;
    QAction* disconnect_action;
    QAction* auto_action;
    QLabel* status_label;
    QTimer* timer;
    bool game_running = false;
    // messages are shown one after the other, each for a few seconds
    QStringList queued;
    qint64 shown_until = 0;
    void ShowQueued();
};

} // namespace Link
