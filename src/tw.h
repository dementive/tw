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

struct tw_layer_surface;
struct tw_lock;
struct tw_server;
struct tw_window;
struct wlr_output;
struct wlr_surface;

/* One scrolling strip. Only the active workspace is on screen. */
struct tw_workspace {
	struct wl_list windows; /* tw_window.link; left to right, one per column */
	struct tw_window *focused; /* remembered while the workspace is hidden */
	int view_x; /* strip coordinate at the left edge of the output */
};

struct tw_output {
	struct wl_list link; /* tw_server.outputs */
	struct tw_server *server;
	struct wlr_output *wlr_output;
	struct wlr_box usable; /* layout box minus layer-shell exclusive zones */
	struct wl_listener frame;
	struct wl_listener request_state;
	struct wl_listener destroy;
};

struct tw_server {
	struct wl_display *display;
	struct wl_event_loop *loop;
	struct wl_event_source *sigint, *sigterm;

	struct wlr_backend *backend;
	struct wlr_session *session; /* NULL when nested or headless */
	struct wlr_renderer *renderer;
	struct wlr_allocator *allocator;
	struct wlr_linux_dmabuf_v1 *linux_dmabuf; /* NULL with the software renderer */

	/* output.c */
	struct wlr_output_layout *output_layout;
	struct wlr_scene *scene;
	struct wlr_scene_output_layout *scene_layout;
	/* Stacking order, bottom to top: layer_trees[background], [bottom],
	 * window_tree, layer_trees[top], fullscreen_tree, layer_trees[overlay],
	 * lock_tree. */
	struct wlr_scene_tree *layer_trees[4]; /* by zwlr_layer_shell_v1_layer */
	struct wlr_scene_tree *window_tree;
	struct wlr_scene_tree *fullscreen_tree;
	struct wlr_scene_tree *lock_tree; /* enabled only while locked */
	struct wl_list outputs; /* tw_output.link; the first usable one shows the strip */
	struct wl_listener new_output;
	struct wl_listener layout_change;

	/* window.c */
	struct wlr_xdg_shell *xdg_shell;
	struct wlr_xdg_decoration_manager_v1 *decoration_manager;
	struct wl_listener new_toplevel;
	struct wl_listener new_popup;
	struct wl_listener new_decoration;
	struct tw_workspace workspaces[TW_WORKSPACES];
	size_t active; /* index of the workspace on screen */
	struct tw_window *focused; /* the window currently drawn as active */

	/* layer.c */
	struct wlr_layer_shell_v1 *layer_shell;
	struct wl_list layer_surfaces; /* tw_layer_surface.link, newest first */
	struct tw_layer_surface *keyboard_layer; /* holds the keyboard, or NULL */
	struct wl_listener new_layer_surface;

	/* idle.c */
	struct wlr_idle_notifier_v1 *idle_notifier;
	struct wlr_idle_inhibit_manager_v1 *idle_inhibit_manager;
	struct wl_listener new_idle_inhibitor;
	struct wl_event_source *blank_timer;
	int blank_after_ms; /* 0 never blanks */
	int64_t last_activity_ms;
	bool blanked; /* outputs are powered off */
	bool idle_inhibited; /* a visible surface asked to keep the screen on */

	/* lock.c */
	struct wlr_session_lock_manager_v1 *lock_manager;
	struct wlr_scene_rect *lock_background;
	struct wl_listener new_lock;
	bool locked;
	struct tw_lock *lock; /* the client holding the lock; NULL if it died */

	/* input.c */
	struct wlr_seat *seat;
	struct wlr_cursor *cursor;
	struct wlr_xcursor_manager *cursor_manager;
	struct wlr_virtual_keyboard_manager_v1 *virtual_keyboard_manager;
	struct wlr_cursor_shape_manager_v1 *cursor_shape_manager;
	uint32_t mod; /* what TW_MOD resolves to */
	struct wl_list keyboards; /* tw_keyboard.link */
	struct wl_listener new_input;
	struct wl_listener new_virtual_keyboard;
	struct wl_listener request_set_shape;
	struct wl_listener request_set_primary_selection;
	struct wl_listener cursor_motion;
	struct wl_listener cursor_motion_absolute;
	struct wl_listener cursor_button;
	struct wl_listener cursor_axis;
	struct wl_listener cursor_frame;
	struct wl_listener request_set_cursor;
	struct wl_listener pointer_focus_change;
	struct wl_listener request_set_selection;
};

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

/* main.c */
void spawn(const char *cmd);
void action_spawn(struct tw_server *server, const union tw_arg *arg);
void action_quit(struct tw_server *server, const union tw_arg *arg);

/* output.c */
void output_init(struct tw_server *server);
void output_finish(struct tw_server *server);
/* The output that shows the strip, or NULL. */
struct tw_output *output_primary(struct tw_server *server);
/* Area of that output left for windows; false if there is no output. */
bool output_primary_box(struct tw_server *server, struct wlr_box *box);

/* window.c */
void window_init(struct tw_server *server);
void window_finish(struct tw_server *server);
/*
 * Window under the layout coordinates, or NULL. *surface is set whenever a
 * client surface is under the point, including layer surfaces and popups.
 */
struct tw_window *window_at(struct tw_server *server, double lx, double ly,
	struct wlr_surface **surface, double *sx, double *sy);
/* Makes the window the focused one of its workspace. Follow with strip_arrange(). */
void window_focus(struct tw_server *server, struct tw_window *window);
/*
 * Re-derives which window is drawn active and which surface has the keyboard,
 * from the active workspace and server->keyboard_layer. Call after changing
 * either.
 */
void focus_update(struct tw_server *server);
/* Scrolls the focused column into view and places every window. */
void strip_arrange(struct tw_server *server);
/* Whether the window or layer surface this surface belongs to is on screen. */
bool surface_visible(struct wlr_surface *surface);

/* Actions on the focused column. arg.i is a direction (-1 left, +1 right). */
void action_close(struct tw_server *server, const union tw_arg *arg);
void action_focus(struct tw_server *server, const union tw_arg *arg);
void action_focus_edge(struct tw_server *server, const union tw_arg *arg);
void action_move(struct tw_server *server, const union tw_arg *arg);
void action_move_edge(struct tw_server *server, const union tw_arg *arg);
void action_cycle_width(struct tw_server *server, const union tw_arg *arg);
void action_maximize(struct tw_server *server, const union tw_arg *arg);
void action_fullscreen(struct tw_server *server, const union tw_arg *arg);
/* Workspace actions. arg.i is a zero-based index, or -1 up / +1 down for the
 * _relative variants. Moving a column takes the focus along, like niri. */
void action_workspace(struct tw_server *server, const union tw_arg *arg);
void action_workspace_relative(struct tw_server *server, const union tw_arg *arg);
void action_move_to_workspace(struct tw_server *server, const union tw_arg *arg);
void action_move_to_workspace_relative(struct tw_server *server, const union tw_arg *arg);

/* layer.c */
void layer_init(struct tw_server *server);
void layer_finish(struct tw_server *server);
/* Configures every layer surface and recomputes each output's usable area. */
void layers_arrange(struct tw_server *server);
/* Closes the layer surfaces of an output that is going away. */
void layers_output_destroyed(struct tw_server *server, struct wlr_output *output);
/* Surface of the layer that holds the keyboard, or NULL. */
struct wlr_surface *layer_keyboard_surface(struct tw_server *server);

/* idle.c */
void idle_init(struct tw_server *server);
void idle_finish(struct tw_server *server);
/* The user did something: restarts the idle clock and wakes the outputs. */
void idle_activity(struct tw_server *server);
/* Re-checks whether a visible surface is keeping the screen on. */
void idle_update_inhibited(struct tw_server *server);
/* Powers the outputs off until the next input. */
void action_blank(struct tw_server *server, const union tw_arg *arg);

/* lock.c */
void lock_init(struct tw_server *server);
void lock_finish(struct tw_server *server);
/* Resizes the lock surfaces and the curtain behind them to the outputs. */
void lock_arrange(struct tw_server *server);
/* Surface that gets the keyboard while locked, or NULL. */
struct wlr_surface *lock_keyboard_surface(struct tw_server *server);

/* input.c */
void input_init(struct tw_server *server);
void input_finish(struct tw_server *server);

#endif
