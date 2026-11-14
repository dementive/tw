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

struct tw_layer_surface;
struct tw_lock;
struct tw_server;
struct tw_window;
struct tw_x11;
struct wlr_output;
struct wlr_pointer_constraint_v1;
struct wlr_surface;

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
	 * window_tree, layer_trees[top], fullscreen_tree, float_tree,
	 * layer_trees[overlay], lock_tree. */
	struct wlr_scene_tree *layer_trees[4]; /* by zwlr_layer_shell_v1_layer */
	struct wlr_scene_tree *window_tree;
	struct wlr_scene_tree *fullscreen_tree;
	struct wlr_scene_tree *float_tree;
	struct wlr_scene_tree *lock_tree; /* enabled only while locked */
	struct wl_list outputs; /* tw_output.link, in the order they appeared */
	struct wl_listener new_output;
	struct wl_listener layout_change;

	/* window.c */
	struct wlr_xdg_shell *xdg_shell;
	struct wlr_xdg_decoration_manager_v1 *decoration_manager;
	struct wlr_xdg_activation_v1 *activation;
	struct wl_listener new_toplevel;
	struct wl_listener new_popup;
	struct wl_listener new_decoration;
	struct wl_listener request_activate;
	struct tw_workspace workspaces[TW_WORKSPACES];
	/* Index of the workspace with the focus. The output showing it is the
	 * focused output: new windows open there. */
	size_t active;
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
	struct wl_event_source *idle_timer;
	struct wl_event_source *resume_watch; /* fires when the machine wakes up */
	int resume_fd;
	int blank_after_ms;   /* 0 never blanks */
	int suspend_after_ms; /* 0 never suspends */
	int suspend_tries;    /* suspend attempts since the last input */
	int64_t last_activity_ms;
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
	int64_t locker_started_ms; /* when tw last started the locker; 0 if never */

	/* x11.c */
	struct tw_x11 *x11; /* NULL when X11 support is off */

	/* input.c */
	struct wlr_seat *seat;
	struct wlr_cursor *cursor;
	struct wlr_xcursor_manager *cursor_manager;
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
	int swipe_axis; /* 0 undecided, 1 sideways (scroll), 2 vertical (workspace) */
	double swipe_dx, swipe_dy;
	uint32_t mod; /* what TW_MOD resolves to */
	struct wl_list keyboards; /* tw_keyboard.link */
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

struct tw_output_rule {
	const char *name; /* connector name such as "eDP-1"; NULL matches the rest */
	float scale;
	int x, y; /* position in the screen area, or TW_AUTO */
};

/* main.c */
/* Monotonic clock in milliseconds. */
int64_t now_ms(void);
void spawn(const char *cmd);
/*
 * Looks up the program a shell command starts (its first word) on PATH,
 * without running anything. On success `path` holds where it is.
 */
bool program_find(const char *command, char *path, size_t size);
void action_spawn(struct tw_server *server, const union tw_arg *arg);
void action_quit(struct tw_server *server, const union tw_arg *arg);

/* output.c */
void output_init(struct tw_server *server);
void output_finish(struct tw_server *server);
/* The output that shows this workspace, or NULL if it is hidden. */
struct tw_output *output_showing(struct tw_server *server, size_t workspace);
/* The output with the focus (the one showing the active workspace), or NULL. */
struct tw_output *output_focused(struct tw_server *server);
struct tw_output *output_at(struct tw_server *server, double lx, double ly);
/* The nearest output in a direction (enum wlr_direction), or NULL. */
struct tw_output *output_adjacent(struct tw_output *output, int direction);
/* `area` is what windows may use on the output, `full` the whole of it.
 * False if the output is NULL or has no room. */
bool output_boxes(struct tw_output *output, struct wlr_box *area, struct wlr_box *full);

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
/* Advances the scroll animation of the workspace on this output by one
 * frame; true if it needs another. */
bool strip_animate(struct tw_output *output);
/* Moves the focus to the workspace this output shows. */
void strip_focus_output(struct tw_server *server, struct tw_output *output);
/* A touchpad swipe drags the strip: `dx` is the finger movement in pixels. */
void strip_swipe_begin(struct tw_server *server);
void strip_swipe_update(struct tw_server *server, double dx, uint32_t time_msec);
/* Lets go: the strip glides to the nearest column edge. */
void strip_swipe_end(struct tw_server *server, bool cancelled);
/* Whether the window or layer surface this surface belongs to is on screen. */
bool surface_visible(struct wlr_surface *surface);

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

/* x11.c */
/* Sets DISPLAY for the programs tw starts, or removes it. Call before any. */
void x11_init(struct tw_server *server);
void x11_finish(struct tw_server *server);

/* input.c */
void input_init(struct tw_server *server);
void input_finish(struct tw_server *server);
/*
 * Re-derives which pointer lock or confinement is in force: the one of the
 * surface under the pointer, if that window also has the keyboard. Call
 * after the keyboard or pointer focus changes.
 */
void pointer_constraint_update(struct tw_server *server);

#endif
