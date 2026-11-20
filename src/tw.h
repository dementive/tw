#ifndef TW_H
#define TW_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <wayland-server-core.h>
#include <wlr/util/box.h>
#include <xkbcommon/xkbcommon.h>

#include "layout.h"

#define LENGTH(a) (sizeof(a) / sizeof((a)[0]))

/* Modifier placeholder for bindings: Super on hardware, Alt when nested. */
#define TW_MOD (1u << 31)

/* Mod+1 .. Mod+9 */
#define TW_WORKSPACES 9

/* In an output rule: let tw place the output, to the right of the others. */
#define TW_AUTO INT32_MIN

struct tw_clipboard;
struct tw_column;
struct tw_layer_surface;
struct tw_lock;
struct tw_power;
struct tw_server;
struct tw_window;
struct tw_x11;
struct wlr_output;
struct wlr_pointer_constraint_v1;
struct wlr_surface;
struct wlr_xdg_popup;
struct wlr_xdg_toplevel;
struct wlr_xdg_toplevel_decoration_v1;

/* One scrolling strip. It is on screen while an output shows it; each output
 * shows a different one. */
struct tw_workspace {
	struct wl_list columns;  /* tw_column.link; the strip, left to right */
	struct wl_list floating; /* tw_window.link; dialogs, topmost first */
	/* Both are remembered while the workspace is hidden. */
	struct tw_window *focused; /* gets the keyboard; tiled or floating */
	struct tw_window *tiled;   /* last focused tiled window; the strip scrolls to it */

	/* view_x is the strip coordinate that belongs at the left edge of the
	 * output. view_pos is where the view is drawn right now: it trails
	 * view_x while animating and leads it during a touchpad swipe. */
	int view_x;
	double view_pos;
	double view_velocity; /* pixels per second */
	bool swiping;
	uint32_t swipe_time_msec; /* of the last swipe event, to measure speed */
	bool on_output; /* an output showed it when it was last arranged */
};

struct tw_output {
	struct wl_list link; /* tw_server.outputs */
	struct tw_server *server;
	struct wlr_output *wlr_output;
	struct wlr_box usable; /* layout box minus layer-shell exclusive zones */
	/* Index of the workspace it shows; no two outputs show the same one.
	 * TW_WORKSPACES if there are more outputs than workspaces. */
	size_t workspace;
	/* That workspace's view is gliding toward view_x. Stepped once per frame
	 * of this output, which is also what stops it: no frame, no work. */
	bool animating;
	int64_t animation_last_ms;
	/* The lock curtain has been on this output's screen since the session
	 * was locked: a frame committed after lock_seq has been presented. */
	bool curtain_shown;
	uint32_t lock_seq;
	/* The laptop's own screen (eDP, LVDS, DSI), and while that is true,
	 * whether it is out of use: off and out of the layout, because the lid
	 * is closed and another output is there. */
	bool builtin;
	bool lid_off;
	/* What the monitors tool chose for it (output management, output.c),
	 * kept until it is unplugged; until then machines.h decides. `off`:
	 * turned off, out of use as behind a closed lid. */
	struct {
		bool off;
		struct wlr_output_mode *mode; /* NULL: its preferred one */
		float scale;                  /* 0: machines.h's */
		bool placed;
		int x, y;
	} chosen;
	/* The frame cap on battery (battery_idle_fps): when the last frame was
	 * shown, and the timer that shows a held-back one when its turn comes.
	 * frame_held while that timer is set. */
	int64_t last_frame_ms;
	struct wl_event_source *frame_timer; /* NULL: never capped */
	bool frame_held;
	/* The display refused to turn the variable refresh rate on or off for
	 * the fullscreen window coming or going; not asked again until that
	 * changes (output_draw()). */
	bool vrr_refused;
	struct wl_listener frame;
	struct wl_listener present;
	struct wl_listener request_state;
	struct wl_listener destroy;
};

/*
 * One window. Where it is (link, workspace, column) and whether it is
 * fullscreen (fullscreen, float_after_fullscreen) are strip.c's to change;
 * the rest is window.c's, which carries them out.
 *
 * Its scene_tree lives under server->window_tree,
 * or fullscreen_tree while fullscreen, or float_tree while floating:
 *
 *   scene_tree          positioned at the window; node.data = window
 *   └── surface_tree    the client's surfaces, clipped to the window's box
 *                       while tiled; a dialog's are not clipped
 *
 * Its menus are drawn above every window, dialog and bar, so popup_tree
 * lives under server->popup_tree and follows scene_tree's position and
 * visibility:
 *
 *   popup_tree          the client's popups, unclipped; node.data = window
 *
 * xdg_surface.data points at the tree its popups attach to: popup_tree for a
 * toplevel, the popup's own tree for a popup. It is NULL once that tree is
 * gone, and for a popup once the popup is.
 */
struct tw_window {
	/* tw_column.windows, or tw_workspace.floating; valid only while mapped. */
	struct wl_list link;
	struct tw_server *server;
	struct tw_workspace *workspace; /* NULL unless mapped */
	struct tw_column *column;       /* NULL unless mapped and tiled; strip.c's */
	struct wlr_xdg_toplevel *xdg_toplevel;
	struct wlr_xdg_toplevel_decoration_v1 *decoration; /* NULL if none */

	struct wlr_scene_tree *scene_tree;
	struct wlr_scene_tree *surface_tree;
	struct wlr_scene_tree *popup_tree;

	bool mapped;
	bool floating;   /* a dialog: centred, sized by the client, not in the strip */
	bool fullscreen; /* covers the whole output; alone in its column */
	/* A window of a fixed size (a game) floats, but is in the strip while
	 * fullscreen: it floats again when it leaves fullscreen. */
	bool float_after_fullscreen;

	/* The last state sent to the client or the scene, so that re-arranging
	 * an unchanged strip sends no configure and causes no damage. */
	int width, height;
	bool suspended;
	bool fullscreen_applied;
	struct wlr_box clip;
	/* How much of the window's left and right side is cut off, because it
	 * sticks out of its output toward a neighbouring one. */
	int cut_left, cut_right;

	/* Its entry in the list of windows other programs see, which a screen
	 * recorder offers to choose from (ext-foreign-toplevel-list); NULL
	 * unless mapped. */
	struct wlr_ext_foreign_toplevel_handle_v1 *handle;
	/* Window capture (window.c): a scene of its own holding only this
	 * window, made at the first request to record it and gone when it
	 * unmaps; NULL until then. `captures` counts the recordings under way:
	 * while there are any, the window is not suspended, even off screen. */
	struct wlr_scene *capture_scene;
	struct wlr_ext_image_capture_source_v1 *capture_source;
	struct wl_list capture_link; /* server->captured, while capture_scene != NULL */
	int captures;

	struct wl_listener map;
	struct wl_listener unmap;
	struct wl_listener commit;
	struct wl_listener destroy;
	struct wl_listener request_maximize;
	struct wl_listener request_fullscreen;
	struct wl_listener set_title;
	struct wl_listener set_app_id;
	/* Linked only while decoration != NULL. */
	struct wl_listener decoration_request_mode;
	struct wl_listener decoration_destroy;
};

/*
 * Everything tw has, one instance. Grouped by the file that owns each part,
 * in the order main() sets them up; a field another file reads is described
 * where it is declared.
 */
struct tw_server {
	struct wl_display *display;
	struct wl_event_loop *loop;
	const struct tw_machine *machine; /* never NULL */
	struct wl_event_source *sigint, *sigterm;

	struct wlr_backend *backend;
	/* NULL when nested or headless, and once lost: seatd or logind gone.
	 * Losing it takes every screen and keyboard with it, so tw exits. */
	struct wlr_session *session;
	struct wl_listener session_destroy;
	struct wlr_renderer *renderer;   /* replaced when the GPU is reset */
	struct wlr_allocator *allocator; /* likewise */
	struct wlr_compositor *compositor;
	struct wlr_linux_dmabuf_v1 *linux_dmabuf; /* NULL with the software renderer */
	/* Screen and window capture (grim, xdg-desktop-portal-wlr). */
	struct wlr_ext_image_copy_capture_manager_v1 *image_copy_capture;

	/* output.c */
	struct wlr_output_layout *output_layout;
	struct wlr_scene *scene;
	struct wlr_scene_output_layout *scene_layout;
	/* Stacking order, bottom to top: layer_trees[background], [bottom],
	 * window_tree, layer_trees[top], fullscreen_tree, float_tree,
	 * popup_tree, layer_trees[overlay], lock_tree, drag_icon_tree. */
	struct wlr_scene_tree *layer_trees[4]; /* by zwlr_layer_shell_v1_layer */
	struct wlr_scene_tree *window_tree;
	struct wlr_scene_tree *fullscreen_tree;
	struct wlr_scene_tree *float_tree;
	/* Menus, above every window: the windows' own, and those of layer
	 * surfaces below the overlay layer (layer.c). */
	struct wlr_scene_tree *popup_tree;
	struct wlr_scene_tree *lock_tree; /* enabled only while locked */
	struct wlr_scene_tree *drag_icon_tree; /* at the cursor; what a drag shows */
	struct wl_list outputs; /* tw_output.link, in the order they appeared */
	struct tw_lid lid; /* the laptop's lid, as outputs_lid() last saw it */
	struct wl_listener new_output;
	struct wl_listener layout_change;
	struct wl_listener renderer_lost;
	/* What a fullscreen window asks of the display, and what a window says
	 * it shows (frame_capped()). */
	struct wlr_tearing_control_manager_v1 *tearing_control;
	struct wlr_content_type_manager_v1 *content_type;
	/* The monitors tool's protocol (wlr-output-management), and the idle
	 * callback that has it restore what it saved once the outputs changed. */
	struct wlr_output_manager_v1 *output_manager;
	struct wl_listener output_apply;
	struct wl_listener output_test;
	struct wl_event_source *monitors_restore;
	bool started; /* the socket is up: programs can be started */
	/* Idle: a new renderer takes over after a GPU reset. */
	struct wl_event_source *renderer_restart;

	/* layer.c */
	struct wlr_layer_shell_v1 *layer_shell;
	struct wl_list layer_surfaces; /* tw_layer_surface.link, newest first */
	struct tw_layer_surface *keyboard_layer; /* holds the keyboard, or NULL */
	struct wl_listener new_layer_surface;

	/* window.c */
	struct wlr_xdg_shell *xdg_shell;
	struct wlr_xdg_decoration_manager_v1 *decoration_manager;
	struct wlr_xdg_activation_v1 *activation;
	struct wlr_ext_foreign_toplevel_list_v1 *toplevel_list;
	struct wlr_ext_foreign_toplevel_image_capture_source_manager_v1 *toplevel_capture;
	struct wl_list captured; /* tw_window.capture_link */
	struct wl_listener new_capture_request;
	struct wl_listener new_capture_session;
	struct wl_listener new_toplevel;
	struct wl_listener new_popup;
	struct wl_listener new_decoration;
	struct wl_listener request_activate;

	/* strip.c */
	struct tw_workspace workspaces[TW_WORKSPACES];
	/* Index of the workspace with the focus. The output showing it is the
	 * focused output: new windows open there. */
	size_t active;
	struct tw_window *focused; /* the window whose client is told it is active */

	/* input.c */
	struct wlr_seat *seat;
	struct wlr_cursor *cursor;
	struct wlr_xcursor_manager *cursor_manager;
	struct xkb_keymap *keymap; /* every keyboard's but the virtual ones; NULL until the first */
	struct wlr_virtual_keyboard_manager_v1 *virtual_keyboard_manager;
	struct wlr_cursor_shape_manager_v1 *cursor_shape_manager;
	struct wlr_pointer_gestures_v1 *pointer_gestures;
	struct wlr_virtual_pointer_manager_v1 *virtual_pointer_manager;
	struct wlr_relative_pointer_manager_v1 *relative_pointer_manager;
	struct wlr_pointer_constraints_v1 *pointer_constraints;
	/* The pointer lock or confinement in force, or NULL. */
	struct wlr_pointer_constraint_v1 *active_constraint;
	/* A touchpad swipe tw is handling itself rather than passing on. */
	bool swipe_ours;
	enum tw_swipe_axis {
		TW_SWIPE_UNDECIDED,
		TW_SWIPE_SIDEWAYS, /* drags the strip */
		TW_SWIPE_VERTICAL, /* switches workspace */
	} swipe_axis;
	double swipe_dx, swipe_dy;
	/* Mod and the left button are moving a window with the mouse: `moved`
	 * is that window, NULL once it has gone (strip_remove()). */
	bool moving;
	struct tw_window *moved;
	/* The window under the pointer when it last moved or was given to what
	 * is under it, NULL over none or once that window has gone
	 * (strip_remove()). Focus follows the mouse only onto another one. */
	struct tw_window *hovered;
	uint32_t mod; /* what TW_MOD resolves to */
	struct wl_listener new_input;
	struct wl_listener new_virtual_keyboard;
	struct wl_listener new_virtual_pointer;
	struct wl_listener new_constraint;
	struct wl_listener request_set_shape;
	struct wl_listener request_set_primary_selection;
	struct wl_listener cursor_motion;
	struct wl_listener cursor_motion_absolute;
	struct wl_listener cursor_button;
	struct wl_listener cursor_axis;
	struct wl_listener cursor_frame;
	struct wl_listener swipe_begin;
	struct wl_listener swipe_update;
	struct wl_listener swipe_end;
	struct wl_listener pinch_begin;
	struct wl_listener pinch_update;
	struct wl_listener pinch_end;
	struct wl_listener hold_begin;
	struct wl_listener hold_end;
	struct wl_listener request_set_cursor;
	struct wl_listener pointer_focus_change;
	struct wl_listener request_set_selection;
	struct wl_listener request_start_drag;
	struct wl_listener start_drag;
	struct wl_listener drag_destroy; /* of the drag under way; linked only then */

	/* clipboard.c */
	struct tw_clipboard *clipboard; /* NULL when copies are not kept */

	/* idle.c */
	struct wlr_idle_notifier_v1 *idle_notifier;
	struct wlr_idle_inhibit_manager_v1 *idle_inhibit_manager;
	struct wl_listener new_idle_inhibitor;
	struct wl_listener session_active; /* only with a session */
	struct wl_event_source *idle_timer;
	struct wl_event_source *resume_watch; /* fires when the machine wakes up */
	int resume_fd;
	int64_t asleep_ms; /* asleep_ms() when the clock last jumped */
	int blank_after_ms;   /* 0 never blanks */
	int lock_after_ms;    /* 0 never locks by itself */
	int suspend_after_ms; /* 0 never suspends */
	int suspend_tries;    /* suspend attempts since the last input */
	int64_t last_activity_ms;
	int64_t woken_ms; /* when a notification last turned the screen on */
	int64_t lit_check_ms; /* when input last looked for an output left dark */
	bool blanked; /* outputs are powered off */
	bool idle_inhibited; /* a visible surface asked to keep the screen on */

	/* lock.c */
	struct wlr_session_lock_manager_v1 *lock_manager;
	struct wlr_scene_rect *lock_background;
	struct wl_listener new_lock;
	bool locked;
	/* The client holding the lock. NULL while locked means there is none:
	 * it died, or tw locked by itself and the locker has not arrived yet. */
	struct tw_lock *lock;
	/* When tw last tried to start the locker (it may not be installed); 0 if
	 * never. */
	int64_t locker_started_ms;

	/* power.c */
	/* NULL without a battery, or when tw does not run the machine (no
	 * session). */
	struct tw_power *power;
	bool on_battery; /* nothing but its battery supplies the machine */

	/* x11.c */
	struct tw_x11 *x11; /* NULL when X11 support is off */
	int x11_idle_ms;    /* 0 never stops the X11 helper */
};

/* An output in use is in the layout and shows a workspace: one neither
 * behind a closed lid nor turned off with the monitors tool. */
static inline bool output_in_use(const struct tw_output *output) {
	return !output->lid_off && !output->chosen.off;
}

/* Nothing is on screen while the outputs are off or the session is locked. */
static inline bool screen_on(const struct tw_server *server) {
	return !server->blanked && !server->locked;
}

union tw_arg {
	int i;
	const char *s;
};

typedef void (*tw_action)(struct tw_server *server, const union tw_arg *arg);

struct tw_binding {
	uint32_t mods;
	xkb_keysym_t sym; /* unshifted keysym, e.g. XKB_KEY_h even with Shift */
	tw_action action;
	union tw_arg arg;
};

struct tw_output_rule {
	const char *name; /* connector name such as "eDP-1" */
	float scale;
	int x, y; /* position in the screen area, or TW_AUTO */
};

/* What depends on the machine (machines.h); main() picks one by its board. */
#define TW_MACHINE_OUTPUTS 4
struct tw_machine {
	const char *board; /* as the firmware names it; NULL for every other machine */
	struct tw_output_rule outputs[TW_MACHINE_OUTPUTS]; /* the unused have no name */
	int suspend_after_seconds;
	int battery_suspend_percent;
};

/* main.c */
/* Monotonic clock in milliseconds. */
int64_t now_ms(void);
/* How long the machine has slept since it booted, in milliseconds. If that
 * grew between two looks, it has just woken up. */
int64_t asleep_ms(void);
/* Runs a shell command, detached from tw. */
void spawn(const char *cmd);
/* In a child of tw, before exec: the signals as a new program expects them,
 * stdin from /dev/null, and no file of tw's left open across exec (a
 * caller that hands one on clears its close-on-exec flag afterwards). */
void child_reset(void);
/* Calls func when the forked child pid exits, through a pidfd: no polling
 * and no SIGCHLD. The caller reaps it then. On failure the child is killed
 * and reaped, and NULL returned. */
struct wl_event_source *child_watch(struct tw_server *server, pid_t pid,
	wl_event_loop_fd_func_t func, void *data);
/*
 * Looks up the program a shell command starts (its first word) on PATH,
 * without running anything. On success `path` holds where it is.
 */
bool program_find(const char *command, char *path, size_t size);
bool program_installed(const char *command);
/* spawn(), unless the command is "" or its program is not installed, which
 * is logged. True if it was started. For the commands in config.h. */
bool spawn_installed(const char *cmd);
void action_spawn(struct tw_server *server, const union tw_arg *arg);
void action_quit(struct tw_server *server, const union tw_arg *arg);

/* output.c */
void output_init(struct tw_server *server);
/* Frees what Mesa keeps of its shader compiler, once a new renderer has
 * built its shaders. Nothing for the software renderer. */
void renderer_release_compiler(struct wlr_renderer *renderer);
void output_finish(struct tw_server *server);
/* The output in use that shows this workspace, or NULL if it is hidden. It
 * may have no mode yet; output_boxes() then fails. */
struct tw_output *output_showing(struct tw_server *server, size_t workspace);
/* The output with the focus (the one showing the active workspace), or NULL. */
struct tw_output *output_focused(struct tw_server *server);
/* The oldest output in use, other than `except`; NULL if there is none. */
struct tw_output *output_oldest(struct tw_server *server, struct tw_output *except);
struct tw_output *output_at(struct tw_server *server, double lx, double ly);
/* The nearest output in a direction (enum wlr_direction), or NULL. */
struct tw_output *output_adjacent(struct tw_output *output, int direction);
/* `area` is what windows may use on the output, `full` the whole of it.
 * False if the output is NULL or has no room. */
bool output_boxes(struct tw_output *output, struct wlr_box *area, struct wlr_box *full);
/* Runs monitors_command (config.h), soon and once however many outputs come
 * or go together. Call once programs can be started, and as outputs change. */
void outputs_restore(struct tw_server *server);
/* Puts every output that is on into that mode. Call when on_battery changed. */
void outputs_update_mode(struct tw_server *server);
/* Shows at once every frame the cap on battery is holding back. Call on
 * input. */
void outputs_release_frames(struct tw_server *server);
/*
 * Turns an output on or off. One that comes on takes the mode it should have
 * now, since the power cable may have gone in or out while it was off.
 */
bool output_set_power(struct tw_output *output, bool on);
/*
 * The lid is `closed`: takes the laptop's own screen out of use while another
 * output is there, brings it back otherwise, and locks the session when that
 * screen is left the only one behind the lid. input.c calls it when the lid
 * moves; output.c when an output comes or goes.
 */
void outputs_lid(struct tw_server *server, bool closed);

/* layer.c */
void layer_init(struct tw_server *server);
void layer_finish(struct tw_server *server);
/* Configures every layer surface and recomputes each output's usable area. */
void layers_arrange(struct tw_server *server);
/* Closes the layer surfaces of an output that is going away or leaving the
 * layout. */
void layers_output_destroyed(struct tw_server *server, struct wlr_output *output);
/* Surface of the layer that holds the keyboard, or NULL. */
struct wlr_surface *layer_keyboard_surface(struct tw_server *server);
/* The box of the output a layer surface's popup is on, in the coordinates of
 * that layer surface; false if the popup does not belong to one. */
bool layer_popup_box(struct tw_server *server, struct wlr_xdg_popup *popup, struct wlr_box *box);

/* window.c */
void window_init(struct tw_server *server);
void window_finish(struct tw_server *server);
/*
 * Window under the layout coordinates, or NULL. *surface is set whenever a
 * client surface is under the point, including layer surfaces and popups.
 */
struct tw_window *window_at(struct tw_server *server, double lx, double ly,
	struct wlr_surface **surface, double *sx, double *sy);
/* Whether the window or layer surface this surface belongs to is on screen. */
bool surface_visible(struct tw_server *server, struct wlr_surface *surface);
/* Gives a popup its tree in the scene, under `parent`: a window's menus get
 * theirs as they appear, a layer surface's when its client attaches them.
 * The tree goes with the popup, or with `parent` if that goes first. */
void popup_attach(struct wlr_xdg_popup *popup, struct wlr_scene_tree *parent);
/*
 * What strip.c places windows with. Each sends the client and the scene only
 * what changed since the last call, so arranging an unchanged strip costs
 * nothing.
 *
 * window_place() puts the window's outer box at `box` (layout coordinates).
 * With `configure` the client is asked for that size; a dialog chose its
 * own. `cut_left` and `cut_right` are how much of it to hide on each side,
 * where it sticks out of its output toward a neighbouring one.
 */
void window_place(struct tw_window *window, const struct wlr_box *box, bool configure,
	int cut_left, int cut_right);
/* Centres a dialog in `area`, at whatever size its client chose. */
void window_centre(struct tw_window *window, const struct wlr_box *area);
/*
 * `on_screen`: draw it. `wanted`: it is, or is about to be, on screen, so the
 * client should be drawing. The two differ while the strip is moving.
 *
 * The battery win of a scrolling layout: most windows are off-screen or on
 * another workspace. A disabled node gets no frame callbacks, and `suspended`
 * tells the client outright to stop drawing.
 */
void window_show(struct tw_window *window, bool on_screen, bool wanted);
/*
 * Brings the client and the scene in line with window->fullscreen: the
 * client is told, and the window gets a place above the bars. With nothing
 * else left to draw, wlroots then hands the client's buffer straight to the
 * display instead of compositing it (direct scanout).
 */
void window_apply_fullscreen(struct tw_window *window);
/* Sets window->floating and tells the client: a dialog picks its own size
 * and is not tiled. */
void window_set_floating(struct tw_window *window, bool floating);
/* Ends every window capture: what records them was made with a renderer that
 * is about to go. */
void windows_capture_stop(struct tw_server *server);

/* strip.c */
void strip_init(struct tw_server *server);
/*
 * A window has mapped: it goes on the active workspace and gets the focus,
 * into the strip in a column of its own, or in front of it as a dialog, and
 * fullscreen if its client asked. False if out of memory.
 */
bool strip_add(struct tw_window *window);
/* A window has unmapped: it leaves its workspace and its focus passes on. */
void strip_remove(struct tw_window *window);
/* The size a window that maps now will get, fullscreen or in a column of its
 * own. False, leaving both untouched, if no output shows the active
 * workspace. */
bool strip_initial_size(struct tw_server *server, bool fullscreen, int *width, int *height);
/* The output boxes for a workspace: `area` is what its windows may use,
 * `full` the whole output (for fullscreen). False if no output shows it. */
bool workspace_boxes(struct tw_server *server, const struct tw_workspace *workspace,
	struct wlr_box *area, struct wlr_box *full);
/*
 * A fullscreen window is alone in its column, so one that shares a column
 * leaves it first. A dialog stays with its parent, but a window that floats
 * only for its fixed size (a game) goes into the strip, and back out when it
 * leaves fullscreen. Follow with strip_arrange().
 */
void window_set_fullscreen(struct tw_window *window, bool fullscreen);
/* Makes the window the focused one of its workspace. Follow with strip_arrange(). */
void window_focus(struct tw_server *server, struct tw_window *window);
/*
 * Re-derives which window is the active one and which surface has the
 * keyboard, from the active workspace, server->keyboard_layer and the lock.
 * Call after changing any of them; nothing else sends focus events.
 */
void focus_update(struct tw_server *server);
/* Scrolls the focused column into view and places every window. */
void strip_arrange(struct tw_server *server);
/* Advances the scroll animation of the workspace on this output by one
 * frame; true if it needs another. */
bool strip_animate(struct tw_output *output);
/* A fullscreen window of this workspace, or NULL. */
struct tw_window *workspace_fullscreen(const struct tw_workspace *workspace);
/* Moves the focus to the workspace this output shows. */
void strip_focus_output(struct tw_server *server, struct tw_output *output);
/*
 * Where the focus comes to rest once the view has glided there: the active
 * window and its box, or with none (NULL) the usable area of the focused
 * output. False if no output shows the active workspace.
 */
bool strip_focus_box(struct tw_server *server, struct tw_window **window, struct wlr_box *box);
/* Puts a window moved with the mouse where it was let go, at lx,ly: beside
 * the column there, or onto that monitor's workspace. */
void strip_drop(struct tw_server *server, struct tw_window *window, double lx, double ly);
/* A touchpad swipe drags the strip. `dx` is the finger movement in pixels. */
void strip_swipe_begin(struct tw_server *server, uint32_t time_msec);
void strip_swipe_update(struct tw_server *server, double dx, uint32_t time_msec);
/* Lets go: the strip glides to the nearest column edge. */
void strip_swipe_end(struct tw_server *server, bool cancelled, uint32_t time_msec);

/* Actions on the focused column. arg.i is a direction (-1 left, +1 right). */
void action_close(struct tw_server *server, const union tw_arg *arg);
void action_focus(struct tw_server *server, const union tw_arg *arg);
void action_focus_edge(struct tw_server *server, const union tw_arg *arg);
void action_move(struct tw_server *server, const union tw_arg *arg);
void action_move_edge(struct tw_server *server, const union tw_arg *arg);
/* Windows within a column. arg.i is -1 up, +1 down. */
void action_focus_window(struct tw_server *server, const union tw_arg *arg);
void action_move_window(struct tw_server *server, const union tw_arg *arg);
/* A window alone in its column joins the column on that side (arg.i); a
 * window that shares a column leaves it for a column of its own. */
void action_consume_or_expel(struct tw_server *server, const union tw_arg *arg);
/* Pulls the top window of the column on the right into the focused column. */
void action_consume(struct tw_server *server, const union tw_arg *arg);
/* Puts the focused window into a column of its own, to the right. */
void action_expel(struct tw_server *server, const union tw_arg *arg);
void action_cycle_width(struct tw_server *server, const union tw_arg *arg);
void action_maximize(struct tw_server *server, const union tw_arg *arg);
void action_fullscreen(struct tw_server *server, const union tw_arg *arg);
/* Workspace actions. arg.i is a zero-based index, or -1 up / +1 down for the
 * _relative variants. Moving a column takes the focus along, like niri. */
void action_workspace(struct tw_server *server, const union tw_arg *arg);
void action_workspace_relative(struct tw_server *server, const union tw_arg *arg);
void action_move_to_workspace(struct tw_server *server, const union tw_arg *arg);
void action_move_to_workspace_relative(struct tw_server *server, const union tw_arg *arg);
/* The same by direction (arg.i is an enum wlr_direction): to the workspace
 * on the monitor on that side, or with no monitor there, to the previous
 * (up, left) or next (down, right) workspace. */
void action_workspace_toward(struct tw_server *server, const union tw_arg *arg);
void action_move_to_workspace_toward(struct tw_server *server, const union tw_arg *arg);

/* input.c */
void input_init(struct tw_server *server);
void input_finish(struct tw_server *server);
/*
 * Re-derives which pointer lock or confinement is in force: the one of the
 * surface under the pointer, if that window also has the keyboard. Call
 * after the keyboard or pointer focus changes.
 */
void pointer_constraint_update(struct tw_server *server);
/* Gives the pointer to the surface under it now, which may have changed
 * without the pointer moving. strip_arrange() and each step of a glide or
 * swipe call it. */
void pointer_refocus(struct tw_server *server);

/* clipboard.c */
/* Keeps what is copied for when its program exits. Call after input_init(). */
void clipboard_init(struct tw_server *server);
void clipboard_finish(struct tw_server *server);
/* A program asked for an empty clipboard: the copy kept so far goes too.
 * Call before the seat is told. */
void clipboard_cleared(struct tw_server *server);

/* idle.c */
void idle_init(struct tw_server *server);
void idle_finish(struct tw_server *server);
/* The user did something: restarts the idle clock and wakes the outputs. */
void idle_activity(struct tw_server *server);
/* Re-checks whether a visible surface is keeping the screen on. */
void idle_update_inhibited(struct tw_server *server);
/* Turns the screen on, if it is off, for another blank_after_seconds, as a
 * notification does: something wants to be seen. Not input: the lock and
 * suspend clocks go on, so notifications never keep an empty desk unlocked. */
void idle_wake(struct tw_server *server);
/* Locks the session (unless lock_after_ms is 0) and runs suspend_command. */
void suspend_now(struct tw_server *server);
/* Powers the outputs off until the next input. */
void action_blank(struct tw_server *server, const union tw_arg *arg);

/* lock.c */
void lock_init(struct tw_server *server);
void lock_finish(struct tw_server *server);
/* Resizes the lock surfaces and the curtain behind them to the outputs. */
void lock_arrange(struct tw_server *server);
/* Surface that gets the keyboard while locked, or NULL. */
struct wlr_surface *lock_keyboard_surface(struct tw_server *server);
/* An output presented a frame, went off or went away: the lock client is
 * told the session is locked once the curtain is on every screen. */
void lock_confirm(struct tw_server *server);
/*
 * Locks the session right now and starts the locker (config.h) to draw the
 * lock screen and take the password. Does nothing if the locker is not
 * installed: a lock nobody can open would be worse than none.
 */
void lock_now(struct tw_server *server);
void action_lock(struct tw_server *server, const union tw_arg *arg);
/* A key was pressed. If the session is locked and has no lock screen (the
 * locker crashed), this starts one. */
void lock_key_pressed(struct tw_server *server);

/* power.c */
/* Finds out what supplies the machine and acts on it. Call before the
 * backend starts, so that the outputs come up in the right mode. */
void power_init(struct tw_server *server);
void power_finish(struct tw_server *server);

/* x11.c */
/* Opens the X11 display sockets and sets DISPLAY for the programs tw starts,
 * or removes DISPLAY if X11 support is off. Call before the session's
 * programs start. (The power command power_init() runs earlier has no use
 * for X11.) */
void x11_init(struct tw_server *server);
void x11_finish(struct tw_server *server);
/* A window closed: its program may have been the last X11 application. */
void x11_window_closed(struct tw_server *server);

#endif
