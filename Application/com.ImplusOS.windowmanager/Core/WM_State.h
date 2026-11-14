#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <time.h>

#include "../../../../Userland/API/Source/IPC.h"
#include "../../../../Userland/API/Source/Input.h"
#include "../../../../Userland/API/Source/Graphics.h"
#include "../../../../Userland/API/Source/WM_Protocol.h"
#include "../../../../Userland/API/Source/WiFi.h"
#include "../../../API/Source/Material.h"

#define WM_MAX_WINDOWS                 128u
#define WM_MAX_INPUT_SUBSCRIPTIONS     32u
#define WM_MAX_NOTIFICATIONS           4u
#define WM_MAX_NOTIFICATION_HISTORY    8u
#define WM_MAX_LAUNCHER_APPS           96u
#define WM_MAX_DAMAGE_RECTS            32u
#define WM_EVENT_QUEUE_SIZE            64u
#define WM_TITLE_MAX                   64u
#define WM_APP_NAME_MAX                48u
#define WM_APP_PATH_MAX                128u
/* Launch argument handed to the app's ELF. Bounded by the kernel's
 * process_t.launch_argument (512 bytes, ProcessScheduler.h): anything longer
 * would be truncated there instead, mid-flag. */
#define WM_APP_ARG_MAX                 256u
#define WM_APP_COMMENT_MAX             96u
#define WM_THEME_PATH_MAX              128u
#define WM_XML_MAX_BYTES               (256u * 1024u)
#define WM_SURFACE_MAX_BYTES           (16u * 1024u * 1024u)

#define WM_TASKBAR_AT_TOP              0

/* Shell metrics, in Material Design 3 terms. The window frame is an M3
 * "large" shape carrying level-3 elevation; its title bar holds a row of
 * 32px icon buttons, which is the size M3 gives an icon button on a
 * pointer-driven surface, at the compact end of the range M3 allows. */
#define WM_TITLE_HEIGHT                32u
#define WM_CORNER_RADIUS               M3_SHAPE_MEDIUM
#define WM_SHADOW_SIZE                 8u
#define WM_DOCK_HEIGHT                 40u
#define WM_DOCK_MARGIN                 0u
#define WM_TITLE_BUTTON_SIZE           26u
#define WM_TITLE_BUTTON_GAP             1u
#define WM_TITLE_BUTTON_MARGIN          4u
#define WM_MIN_WINDOW_WIDTH            160u
#define WM_MIN_WINDOW_HEIGHT           80u
#define WM_CURSOR_WIDTH                14u
#define WM_CURSOR_HEIGHT               22u

#define WM_SEARCH_MAX                  128u

typedef struct {
    int32_t x;
    int32_t y;
    uint32_t w;
    uint32_t h;
} wm_rect_t;

typedef enum {
    WM_DIALOG_INFO = 0,
    WM_DIALOG_WARNING,
    WM_DIALOG_ERROR
} wm_dialog_type_t;

#define WM_DIALOG_TITLE_MAX 64
#define WM_DIALOG_MSG_MAX 256

typedef struct {
    char title[WM_DIALOG_TITLE_MAX];
    char message[WM_DIALOG_MSG_MAX];
    wm_dialog_type_t type;
    bool active;
    bool dragging;
    bool hover_close;
    bool hover_ok;
    int32_t x;
    int32_t y;
    uint32_t w;
    uint32_t h;
    int32_t drag_start_x;
    int32_t drag_start_y;
    int32_t drag_origin_x;
    int32_t drag_origin_y;
    uint64_t start_ms;
} wm_dialog_t;

typedef struct {
    wm_rect_t rects[WM_MAX_DAMAGE_RECTS];
    uint32_t count;
    bool full;
} wm_region_t;

typedef enum {
    WM_LAYER_DESKTOP = 0,
    WM_LAYER_NORMAL,
    WM_LAYER_FLOATING,
    WM_LAYER_PANEL,
    WM_LAYER_OVERLAY,
    WM_LAYER_CURSOR,
    WM_LAYER_COUNT
} wm_layer_t;

typedef enum {
    WM_HIT_NONE = 0,
    WM_HIT_CONTENT,
    WM_HIT_TITLE,
    WM_HIT_CLOSE,
    WM_HIT_MAXIMIZE,
    WM_HIT_MINIMIZE,
    WM_HIT_RESIZE_LEFT,
    WM_HIT_RESIZE_RIGHT,
    WM_HIT_RESIZE_TOP,
    WM_HIT_RESIZE_BOTTOM,
    WM_HIT_RESIZE_TOP_LEFT,
    WM_HIT_RESIZE_TOP_RIGHT,
    WM_HIT_RESIZE_BOTTOM_LEFT,
    WM_HIT_RESIZE_BOTTOM_RIGHT
} wm_hit_zone_t;

typedef enum {
    WM_TRANSITION_NONE = 0,
    WM_TRANSITION_SHOW,
    WM_TRANSITION_CLOSE,
    WM_TRANSITION_HIDE,
    WM_TRANSITION_MINIMIZE,
    WM_TRANSITION_RESTORE
} wm_transition_t;

typedef enum {
    WM_CURSOR_DEFAULT = 0,
    WM_CURSOR_RESIZE_HORIZONTAL,
    WM_CURSOR_RESIZE_VERTICAL,
    WM_CURSOR_RESIZE_DIAGONAL_NW_SE,
    WM_CURSOR_RESIZE_DIAGONAL_NE_SW
} wm_cursor_style_t;

/* The shell's Material Design 3 theme: one seed colour expanded into the
 * full set of M3 colour roles, the M3 type scale, and the handful of
 * metrics that are the shell's own rather than the specification's.
 *
 * Nothing here is a one-off colour. Every surface, outline and piece of
 * text the window manager paints names a role in `color`, so retinting the
 * desktop is a matter of changing `seed` -- which is what the WM_SET_THEME
 * message and the [Material] section of a .theme file both do. */
typedef struct {
    uint32_t seed;                 /* source colour for the tonal palettes */
    bool dark;                     /* which of the two schemes was built   */
    m3_scheme_t color;          /* the M3 colour roles                  */
    m3_typescale_t type;        /* the M3 type scale, in pixels         */
    float type_scale;              /* multiplier applied to that scale     */

    uint32_t title_height;         /* window title bar                     */
    uint32_t corner_radius;        /* window shape token                   */
    uint32_t shadow_size;          /* window elevation spread              */
    uint32_t dock_height;          /* navigation bar (taskbar)             */
} wm_theme_t;

typedef struct {
    uint32_t *pixels;
    uint32_t width;
    uint32_t height;
    uint32_t stride;
    wm_rect_t clip;
} wm_canvas_t;

typedef struct {
    void *impl;
    bool loaded;
} wm_font_t;

typedef struct wm_window {
    uint32_t id;
    int32_t owner_pid;
    wm_rect_t frame;
    wm_rect_t restore_frame;
    uint32_t bg_color;
    char title[WM_TITLE_MAX];

    bool visible;
    bool has_focus;
    bool is_system;
    bool minimized;
    bool maximized;
    bool close_requested;
    /* WM_SET_WINDOW_CLOSE_POLICY: close goes to the owner as
     * WM_CLOSE_REQUESTED, and the owner is never killed for it. */
    bool owner_handles_close;
    /* WM_DESTROY_WINDOW from the owner is under way. */
    bool owner_destroying;
    bool hover_close;
    bool hover_maximize;
    bool hover_minimize;

    wm_layer_t layer;
    struct wm_window *z_prev;
    struct wm_window *z_next;

    uint32_t *surface;
    uint32_t surface_bytes;
    int32_t surface_shared_memory_handle;
    bool surface_opaque;
    wm_region_t damage;
    uint32_t transaction_depth;
    bool transaction_dirty;

    char *xml_buffer;
    uint32_t xml_size;
    uint32_t xml_capacity;

    uint32_t icon[32u * 32u];
    bool has_icon;

    uint8_t *shadow_mask;
    uint32_t shadow_mask_width;
    uint32_t shadow_mask_height;
    uint32_t shadow_mask_capacity;
    uint32_t shadow_cache_frame_width;
    uint32_t shadow_cache_frame_height;
    uint32_t shadow_cache_radius;
    uint32_t shadow_cache_size;
    uint8_t shadow_cache_alpha;

    wm_transition_t transition;
    uint64_t transition_started_ms;
    uint32_t transition_duration_ms;
    float visual_alpha;
    float visual_scale;
    float visual_offset_y;
} wm_window_t;

typedef struct {
    int32_t subscriber_pid;
    uint32_t window_id;
    uint32_t input_types;
} wm_input_subscription_t;

typedef struct {
    wm_window_t *id_table[WM_MAX_WINDOWS + 1u];
    wm_window_t *layer_top[WM_LAYER_COUNT];
    wm_window_t *layer_bottom[WM_LAYER_COUNT];
    uint32_t window_count;
    uint32_t focused_id;
    wm_input_subscription_t input_subs[WM_MAX_INPUT_SUBSCRIPTIONS];
    uint32_t input_sub_count;
    uint32_t cursor_x;
    uint32_t cursor_y;
    bool cursor_visible;
    wm_cursor_style_t cursor_style;
} wm_scene_t;

typedef struct {
    uint32_t *shadow;
    uint32_t *background;
    uint32_t framebuffer_width;
    uint32_t framebuffer_height;
    uint32_t framebuffer_stride;
    uint32_t *mapped_framebuffer;
    uint32_t buffer_bytes;
    wm_region_t damage;
    uint32_t previous_cursor_x;
    uint32_t previous_cursor_y;
    bool previous_cursor_visible;
    wm_cursor_style_t previous_cursor_style;
    uint64_t next_frame_ms;
    /* The window being drawn is going into its own GPU layer texture
     * (Compositor/WM_Gpu.c), cleared to transparent first, rather than onto
     * the composited desktop: see wm_decoration_draw_window(). */
    bool layer_render;
} wm_compositor_t;

typedef struct {
    display_monitor_info_t info;
    wm_rect_t bounds;
} wm_monitor_layout_t;

typedef struct {
    uint32_t *pixels;
    uint32_t width;
    uint32_t height;
} wm_icon_image_t;

typedef struct {
    wm_icon_image_t launcher;
    wm_icon_image_t power;
    wm_icon_image_t reboot;
    wm_icon_image_t notification;
    wm_icon_image_t network;
    wm_icon_image_t volume;
    wm_icon_image_t battery;
    wm_icon_image_t close;
    wm_icon_image_t maximize;
    wm_icon_image_t restore;
    wm_icon_image_t minimize;
    wm_icon_image_t window;
    wm_icon_image_t application;
    wm_icon_image_t search;
    wm_icon_image_t logo;
} wm_system_icons_t;

/* Where an entry in the launcher came from, which is also how it is
 * started. NATIVE is an ImplusOS ELF the kernel loads directly; LINUX is a
 * foreign glibc binary that only reaches the screen through a host launcher
 * (X11 via com.ImplusOS.linuxapp, Wayland via com.ImplusOS.gtk3demo), so
 * `path` names that launcher and `argument` names the Linux program. */
typedef enum {
    WM_APP_KIND_NATIVE = 0,
    WM_APP_KIND_LINUX
} wm_app_kind_t;

/* freedesktop.org menu categories, collapsed to the handful worth a section
 * header here. wm_app_category_label() has the Japanese names. */
typedef enum {
    WM_APP_CATEGORY_SYSTEM = 0,
    WM_APP_CATEGORY_INTERNET,
    WM_APP_CATEGORY_DEVELOPMENT,
    WM_APP_CATEGORY_GRAPHICS,
    WM_APP_CATEGORY_MULTIMEDIA,
    WM_APP_CATEGORY_OFFICE,
    WM_APP_CATEGORY_GAME,
    WM_APP_CATEGORY_UTILITY,
    WM_APP_CATEGORY_OTHER,
    WM_APP_CATEGORY_COUNT
} wm_app_category_t;

typedef struct {
    char name[WM_APP_NAME_MAX];
    char path[WM_APP_PATH_MAX];
    char argument[WM_APP_ARG_MAX];
    char comment[WM_APP_COMMENT_MAX];
    /* Absolute path of a PNG to draw as the icon. Empty means "derive one":
     * <dir-of-path>/Resource/App.png, and failing that the badge. */
    char icon_path[WM_APP_PATH_MAX];
    char badge[4];
    /* wm_icon_kind_t to draw when no icon file could be loaded, or -1 for
     * the two-letter badge. Resolved from Icon= at startup, which is a
     * string compare -- unlike a PNG lookup, which touches the disk. */
    int32_t vector_icon;
    wm_app_category_t category;
    wm_app_kind_t kind;
    /* Runs a foreign glibc binary, whether or not `kind` is LINUX -- see
     * wm_desktop_entry_t.linux_backed. Drawn as a chip in the Start menu. */
    bool linux_backed;
    uint32_t *icon_pixels;
    uint32_t icon_width;
    uint32_t icon_height;
} wm_launcher_app_t;

typedef struct {
    wm_launcher_app_t apps[WM_MAX_LAUNCHER_APPS];
    uint32_t app_count;
    uint32_t *wallpaper_pixels;
    uint32_t wallpaper_width;
    uint32_t wallpaper_height;
    wm_system_icons_t system_icons;
    uint32_t system_icon_load_index;
    uint32_t app_icon_load_index;
    bool metadata_loaded;
    bool icons_loaded;
    bool wallpaper_load_attempted;
} wm_assets_t;

typedef struct {
    char title[64];
    char message[128];
    uint64_t start_ms;
    uint32_t duration_ms;
    bool active;
} wm_notification_t;

typedef struct {
    char title[64];
    char message[128];
    uint64_t created_ms;
    bool valid;
} wm_notification_history_t;

typedef struct {
    ipc_message_t messages[WM_EVENT_QUEUE_SIZE];
    uint32_t head;
    uint32_t tail;
    uint32_t count;
    uint32_t dropped;
    int32_t last_mouse_index;
} wm_event_queue_t;

typedef struct {
    bool dragging;
    bool resizing;
    uint32_t active_window_id;
    wm_hit_zone_t resize_zone;
    int32_t pointer_start_x;
    int32_t pointer_start_y;
    wm_rect_t frame_start;
    uint8_t previous_buttons;
    uint64_t last_title_click_ms;
    uint32_t last_title_click_window;
    uint32_t last_hover_window_id;
    wm_hit_zone_t last_hover_zone;
    uint64_t last_pointer_frame_ms;
    uint64_t last_kernel_pointer_sequence;
} wm_input_state_t;

typedef struct {
    time_t ntp_base_sec;
    uint64_t ntp_base_uptime_ms;
    uint64_t ntp_poll_start_ms;
    uint16_t ntp_local_port;
    uint32_t ntp_server_ip;
    bool ntp_ready;
} wm_ntp_state_t;

/* State for the Wi-Fi settings panel (UI/WM_WifiPanel.c), opened from the
 * taskbar's network tray icon. selected_index < 0 shows the scan-result
 * list; selected_index >= 0 shows the password-entry sub-view for
 * results[selected_index] (skipped entirely for open networks, which
 * connect straight from the list). */
#define WM_WIFI_PASSWORD_MAX 63u

typedef struct {
    bool open;
    wifi_scan_result_t results[WIFI_MAX_SCAN_RESULTS];
    uint32_t result_count;
    uint32_t scroll;
    int32_t selected_index;
    char password[WM_WIFI_PASSWORD_MAX + 1u];
    uint32_t password_len;
    bool password_active;
    wifi_status_t status;
    uint64_t last_status_poll_ms;
    uint64_t last_results_poll_ms;
    uint64_t scan_started_ms;
    bool scan_active;
} wm_wifi_panel_t;

typedef enum {
    WM_MENU_ROW_HEADER = 0,   /* `value` is a wm_app_category_t */
    WM_MENU_ROW_APP           /* `value` indexes wm_assets_t.apps[] */
} wm_menu_row_kind_t;

typedef struct {
    uint16_t kind;
    uint16_t value;
} wm_menu_row_t;

typedef struct {
    wm_scene_t scene;
    wm_compositor_t compositor;
    display_topology_t display_topology;
    wm_monitor_layout_t monitors[DISPLAY_MAX_MONITORS];
    uint32_t monitor_count;
    wm_theme_t theme;
    wm_font_t font;
    wm_assets_t assets;
    wm_event_queue_t event_queue;
    wm_input_state_t input;
    wm_notification_t notifications[WM_MAX_NOTIFICATIONS];
    wm_notification_history_t notification_history[WM_MAX_NOTIFICATION_HISTORY];
    uint32_t notification_history_count;
    uint32_t notification_history_next;
    uint32_t notification_unread_count;
    uint32_t notification_center_scroll;
    bool notification_center_open;
    bool launcher_open;
    uint32_t launcher_scroll;
    int32_t launcher_hover_index;
    /* Start-menu keyboard selection, kept apart from the pointer's hover.
     * They cannot share a field: the hover is recomputed from the cursor
     * position on every pointer report -- and an absolute pointing device
     * keeps reporting while it sits still -- so a shared field would be
     * cleared again within milliseconds of an arrow key setting it. The
     * pointer clears this one only when the cursor has actually moved. */
    int32_t launcher_selected_index;
    uint32_t taskbar_hover_kind;
    uint32_t taskbar_hover_window_id;
    char clock_text[32];
    char date_text[16];
    char search_text[WM_SEARCH_MAX];
    uint32_t search_len;
    bool search_active;
    /* Start-menu viewport rows: section headers interleaved with app cards,
     * so a category heading scrolls with the apps under it instead of being
     * pinned. Sized for every app plus one header per category. */
    wm_menu_row_t menu_rows[WM_MAX_LAUNCHER_APPS + WM_APP_CATEGORY_COUNT];
    uint32_t filter_count;
    uint32_t filter_generation;
    wm_dialog_t dialog;
    bool running;
    wm_ntp_state_t ntp;
    wm_wifi_panel_t wifi_panel;
} wm_state_t;

extern wm_state_t g_wm_state;

static inline int32_t wm_max_i32(int32_t a, int32_t b) { return a > b ? a : b; }
static inline int32_t wm_min_i32(int32_t a, int32_t b) { return a < b ? a : b; }
static inline uint32_t wm_min_u32(uint32_t a, uint32_t b) { return a < b ? a : b; }
static inline uint32_t wm_max_u32(uint32_t a, uint32_t b) { return a > b ? a : b; }
