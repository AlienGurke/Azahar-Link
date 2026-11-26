// Copyright Azahar-Link contributors
// Licensed under GPLv3 or any later version
// Refer to the LICENSE.txt file included.

// Text on the game picture, as melonDS-Link has it: the 3DS battery and WiFi in the top
// right corner (fixed, or slowly moving - kind to OLED screens), messages in the top left,
// and an all black screen while the 3DS is closed. Small native windows on top of
// Azahar's render widget (a picture with frame generation or a capture then has them too).

#pragma once

#include <QStringList>
#include <QWidget>

namespace Link {

/// A black box with a few lines of text.
class OsdBox : public QWidget {
public:
    explicit OsdBox(QWidget* parent);
    /// Lines, their colour; the box takes the size it needs.
    void SetLines(const QStringList& lines, QColor color);
    QStringList Lines() const {
        return lines;
    }

protected:
    void paintEvent(QPaintEvent* event) override;

private:
    QStringList lines;
    QColor color;
};

/// The three on one render widget: status, messages, black.
class Osd {
public:
    /// `render`: Azahar's render window (the boxes are its children).
    explicit Osd(QWidget* render);
    ~Osd();

    /// Status lines (empty = none). mode 1 fixed, 2 moving a little, 3 wandering over a
    /// bigger area (as in melonDS-Link).
    void SetStatus(const QStringList& lines, int mode);
    /// A message for a few seconds (several stack up).
    void AddMessage(const QString& text);
    /// All black (the 3DS is closed and the game paused).
    void SetBlackout(bool on);
    /// Places the boxes again and keeps them above the picture (call it often: the render
    /// widget is made anew for every game).
    void Update();

private:
    QWidget* render;
    OsdBox* status;
    OsdBox* messages;
    QWidget* black;
    int status_mode = 0;
    struct Message {
        QString text;
        qint64 until;
    };
    QList<Message> shown;
};

} // namespace Link
