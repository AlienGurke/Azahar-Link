// Copyright Azahar-Link contributors
// Licensed under GPLv3 or any later version
// Refer to the LICENSE.txt file included.

#include <algorithm>
#include <cmath>
#include <QDateTime>
#include <QFontMetrics>
#include <QPainter>
#include "citra_qt/link/link_osd.h"

namespace Link {

namespace {
constexpr int kMargin = 10;  // from the edges of the picture
constexpr int kPadding = 6;  // inside the box
constexpr int kMessageMs = 5000;

// native, so it lies on top of the render widget (itself a native window: OpenGL/Vulkan)
void MakeOverlay(QWidget* w) {
    w->setAttribute(Qt::WA_NativeWindow);
    w->setAttribute(Qt::WA_TransparentForMouseEvents);
    w->setAttribute(Qt::WA_ShowWithoutActivating);
    w->setFocusPolicy(Qt::NoFocus);
    w->hide();
}
} // namespace

OsdBox::OsdBox(QWidget* parent) : QWidget(parent) {
    MakeOverlay(this);
    setAutoFillBackground(false);
    QFont f = font();
    f.setPointSizeF(std::max(9.0, f.pointSizeF() * 1.1));
    f.setBold(true);
    setFont(f);
}

void OsdBox::SetLines(const QStringList& new_lines, QColor new_color) {
    if (new_lines == lines && new_color == color) {
        return;
    }
    lines = new_lines;
    color = new_color;
    const QFontMetrics fm(font());
    int w = 0;
    for (const QString& l : lines) {
        w = std::max(w, fm.horizontalAdvance(l));
    }
    resize(w + 2 * kPadding, static_cast<int>(lines.size()) * fm.height() + 2 * kPadding);
    update();
}

void OsdBox::paintEvent(QPaintEvent*) {
    QPainter p(this);
    p.fillRect(rect(), Qt::black); // black: nothing lit on an OLED around the text
    p.setPen(color);
    const QFontMetrics fm(font());
    int y = kPadding + fm.ascent();
    for (const QString& l : lines) {
        p.drawText(kPadding, y, l);
        y += fm.height();
    }
}

Osd::Osd(QWidget* render_) : render(render_) {
    status = new OsdBox(render);
    messages = new OsdBox(render);
    black = new QWidget(render);
    MakeOverlay(black);
    black->setAutoFillBackground(true);
    QPalette pal = black->palette();
    pal.setColor(QPalette::Window, Qt::black);
    black->setPalette(pal);
}

Osd::~Osd() = default; // the boxes belong to the render widget

void Osd::SetStatus(const QStringList& lines, int mode) {
    status_mode = lines.isEmpty() ? 0 : mode;
    // much dimmer when it moves (OLED), as in melonDS-Link
    status->SetLines(lines, mode >= 2 ? QColor(0x78, 0x78, 0x78) : QColor(0xD0, 0xD0, 0xD0));
}

void Osd::AddMessage(const QString& text) {
    shown.append({text, QDateTime::currentMSecsSinceEpoch() + kMessageMs});
    while (shown.size() > 4) {
        shown.removeFirst();
    }
    Update();
}

void Osd::SetBlackout(bool on) {
    if (on != black->isVisible()) {
        black->setVisible(on);
    }
    Update();
}

void Osd::Update() {
    const bool visible = render->isVisible();
    const int w = render->width(), h = render->height();
    if (black->isVisible()) {
        black->setGeometry(0, 0, w, h);
        black->raise();
    }

    // messages, top left, the newest at the bottom
    const qint64 now = QDateTime::currentMSecsSinceEpoch();
    shown.erase(std::remove_if(shown.begin(), shown.end(),
                               [now](const Message& m) { return m.until <= now; }),
                shown.end());
    QStringList lines;
    for (const Message& m : shown) {
        lines.append(m.text);
    }
    messages->SetLines(lines, QColor(0xF0, 0xF0, 0xF0));
    const bool show_messages = visible && !lines.isEmpty() && !black->isVisible();
    if (show_messages) {
        messages->move(kMargin, kMargin);
        messages->raise();
    }
    messages->setVisible(show_messages);

    // status, top right; moving slowly if wanted (no pixel of an OLED shows the same for long)
    const bool show_status = visible && status_mode != 0 && !black->isVisible();
    if (show_status) {
        int dx = 0, dy = 0;
        const double t = now / 1000.0;
        if (status_mode == 3) {
            // a bigger area: down to 6 times its height, 60 pixels to the side, zigzag
            const int range_y = std::max(0, std::min(status->height() * 6, h / 3 - status->height()));
            const int range_x = 60;
            const auto zigzag = [](double t_, double period) {
                const double p = std::fmod(t_, period) / period;
                return p < 0.5 ? p * 2.0 : (1.0 - p) * 2.0;
            };
            const double period_y = std::max(60.0, 2.0 * range_y / 0.2);
            dy = static_cast<int>(zigzag(t, period_y) * range_y);
            dx = static_cast<int>(zigzag(t + 371.0, 2.0 * range_x / 0.1 * 1.0937) * range_x);
        } else if (status_mode == 2) {
            dx = static_cast<int>(30.0 * (0.5 - 0.5 * std::cos(t * 6.283185307 / 613.0)));
            dy = static_cast<int>(20.0 * (0.5 - 0.5 * std::cos(t * 6.283185307 / 457.0)));
        }
        status->move(w - kMargin - status->width() - dx, kMargin + dy);
        status->raise();
    }
    status->setVisible(show_status);
}

} // namespace Link
