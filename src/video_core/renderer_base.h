// Copyright 2014-2026 Citra Emulator Project / Azahar Emulator Project
// Licensed under GPLv3 or any later version
// Refer to the LICENSE.txt file included.

#pragma once

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <mutex>
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
};

LinkCapture& GetLinkCapture();

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
