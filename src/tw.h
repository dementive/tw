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

struct tw_window;
struct wlr_surface;

struct tw_server {
	struct wl_display *display;
	struct wl_event_loop *loop;
	struct wl_event_source *sigint, *sigterm;

	struct wlr_backend *backend;
	struct wlr_session *session; /* NULL when nested or headless */
	struct wlr_renderer *renderer;
	struct wlr_allocator *allocator;

	/* output.c */
	struct wlr_output_layout *output_layout;
	struct wlr_scene *scene;
	struct wlr_scene_output_layout *scene_layout;
	struct wl_list outputs; /* tw_output.link; the first usable one shows the strip */
	struct wl_listener new_output;
	struct wl_listener layout_change;

	/* window.c */
	struct wlr_xdg_shell *xdg_shell;
	struct wlr_xdg_decoration_manager_v1 *decoration_manager;
	struct wl_listener new_toplevel;
	struct wl_listener new_popup;
	struct wl_listener new_decoration;
	struct wl_list windows; /* tw_window.link; the strip, left to right */
	struct tw_window *focused;
	int view_x; /* strip coordinate at the left edge of the output */

	/* input.c */
	struct wlr_seat *seat;
	struct wlr_cursor *cursor;
	struct wlr_xcursor_manager *cursor_manager;
	uint32_t mod; /* what TW_MOD resolves to */
	struct wl_list keyboards; /* tw_keyboard.link */
	struct wl_listener new_input;
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
/* Layout box of the output that shows the strip; false if there is none. */
bool output_primary_box(struct tw_server *server, struct wlr_box *box);

/* window.c */
void window_init(struct tw_server *server);
void window_finish(struct tw_server *server);
/*
 * Window under the layout coordinates, or NULL. *surface is set only when a
 * client surface (not a border) is under the point.
 */
struct tw_window *window_at(struct tw_server *server, double lx, double ly,
	struct wlr_surface **surface, double *sx, double *sy);
/* Moves keyboard focus; callers follow up with strip_arrange(). */
void window_focus(struct tw_server *server, struct tw_window *window);
/* Scrolls the focused column into view and places every window. */
void strip_arrange(struct tw_server *server);
void action_close(struct tw_server *server, const union tw_arg *arg);
void action_focus(struct tw_server *server, const union tw_arg *arg);
void action_move(struct tw_server *server, const union tw_arg *arg);
void action_cycle_width(struct tw_server *server, const union tw_arg *arg);

/* input.c */
void input_init(struct tw_server *server);
void input_finish(struct tw_server *server);

#endif
