#pragma once

/*
 * GPU compositing over the VirtIO-GPU 3D (virgl) render node.
 *
 * The CPU compositor (WM_Compositor.c) repaints every damaged pixel of the
 * desktop -- background, every window and its shadow under it, the shell on
 * top -- into one shadow buffer and copies that to the framebuffer. This
 * module instead keeps each piece in its own texture on the host GPU and
 * lets the GPU blend them:
 *
 *   background   the wallpaper and desktop icons, uploaded when regenerated;
 *   one layer per window, holding its frame, shadow and client pixels at
 *                rest (scale 1, opacity 1), re-rasterized only where the
 *                client drew or when the decoration itself changed;
 *   overlay      the taskbar, Start menu, notifications, Wi-Fi panel and
 *                dialogs, re-rasterized in damaged areas;
 *   cursor       the pointer sprite, re-rasterized when its shape changes.
 *
 * Moving, raising, fading or zooming a window therefore costs no CPU
 * rasterization and no upload: only a new command stream of textured quads,
 * drawn into a render target that is itself on scanout.
 *
 * Everything is optional. When the render node, the 3D backend or the
 * kernel's scanout command is missing, or any GPU call fails at run time,
 * the window manager stays on (or returns to) the CPU path.
 */

#include "../Core/WM_State.h"

/* Opens the render node and sets up the pipeline and the render target for
 * the current framebuffer size. false: keep compositing on the CPU. */
bool wm_gpu_init(wm_state_t *state);
bool wm_gpu_active(void);
/* The desktop size changed (after wm_compositor_resize()). On failure the
 * GPU path shuts itself down. */
void wm_gpu_resize(wm_state_t *state);
/* The CPU-side background (wallpaper, desktop icons) was redrawn. */
void wm_gpu_background_changed(void);
/* Renders one frame. `rects` is the screen damage (cursor included) and is
 * used to decide what to re-rasterize and what to show; `full` means the
 * whole screen. Returns false when the GPU path failed and has shut itself
 * down -- the caller then repaints the whole screen on the CPU. */
bool wm_gpu_render(wm_state_t *state, const wm_rect_t *rects, uint32_t count,
                   bool full, uint64_t now_ms);
/* Hands the scanout back to the display driver and frees every GPU object. */
void wm_gpu_shutdown(void);
