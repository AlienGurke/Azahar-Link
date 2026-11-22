// Copyright 2014-2026 Citra Emulator Project / Azahar Emulator Project
// Licensed under GPLv3 or any later version
// Refer to the LICENSE.txt file included.

#pragma once

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <mutex>
#include <string>
#include <vector>
#include "common/common_types.h"
#include "core/frontend/framebuffer_layout.h"
#include "video_core/rasterizer_interface.h"

namespace Frontend {
class EmuWindow;
}

namespace Core {
class System;
}

namespace VideoCore {

enum class ScreenId : u32 {
    TopLeft,
    TopRight,
    Bottom,
};

/**
 * 3DS Link: the bottom screen at its native size for the 3DS app, which shows it on the
 * real console. While enabled, the renderer draws it into a small extra target after each
 * presented frame and reads it back without waiting for the GPU (it arrives a frame or two
 * later). The Link client takes the newest picture from its own thread.
 */
class LinkCapture {
public:
    static constexpr u32 Width = 320;
    static constexpr u32 Height = 240;

    void SetEnabled(bool on) {
        enabled = on;
    }
    [[nodiscard]] bool IsEnabled() const {
        return enabled;
    }

    /// The layout the renderers draw the picture with (based on the window's layout, so
    /// the 3D mode and the like stay the same).
    static Layout::FramebufferLayout MakeLayout(const Layout::FramebufferLayout& base);

    /// Renderer thread: hands over a picture, 4 bytes per pixel.
    /// bgra: bytes B,G,R,A (else R,G,B,A). bottom_up: the first row is the bottom one.
    /// frame: the emulated frame it shows (older ones than the newest handed over are ignored).
    void Submit(const void* pixels, bool bgra, bool bottom_up, u64 frame);

    /// Client thread: waits up to `timeout` for a picture newer than `seq`. On success
    /// `rgb565` gets Width * Height pixels (rows top to bottom) and `seq` is updated.
    /// `frame` (if given) gets the emulated frame the picture shows.
    bool Take(std::vector<u16>& rgb565, u64& seq, std::chrono::milliseconds timeout,
              u64* frame = nullptr);

private:
    std::atomic_bool enabled{false};
    std::mutex mutex;
    std::condition_variable cv;
    std::vector<u16> latest;
    u64 latest_seq = 0;
    u64 latest_frame = 0;

public:
    /// Frames not read back because the GPU was still busy with earlier copies (for the log).
    std::atomic<u64> skipped{0};
};

LinkCapture& GetLinkCapture();

/**
 * 3DS Link: how evenly Azahar itself runs (only for the log). Every emulated refresh
 * (VBlank, ~59.83 a second) with the time it took the renderer, the games' buffer swaps,
 * and every picture handed to the window (or left out as a copy).
 */
class LinkPacing {
public:
    static constexpr double FrameMs = 1000.0 * 4481136.0 / 268111856.0; // 16.713 ms

    struct Stats {
        double seconds = 0;
        u64 vblanks = 0, top_swaps = 0, bottom_swaps = 0, presents = 0, copies_left_out = 0;
        // time between refreshes (the frame limiter's aim: FrameMs)
        double gap_max_ms = 0;
        u64 gaps_over_20 = 0, gaps_over_25 = 0, gaps_over_34 = 0, gaps_under_12 = 0;
        // time the renderer took for a refresh (drawing, waiting for the GPU) - without the
        // frame limiter's wait, which is counted on its own
        double swap_avg_ms = 0, swap_max_ms = 0;
        u64 swaps_over_8 = 0;
        double limiter_avg_ms = 0;
        // the emulation between two refreshes (game code, its drawing commands)
        double emu_avg_ms = 0, emu_max_ms = 0;
        // new shaders and pipelines (each one compiled the first time it is needed)
        u64 shaders = 0, pipelines = 0;
        double shader_ms = 0, pipeline_ms = 0;
        // the game's calls to system services that took over 1 ms, and file accesses on the
        // PC's disk over 0.5 ms (both hold up the emulation where they run on its thread)
        u64 slow_calls = 0, slow_files = 0;
        double slow_call_ms = 0, slow_file_ms = 0, slowest_call_ms = 0;
        std::string slowest_call;
        // speed in each whole second (100 = like the 3DS)
        double speed_min = 0, speed_max = 0;
        // pictures to the window: the longest gap, gaps off the refresh grid by over 4 ms,
        // and changes of rhythm (e.g. 2,2,2,3 refreshes apart)
        double present_gap_max_ms = 0;
        u64 present_off_grid = 0, present_rhythm_changes = 0;
        u64 present_apart[4] = {}; // 1, 2, 3, 4+ refreshes apart
    };

    void VBlank(double swap_ms);
    void Limiter(double ms);
    void Compiled(bool pipeline, double ms, bool count = true);
    void ServiceCall(const std::string& name, double ms);
    void FileAccess(double ms);
    void Swap(bool top);
    void Present(bool shown);
    Stats Take();
    /// Lines about single refreshes that took long (at most 30 since the last call).
    std::vector<std::string> TakeHitches();

private:
    using Clock = std::chrono::steady_clock;
    std::mutex mutex;
    Stats cur;
    double swap_sum = 0, limiter_sum = 0, emu_sum = 0;
    double limiter_cur = 0; // of the refresh going on
    u64 f_shaders = 0, f_pipelines = 0;
    double f_shader_ms = 0, f_pipeline_ms = 0;
    u64 f_calls = 0, f_files = 0;
    double f_call_ms = 0, f_file_ms = 0, f_slowest_ms = 0;
    std::string f_slowest;
    std::vector<std::string> hitches;
    u64 hitches_dropped = 0;
    Clock::time_point since{}, last_vblank{}, last_present{}, second_start{};
    u64 second_vblanks = 0;
    int last_k = 0;
};

LinkPacing& GetLinkPacing();

struct RendererSettings {
    // Screenshot
    std::atomic_bool screenshot_requested{false};
    void* screenshot_bits{};
    std::function<void(bool)> screenshot_complete_callback;
    Layout::FramebufferLayout screenshot_framebuffer_layout;
    // Renderer
    std::atomic_bool bg_color_update_requested{false};
    std::atomic_bool shader_update_requested{false};
};

class RendererBase : NonCopyable {
public:
    explicit RendererBase(Core::System& system, Frontend::EmuWindow& window,
                          Frontend::EmuWindow* secondary_window);
    virtual ~RendererBase();

    /// Returns the rasterizer owned by the renderer
    virtual VideoCore::RasterizerInterface* Rasterizer() = 0;

    /// Finalize rendering the guest frame and draw into the presentation texture
    virtual void SwapBuffers() = 0;

    /// Draws the latest frame to the window waiting timeout_ms for a frame to arrive (Renderer
    /// specific implementation)
    virtual void TryPresent(int timeout_ms, bool is_secondary) = 0;
    virtual void TryPresent(int timeout_ms) {
        TryPresent(timeout_ms, false);
    }

    /// Prepares for video dumping (e.g. create necessary buffers, etc)
    virtual void PrepareVideoDumping() {}

    /// Cleans up after video dumping is ended
    virtual void CleanupVideoDumping() {}

    /// This is called to notify the rendering backend of a surface change
    // if second == true then it is the second screen
    virtual void NotifySurfaceChanged(bool second) {}

    /// Returns the resolution scale factor relative to the native 3DS screen resolution
    u32 GetResolutionScaleFactor();

    /// Returns the MSAA sample count
    u8 GetSampleCount() const;

    /// Updates the framebuffer layout of the contained render window handle.
    void UpdateCurrentFramebufferLayout(bool is_portrait_mode = {});

    /// Ends the current frame
    void EndFrame();

    f32 GetCurrentFPS() const {
        return current_fps;
    }

    u64 GetCurrentFrame() const {
        return current_frame;
    }

    Frontend::EmuWindow& GetRenderWindow() {
        return render_window;
    }

    const Frontend::EmuWindow& GetRenderWindow() const {
        return render_window;
    }

    [[nodiscard]] RendererSettings& Settings() {
        return settings;
    }

    [[nodiscard]] const RendererSettings& Settings() const {
        return settings;
    }

    /// Returns true if a screenshot is being processed
    [[nodiscard]] bool IsScreenshotPending() const;

    /// Request a screenshot of the next frame
    void RequestScreenshot(void* data, std::function<void(bool)> callback,
                           const Layout::FramebufferLayout& layout);

protected:
    /// Waits until the GPU is at most one game frame behind the emulation.
    /// Called on VBlanks where the guest application submits a frame.
    virtual void LimitFramesAheadOfGpu() {}

    Core::System& system;
    RendererSettings settings;
    Frontend::EmuWindow& render_window;    /// Reference to the render window handle.
    Frontend::EmuWindow* secondary_window; /// Reference to the secondary render window handle.

protected:
    f32 current_fps = 0.0f; /// Current framerate, should be set by the renderer
    u64 current_frame = 0;  /// Current frame, should be set by the renderer

private:
    u64 limited_game_frames = 0; /// Guest frame count at the last LimitFramesAheadOfGpu call
};

} // namespace VideoCore
