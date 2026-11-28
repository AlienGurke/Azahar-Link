// Play time on this PC - see link_activity.h

#include <chrono>
#include <cstring>
#include <QCoreApplication>
#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QRandomGenerator>
#include <QStandardPaths>
#include <QSysInfo>
#include <QTextStream>
#include <QTimer>
#include "citra_qt/link/link_activity.h"
#include "citra_qt/link/link_client.h"

namespace Link::Activity {

namespace {

constexpr char kMagic[8] = {'3', 'D', 'S', 'L', 'P', 'C', 'P', 'L'};
constexpr u32 kVersion = 1;
constexpr u32 kEntrySize = 24;
constexpr u16 kFlagWith3ds = 1;  // a 3DS was connected at some point of the session
constexpr u16 kFlagClosed = 2;   // the session ended normally (not cut off by a crash)

using Clock = std::chrono::steady_clock;

bool g_enabled = true;
bool g_open = false;
u64 g_tid = 0;
qint64 g_offset = -1;          // of the open session's entry in the file
u32 g_seconds = 0;             // counted before the current run
bool g_running = false;
Clock::time_point g_run_start;
u16 g_flags = 0;
QTimer* g_timer = nullptr;

QString FileName() {
    return QString::fromStdString(Folder()) + QStringLiteral("/azahar.bin");
}

/// local time (as the PC shows it) in seconds since 1.1.2000 - like the 3DS counts
u32 LocalSeconds() {
    const QDateTime now = QDateTime::currentDateTime();
    const qint64 local = now.toSecsSinceEpoch() + now.offsetFromUtc();
    return static_cast<u32>(local - 946684800);
}

/// pc.txt: this PC's random id (made once) and a name that can be changed
void EnsurePcFile() {
    const QString path = QString::fromStdString(Folder()) + QStringLiteral("/pc.txt");
    if (QFile::exists(path)) {
        return;
    }
    QFile f(path);
    if (!f.open(QIODevice::WriteOnly | QIODevice::Text)) {
        return;
    }
    const quint64 id = QRandomGenerator::system()->generate64();
    QTextStream out(&f);
    out << "# this PC for Dashboard-Link / 3DS Link - the name can be changed, the id not\n";
    out << "id=" << QStringLiteral("%1").arg(id, 16, 16, QLatin1Char('0')).toUpper() << "\n";
    out << "name=" << QSysInfo::machineHostName() << "\n";
}

/// titles.txt: "title id<TAB>name" of every game played here (for names without the 3DS)
void NoteTitle(u64 tid, const std::string& name) {
    if (tid == 0 || name.empty()) {
        return;
    }
    const QString path = QString::fromStdString(Folder()) + QStringLiteral("/titles.txt");
    const QString key = QStringLiteral("%1").arg(tid, 16, 16, QLatin1Char('0')).toUpper();
    QFile f(path);
    if (f.open(QIODevice::ReadOnly | QIODevice::Text)) {
        while (!f.atEnd()) {
            if (QString::fromUtf8(f.readLine()).startsWith(key + QLatin1Char('\t'))) {
                return;
            }
        }
        f.close();
    }
    if (f.open(QIODevice::Append | QIODevice::Text)) {
        f.write((key + QLatin1Char('\t') + QString::fromStdString(name) + QLatin1Char('\n')).toUtf8());
    }
}

u32 SecondsNow() {
    u32 s = g_seconds;
    if (g_running) {
        s += static_cast<u32>(
            std::chrono::duration_cast<std::chrono::seconds>(Clock::now() - g_run_start).count());
    }
    return s;
}

/// writes the open session's time and flags into its entry
void Update(bool closing) {
    if (!g_open || g_offset < 0) {
        return;
    }
    if (Get().GetState() == State::Connected) {
        g_flags |= kFlagWith3ds;
    }
    if (closing) {
        g_flags |= kFlagClosed;
    }
    QFile f(FileName());
    if (!f.open(QIODevice::ReadWrite)) {
        return;
    }
    const u32 s = SecondsNow();
    f.seek(g_offset + 12);
    f.write(reinterpret_cast<const char*>(&s), 4);
    f.seek(g_offset + 20);
    f.write(reinterpret_cast<const char*>(&g_flags), 2);
}

/// a new entry at the end of the file (a cut-off last entry is overwritten)
bool Append(u64 tid) {
    QFile f(FileName());
    if (!f.open(QIODevice::ReadWrite)) {
        return false;
    }
    char head[16];
    if (f.size() < 16 || f.read(head, 16) != 16 || std::memcmp(head, kMagic, 8) != 0) {
        if (f.size() >= 16) {
            return false; // not a file of ours: never touched
        }
        f.resize(0);
        f.write(kMagic, 8);
        f.write(reinterpret_cast<const char*>(&kVersion), 4);
        f.write(reinterpret_cast<const char*>(&kEntrySize), 4);
    }
    const qint64 count = (f.size() - 16) / kEntrySize;
    g_offset = 16 + count * kEntrySize;
    char e[kEntrySize] = {};
    const u32 start = LocalSeconds();
    std::memcpy(e, &tid, 8);
    std::memcpy(e + 8, &start, 4);
    std::memcpy(e + 20, &g_flags, 2);
    f.seek(g_offset);
    return f.write(e, kEntrySize) == kEntrySize && f.resize(g_offset + kEntrySize);
}

void EnsureTimer() {
    if (g_timer) {
        return;
    }
    g_timer = new QTimer(QCoreApplication::instance());
    g_timer->setInterval(30000);
    QObject::connect(g_timer, &QTimer::timeout, [] { Update(false); });
}

} // namespace

void SetEnabled(bool on) {
    if (!on && g_open) {
        End();
    }
    g_enabled = on;
}

bool Enabled() {
    return g_enabled;
}

std::string Folder() {
#ifdef _WIN32
    QString base = qEnvironmentVariable("APPDATA");
#else
    QString base = QStandardPaths::writableLocation(QStandardPaths::GenericDataLocation);
#endif
    const QString dir = base + QStringLiteral("/3DS-Link/activity");
    QDir().mkpath(dir);
    return dir.toStdString();
}

void Resume(u64 title_id, const std::string& name) {
    if (!g_enabled || title_id == 0) {
        return;
    }
    if (g_open && g_tid != title_id) {
        End();
    }
    if (!g_open) {
        EnsurePcFile();
        NoteTitle(title_id, name);
        g_flags = Get().GetState() == State::Connected ? kFlagWith3ds : 0;
        g_seconds = 0;
        g_tid = title_id;
        if (!Append(title_id)) {
            return;
        }
        g_open = true;
    }
    if (!g_running) {
        g_running = true;
        g_run_start = Clock::now();
    }
    EnsureTimer();
    g_timer->start();
    Update(false);
}

void Pause() {
    if (!g_open || !g_running) {
        return;
    }
    g_seconds = SecondsNow();
    g_running = false;
    Update(false);
    if (g_timer) {
        g_timer->stop();
    }
}

void End() {
    if (!g_open) {
        return;
    }
    g_seconds = SecondsNow();
    g_running = false;
    Update(true);
    g_open = false;
    g_offset = -1;
    if (g_timer) {
        g_timer->stop();
    }
}

} // namespace Link::Activity
