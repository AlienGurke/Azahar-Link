// Copyright 2015-2026 Citra Emulator Project / Azahar Emulator Project
// Licensed under GPLv3 or any later version
// Refer to the LICENSE.txt file included.

#include <algorithm>
#include <cmath>
#include "common/settings.h"
#include "core/core.h"
#include "core/frontend/emu_window.h"
#include "core/hle/service/hid/hid.h"
#include "core/tracer/recorder.h"
#include "video_core/debug_utils/debug_utils.h"
#include "video_core/renderer_base.h"

namespace VideoCore {

LinkCapture& GetLinkCapture() {
    static LinkCapture capture;
    return capture;
}

LinkPacing& GetLinkPacing() {
    static LinkPacing pacing;
    return pacing;
}

void LinkPacing::VBlank(double swap_ms) {
    const auto now = Clock::now();
    std::scoped_lock lock{mutex};
    if (since == Clock::time_point{}) {
        since = second_start = now;
    }
    cur.vblanks++;
    swap_sum += swap_ms;
    cur.swap_max_ms = std::max(cur.swap_max_ms, swap_ms);
    if (swap_ms > 8.0) {
        cur.swaps_over_8++;
    }
    if (last_vblank != Clock::time_point{}) {
        const double gap = std::chrono::duration<double, std::milli>(now - last_vblank).count();
        cur.gap_max_ms = std::max(cur.gap_max_ms, gap);
        cur.gaps_over_20 += gap > 20.0;
        cur.gaps_over_25 += gap > 25.0;
        cur.gaps_over_34 += gap > 34.0;
        cur.gaps_under_12 += gap < 12.0;
    }
    last_vblank = now;
    second_vblanks++;
    const double sec = std::chrono::duration<double>(now - second_start).count();
    if (sec >= 1.0) {
        const double speed = 100.0 * second_vblanks * FrameMs / (sec * 1000.0);
        if (cur.speed_min == 0 || speed < cur.speed_min) {
            cur.speed_min = speed;
        }
        cur.speed_max = std::max(cur.speed_max, speed);
        second_start = now;
        second_vblanks = 0;
    }
}

void LinkPacing::Swap(bool top) {
    std::scoped_lock lock{mutex};
    (top ? cur.top_swaps : cur.bottom_swaps)++;
}

void LinkPacing::Present(bool shown) {
    const auto now = Clock::now();
    std::scoped_lock lock{mutex};
    if (!shown) {
        cur.copies_left_out++;
        return;
    }
    cur.presents++;
    if (last_present != Clock::time_point{}) {
        const double gap = std::chrono::duration<double, std::milli>(now - last_present).count();
        cur.present_gap_max_ms = std::max(cur.present_gap_max_ms, gap);
        const int k = std::max(1, static_cast<int>(gap / FrameMs + 0.5));
        if (std::abs(gap - k * FrameMs) > 4.0) {
            cur.present_off_grid++;
        }
        cur.present_apart[std::min(k, 4) - 1]++;
        if (last_k != 0 && k != last_k && gap < 200.0) {
            cur.present_rhythm_changes++;
        }
        last_k = gap < 200.0 ? k : 0;
    }
    last_present = now;
}

LinkPacing::Stats LinkPacing::Take() {
    const auto now = Clock::now();
    std::scoped_lock lock{mutex};
    Stats out = cur;
    if (since != Clock::time_point{}) {
        out.seconds = std::chrono::duration<double>(now - since).count();
    }
    out.swap_avg_ms = cur.vblanks ? swap_sum / cur.vblanks : 0.0;
    cur = Stats{};
    swap_sum = 0;
    since = now;
    return out;
}

Layout::FramebufferLayout LinkCapture::MakeLayout(const Layout::FramebufferLayout& base) {
    Layout::FramebufferLayout layout = base;
    layout.width = Width;
    layout.height = Height;
    layout.top_screen_enabled = false;
    layout.bottom_screen_enabled = true;
    layout.top_screen = {0, 0, 0, 0};
    layout.bottom_screen = {0, 0, Width, Height};
    layout.is_rotated = true;
    layout.additional_screen_enabled = false;
    layout.top_opacity = 1.0f;
    layout.bottom_opacity = 1.0f;
    return layout;
}

void LinkCapture::Submit(const void* pixels, bool bgra, bool bottom_up, u64 frame) {
    const u8* src = static_cast<const u8*>(pixels);
    std::vector<u16> out(Width * Height);
    const u32 r_off = bgra ? 2 : 0;
    const u32 b_off = bgra ? 0 : 2;
    for (u32 y = 0; y < Height; y++) {
        const u8* row = src + (bottom_up ? Height - 1 - y : y) * Width * 4;
        u16* dst = out.data() + y * Width;
        for (u32 x = 0; x < Width; x++) {
            const u8* p = row + x * 4;
            dst[x] = static_cast<u16>(((p[r_off] >> 3) << 11) | ((p[1] >> 2) << 5) | (p[b_off] >> 3));
        }
    }
    {
        std::scoped_lock lock{mutex};
        if (frame < latest_frame && latest_frame - frame < 1000) {
            return; // an older picture than the one handed over already
        }
        latest = std::move(out);
        latest_seq++;
        latest_frame = frame;
    }
    cv.notify_all();
}

bool LinkCapture::Take(std::vector<u16>& rgb565, u64& seq, std::chrono::milliseconds timeout,
                       u64* frame) {
    std::unique_lock lock{mutex};
    if (!cv.wait_for(lock, timeout, [&] { return latest_seq != seq && !latest.empty(); })) {
        return false;
    }
    rgb565 = latest;
    seq = latest_seq;
    if (frame) {
        *frame = latest_frame;
    }
    return true;
}

RendererBase::RendererBase(Core::System& system_, Frontend::EmuWindow& window,
                           Frontend::EmuWindow* secondary_window_)
    : system{system_}, render_window{window}, secondary_window{secondary_window_} {}

RendererBase::~RendererBase() = default;

u32 RendererBase::GetResolutionScaleFactor() {
    const auto graphics_api = Settings::GetWorkingGraphicsAPI();
    if (graphics_api == Settings::GraphicsAPI::Software) {
        // Software renderer always render at native resolution
        return 1;
    }

    const u32 scale_factor = Settings::values.resolution_factor.GetValue();
    return scale_factor != 0 ? scale_factor
                             : render_window.GetFramebufferLayout().GetScalingRatio();
}

u8 RendererBase::GetSampleCount() const {
    return Settings::GetAntiAliasingSampleCount(Settings::values.antialiasing.GetValue());
}

void RendererBase::UpdateCurrentFramebufferLayout(bool is_portrait_mode) {
    const auto update_layout = [is_portrait_mode](Frontend::EmuWindow& window) {
        const Layout::FramebufferLayout& layout = window.GetFramebufferLayout();
        window.UpdateCurrentFramebufferLayout(layout.width, layout.height, is_portrait_mode);
    };
    update_layout(render_window);
    if (secondary_window != nullptr) {
        update_layout(*secondary_window);
    }
}

void RendererBase::EndFrame() {
    current_frame++;

    system.perf_stats->EndSystemFrame();

    render_window.PollEvents();

    // Wait for the GPU to finish and to be at most one game frame behind.
    // Below 60 FPS a guest frame spans several VBlanks, and its GPU work may still run while the
    // next one is emulated.
    const u64 game_frames = system.perf_stats->GetGameFrameCount();
    if (game_frames != limited_game_frames) {
        limited_game_frames = game_frames;
        if (Settings::values.use_limit_frames_ahead_of_gpu.GetValue()) {
            LimitFramesAheadOfGpu();
        }
    }

    system.frame_limiter.DoFrameLimiting(system.CoreTiming().GetGlobalTimeUs());

    // On fast enough devices, the time limiter sleep spans most of the 16ms period
    // between VBlank events. If an input event lands in that period, then the change
    // will not reflect in HID until the next HID event. For applications that only
    // check the input just after VBlank, it will cause them to lose the event until
    // the next frame. Sacrifice HID timing accuracy for input latency by triggering
    // the event just after VBlank (frame limiting behaviour is not accurate anyways).
    if (const auto hid = Service::HID::GetModule(system)) {
        hid->RestartPadUpdates();
    }

    system.perf_stats->BeginSystemFrame();
}

bool RendererBase::IsScreenshotPending() const {
    return settings.screenshot_requested;
}

void RendererBase::RequestScreenshot(void* data, std::function<void(bool)> callback,
                                     const Layout::FramebufferLayout& layout) {
    if (settings.screenshot_requested) {
        LOG_ERROR(Render, "A screenshot is already requested or in progress, ignoring the request");
        return;
    }
    settings.screenshot_bits = data;
    settings.screenshot_complete_callback = callback;
    settings.screenshot_framebuffer_layout = layout;
    settings.screenshot_requested = true;
}
} // namespace VideoCore
