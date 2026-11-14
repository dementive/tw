#include <stdlib.h>
#include <wlr/types/wlr_compositor.h>
#include <wlr/types/wlr_keyboard.h>
#include <wlr/types/wlr_scene.h>
#include <wlr/types/wlr_seat.h>
#include <wlr/types/wlr_xdg_decoration_v1.h>
#include <wlr/types/wlr_xdg_shell.h>
#include <wlr/util/edges.h>
#include <wlr/util/log.h>

#include "tw.h"
#include "config.h"

/*
 * Scene layout of one window:
 *
 *   scene_tree          positioned at the column; node.data = window
 *   ├── border[0..3]    solid rectangles
 *   ├── surface_tree    the client's surfaces, clipped to the column
 *   └── popup_tree      the client's popups, unclipped; node.data = window
 *
 * xdg_surface.data points at the tree its popups attach to: popup_tree for a
 * toplevel, the popup's own tree for a popup.
 */
struct tw_window {
	struct wl_list link; /* tw_server.windows; valid only while mapped */
	struct tw_server *server;
	struct wlr_xdg_toplevel *xdg_toplevel;
	struct wlr_xdg_toplevel_decoration_v1 *decoration; /* NULL if none */

	struct wlr_scene_tree *scene_tree;
	struct wlr_scene_tree *surface_tree;
	struct wlr_scene_tree *popup_tree;
	struct wlr_scene_rect *border[4]; /* top, bottom, left, right */

	bool mapped;
	size_t width_idx; /* index into column_widths */

	/* The last state sent to the client or the scene, so that re-arranging
	 * an unchanged strip sends no configure and causes no damage. */
	int width, height; /* content size, borders excluded */
	bool suspended;
	struct wlr_box clip;

	struct wl_listener map;
	struct wl_listener unmap;
	struct wl_listener commit;
	struct wl_listener destroy;
	struct wl_listener request_maximize;
	struct wl_listener request_fullscreen;
	/* Linked only while decoration != NULL. */
	struct wl_listener decoration_request_mode;
	struct wl_listener decoration_destroy;
};

struct tw_popup {
	struct tw_server *server;
	struct wlr_xdg_popup *xdg_popup;
	struct wl_listener commit;
	struct wl_listener destroy;
};

static struct tw_window *window_from_toplevel(struct wlr_xdg_toplevel *toplevel) {
	struct wlr_scene_tree *tree = toplevel->base->data;
	return tree != NULL ? tree->node.data : NULL;
}

struct tw_window *window_at(struct tw_server *server, double lx, double ly,
		struct wlr_surface **surface, double *sx, double *sy) {
	struct wlr_scene_node *node =
		wlr_scene_node_at(&server->scene->tree.node, lx, ly, sx, sy);
	if (node == NULL) {
		return NULL;
	}
	if (node->type == WLR_SCENE_NODE_BUFFER) {
		struct wlr_scene_surface *scene_surface =
			wlr_scene_surface_try_from_buffer(wlr_scene_buffer_from_node(node));
		if (scene_surface != NULL) {
			*surface = scene_surface->surface;
		}
	}
	for (struct wlr_scene_tree *tree = node->parent; tree != NULL; tree = tree->node.parent) {
		if (tree->node.data != NULL) {
			return tree->node.data;
		}
	}
	return NULL;
}

/* --- Geometry --------------------------------------------------------------- */

static int column_width(const struct tw_window *window, const struct wlr_box *area) {
	return tw_column_width(area->width, gap, column_widths[window->width_idx]);
}

static int content_size(int outer) {
	int inner = outer - 2 * border_width;
	return inner > 1 ? inner : 1;
}

static void window_update_borders(struct tw_window *window) {
	int bw = border_width, w = window->width, h = window->height;
	wlr_scene_rect_set_size(window->border[0], w + 2 * bw, bw);
	wlr_scene_rect_set_size(window->border[1], w + 2 * bw, bw);
	wlr_scene_rect_set_size(window->border[2], bw, h);
	wlr_scene_rect_set_size(window->border[3], bw, h);
	wlr_scene_node_set_position(&window->border[1]->node, 0, h + bw);
	wlr_scene_node_set_position(&window->border[2]->node, 0, bw);
	wlr_scene_node_set_position(&window->border[3]->node, w + bw, bw);
}

/* A client may ignore the size it was given; never let it draw outside its
 * column. The clip is in surface coordinates, hence the geometry offset. */
static void window_update_clip(struct tw_window *window) {
	const struct wlr_box *geometry = &window->xdg_toplevel->base->geometry;
	struct wlr_box clip = {
		.x = geometry->x,
		.y = geometry->y,
		.width = window->width,
		.height = window->height,
	};
	if (wlr_box_equal(&clip, &window->clip)) {
		return;
	}
	window->clip = clip;
	wlr_scene_subsurface_tree_set_clip(&window->surface_tree->node, &clip);
}

static void window_set_size(struct tw_window *window, int width, int height) {
	if (window->width == width && window->height == height) {
		return;
	}
	window->width = width;
	window->height = height;
	wlr_xdg_toplevel_set_size(window->xdg_toplevel, width, height);
	window_update_borders(window);
}

void strip_arrange(struct tw_server *server) {
	struct wlr_box area;
	if (!output_primary_box(server, &area)) {
		return;
	}
	struct tw_window *window;

	/* Scroll just far enough to show the focused column completely. */
	int x = gap;
	wl_list_for_each(window, &server->windows, link) {
		int width = column_width(window, &area);
		if (window == server->focused) {
			server->view_x = tw_view_fit(server->view_x, area.width, x, width, gap);
		}
		x += width + gap;
	}
	if (wl_list_empty(&server->windows)) {
		server->view_x = 0;
	}

	x = gap;
	wl_list_for_each(window, &server->windows, link) {
		int width = column_width(window, &area);
		bool visible = tw_column_visible(server->view_x, area.width, x, width);

		wlr_scene_node_set_position(&window->scene_tree->node,
			area.x + x - server->view_x, area.y + gap);
		window_set_size(window, content_size(width), content_size(area.height - 2 * gap));
		window_update_clip(window);

		/*
		 * The battery win of a scrolling layout: most windows are off-screen.
		 * A disabled node gets no frame callbacks, and `suspended` tells the
		 * client outright to stop drawing.
		 */
		wlr_scene_node_set_enabled(&window->scene_tree->node, visible);
		if (window->suspended == visible) {
			window->suspended = !visible;
			wlr_xdg_toplevel_set_suspended(window->xdg_toplevel, !visible);
		}

		x += width + gap;
	}
}

/* --- Focus ------------------------------------------------------------------ */

static void window_set_active(struct tw_window *window, bool active) {
	const float *color = active ? border_focused : border_unfocused;
	for (size_t i = 0; i < LENGTH(window->border); i++) {
		wlr_scene_rect_set_color(window->border[i], color);
	}
	wlr_xdg_toplevel_set_activated(window->xdg_toplevel, active);
}

void window_focus(struct tw_server *server, struct tw_window *window) {
	if (server->focused == window) {
		return;
	}
	if (server->focused != NULL) {
		window_set_active(server->focused, false);
	}
	server->focused = window;
	if (window == NULL) {
		wlr_seat_keyboard_notify_clear_focus(server->seat);
		return;
	}
	window_set_active(window, true);

	struct wlr_surface *surface = window->xdg_toplevel->base->surface;
	struct wlr_keyboard *keyboard = wlr_seat_get_keyboard(server->seat);
	if (keyboard != NULL) {
		wlr_seat_keyboard_notify_enter(server->seat, surface,
			keyboard->keycodes, keyboard->num_keycodes, &keyboard->modifiers);
	} else {
		wlr_seat_keyboard_notify_enter(server->seat, surface, NULL, 0, NULL);
	}
}

static struct tw_window *window_neighbour(struct tw_window *window, int direction) {
	struct wl_list *link = direction < 0 ? window->link.prev : window->link.next;
	if (link == &window->server->windows) {
		return NULL;
	}
	struct tw_window *neighbour = wl_container_of(link, neighbour, link);
	return neighbour;
}

/* --- Toplevel lifecycle ----------------------------------------------------- */

static void decoration_apply(struct tw_window *window) {
	/* We draw the borders, so ask clients not to draw title bars. A mode can
	 * only be sent once the surface has made its initial commit. */
	if (window->decoration != NULL && window->xdg_toplevel->base->initialized) {
		wlr_xdg_toplevel_decoration_v1_set_mode(window->decoration,
			WLR_XDG_TOPLEVEL_DECORATION_V1_MODE_SERVER_SIDE);
	}
}

static void decoration_detach(struct tw_window *window) {
	wl_list_remove(&window->decoration_request_mode.link);
	wl_list_remove(&window->decoration_destroy.link);
	window->decoration = NULL;
}

static void window_unmap(struct tw_window *window) {
	struct tw_server *server = window->server;
	struct tw_window *next = NULL;

	if (server->focused == window) {
		next = window_neighbour(window, -1);
		if (next == NULL) {
			next = window_neighbour(window, +1);
		}
		/* Cleared directly: this surface must not be sent a configure now. */
		server->focused = NULL;
	}
	wl_list_remove(&window->link);
	window->mapped = false;
	wlr_scene_node_set_enabled(&window->scene_tree->node, false);

	/* If the client maps again it starts from a fresh initial commit. */
	window->width = window->height = 0;
	window->suspended = false;
	window->clip = (struct wlr_box){0};

	if (next != NULL) {
		window_focus(server, next);
	} else if (server->focused == NULL) {
		wlr_seat_keyboard_notify_clear_focus(server->seat);
	}
	strip_arrange(server);
}

static void handle_map(struct wl_listener *listener, void *data) {
	struct tw_window *window = wl_container_of(listener, window, map);
	struct tw_server *server = window->server;

	/* A new column opens to the right of the focused one and never resizes
	 * the others. */
	struct wl_list *after = server->focused != NULL ?
		&server->focused->link : server->windows.prev;
	wl_list_insert(after, &window->link);
	window->mapped = true;

	window_focus(server, window);
	strip_arrange(server);

	const char *app_id = window->xdg_toplevel->app_id;
	wlr_log(WLR_DEBUG, "map %s %dx%d decorations=%s", app_id != NULL ? app_id : "(no app_id)",
		window->width, window->height, window->decoration != NULL ? "server" : "client");
}

static void handle_unmap(struct wl_listener *listener, void *data) {
	struct tw_window *window = wl_container_of(listener, window, unmap);
	window_unmap(window);
}

static void handle_commit(struct wl_listener *listener, void *data) {
	struct tw_window *window = wl_container_of(listener, window, commit);
	struct wlr_xdg_toplevel *toplevel = window->xdg_toplevel;

	if (toplevel->base->initial_commit) {
		/* Tell the client its column size before it draws anything, so the
		 * first frame already fits and nothing visibly resizes on map. */
		struct wlr_box area;
		wlr_xdg_toplevel_set_tiled(toplevel,
			WLR_EDGE_TOP | WLR_EDGE_BOTTOM | WLR_EDGE_LEFT | WLR_EDGE_RIGHT);
		decoration_apply(window);
		if (output_primary_box(window->server, &area)) {
			window_set_size(window, content_size(column_width(window, &area)),
				content_size(area.height - 2 * gap));
		} else {
			wlr_xdg_toplevel_set_size(toplevel, 0, 0);
		}
		return;
	}
	if (window->mapped) {
		window_update_clip(window);
	}
}

static void handle_destroy(struct wl_listener *listener, void *data) {
	struct tw_window *window = wl_container_of(listener, window, destroy);

	if (window->mapped) {
		window_unmap(window);
	}
	/* The decoration object can outlive this handler; it must not call back
	 * into listeners that live in the memory freed below. */
	if (window->decoration != NULL) {
		decoration_detach(window);
	}
	wl_list_remove(&window->map.link);
	wl_list_remove(&window->unmap.link);
	wl_list_remove(&window->commit.link);
	wl_list_remove(&window->destroy.link);
	wl_list_remove(&window->request_maximize.link);
	wl_list_remove(&window->request_fullscreen.link);

	window->xdg_toplevel->base->data = NULL;
	wlr_scene_node_destroy(&window->scene_tree->node);
	free(window);
}

static void handle_request_maximize(struct wl_listener *listener, void *data) {
	/* Not supported, but xdg-shell requires a configure in reply. */
	struct tw_window *window = wl_container_of(listener, window, request_maximize);
	if (window->xdg_toplevel->base->initialized) {
		wlr_xdg_surface_schedule_configure(window->xdg_toplevel->base);
	}
}

static void handle_request_fullscreen(struct wl_listener *listener, void *data) {
	struct tw_window *window = wl_container_of(listener, window, request_fullscreen);
	if (window->xdg_toplevel->base->initialized) {
		wlr_xdg_surface_schedule_configure(window->xdg_toplevel->base);
	}
}

static void handle_new_toplevel(struct wl_listener *listener, void *data) {
	struct tw_server *server = wl_container_of(listener, server, new_toplevel);
	struct wlr_xdg_toplevel *toplevel = data;

	struct tw_window *window = calloc(1, sizeof(*window));
	if (window == NULL) {
		wl_resource_post_no_memory(toplevel->resource);
		return;
	}
	window->server = server;
	window->xdg_toplevel = toplevel;
	window->width_idx = default_column_width;

	bool ok = (window->scene_tree = wlr_scene_tree_create(&server->scene->tree)) != NULL;
	for (size_t i = 0; ok && i < LENGTH(window->border); i++) {
		ok = (window->border[i] =
			wlr_scene_rect_create(window->scene_tree, 0, 0, border_unfocused)) != NULL;
	}
	ok = ok && (window->surface_tree =
		wlr_scene_xdg_surface_create(window->scene_tree, toplevel->base)) != NULL;
	ok = ok && (window->popup_tree = wlr_scene_tree_create(window->scene_tree)) != NULL;
	if (!ok) {
		if (window->scene_tree != NULL) {
			wlr_scene_node_destroy(&window->scene_tree->node);
		}
		free(window);
		wl_resource_post_no_memory(toplevel->resource);
		return;
	}

	/* Hidden until mapped and arranged. */
	wlr_scene_node_set_enabled(&window->scene_tree->node, false);
	wlr_scene_node_set_position(&window->surface_tree->node, border_width, border_width);
	wlr_scene_node_set_position(&window->popup_tree->node, border_width, border_width);
	window->scene_tree->node.data = window;
	window->popup_tree->node.data = window;
	toplevel->base->data = window->popup_tree;

	window->map.notify = handle_map;
	wl_signal_add(&toplevel->base->surface->events.map, &window->map);
	window->unmap.notify = handle_unmap;
	wl_signal_add(&toplevel->base->surface->events.unmap, &window->unmap);
	window->commit.notify = handle_commit;
	wl_signal_add(&toplevel->base->surface->events.commit, &window->commit);
	window->destroy.notify = handle_destroy;
	wl_signal_add(&toplevel->events.destroy, &window->destroy);
	window->request_maximize.notify = handle_request_maximize;
	wl_signal_add(&toplevel->events.request_maximize, &window->request_maximize);
	window->request_fullscreen.notify = handle_request_fullscreen;
	wl_signal_add(&toplevel->events.request_fullscreen, &window->request_fullscreen);
}

/* --- Decorations ------------------------------------------------------------ */

static void handle_decoration_request_mode(struct wl_listener *listener, void *data) {
	struct tw_window *window = wl_container_of(listener, window, decoration_request_mode);
	decoration_apply(window);
}

static void handle_decoration_destroy(struct wl_listener *listener, void *data) {
	struct tw_window *window = wl_container_of(listener, window, decoration_destroy);
	decoration_detach(window);
}

static void handle_new_decoration(struct wl_listener *listener, void *data) {
	struct wlr_xdg_toplevel_decoration_v1 *decoration = data;
	struct tw_window *window = window_from_toplevel(decoration->toplevel);
	if (window == NULL || window->decoration != NULL) {
		return;
	}
	window->decoration = decoration;
	window->decoration_request_mode.notify = handle_decoration_request_mode;
	wl_signal_add(&decoration->events.request_mode, &window->decoration_request_mode);
	window->decoration_destroy.notify = handle_decoration_destroy;
	wl_signal_add(&decoration->events.destroy, &window->decoration_destroy);
	decoration_apply(window);
}

/* --- Popups ----------------------------------------------------------------- */

static struct tw_window *popup_window(struct wlr_xdg_popup *popup) {
	struct wlr_surface *parent = popup->parent;
	while (parent != NULL) {
		struct wlr_xdg_surface *xdg_surface = wlr_xdg_surface_try_from_wlr_surface(parent);
		if (xdg_surface == NULL) {
			return NULL;
		}
		if (xdg_surface->role == WLR_XDG_SURFACE_ROLE_TOPLEVEL && xdg_surface->toplevel != NULL) {
			return window_from_toplevel(xdg_surface->toplevel);
		}
		if (xdg_surface->role != WLR_XDG_SURFACE_ROLE_POPUP || xdg_surface->popup == NULL) {
			return NULL;
		}
		parent = xdg_surface->popup->parent;
	}
	return NULL;
}

static void handle_popup_commit(struct wl_listener *listener, void *data) {
	struct tw_popup *popup = wl_container_of(listener, popup, commit);
	struct wlr_xdg_popup *xdg_popup = popup->xdg_popup;
	if (!xdg_popup->base->initial_commit) {
		return;
	}

	/* Keep the popup on the output: hand wlroots the output box, expressed
	 * relative to the window the popup belongs to. */
	struct tw_window *window = popup_window(xdg_popup);
	struct wlr_box area;
	int lx, ly;
	if (window != NULL && output_primary_box(popup->server, &area) &&
			wlr_scene_node_coords(&window->popup_tree->node, &lx, &ly)) {
		area.x -= lx;
		area.y -= ly;
		wlr_xdg_popup_unconstrain_from_box(xdg_popup, &area);
	}
	wlr_xdg_surface_schedule_configure(xdg_popup->base);
}

static void handle_popup_destroy(struct wl_listener *listener, void *data) {
	struct tw_popup *popup = wl_container_of(listener, popup, destroy);
	wl_list_remove(&popup->commit.link);
	wl_list_remove(&popup->destroy.link);
	free(popup);
}

static void handle_new_popup(struct wl_listener *listener, void *data) {
	struct tw_server *server = wl_container_of(listener, server, new_popup);
	struct wlr_xdg_popup *xdg_popup = data;

	struct tw_popup *popup = calloc(1, sizeof(*popup));
	if (popup == NULL) {
		wl_resource_post_no_memory(xdg_popup->resource);
		return;
	}
	popup->server = server;
	popup->xdg_popup = xdg_popup;

	/* A popup whose parent we do not manage is tracked but never shown. */
	struct wlr_xdg_surface *parent = xdg_popup->parent != NULL ?
		wlr_xdg_surface_try_from_wlr_surface(xdg_popup->parent) : NULL;
	if (parent != NULL && parent->data != NULL) {
		xdg_popup->base->data = wlr_scene_xdg_surface_create(parent->data, xdg_popup->base);
	}

	popup->commit.notify = handle_popup_commit;
	wl_signal_add(&xdg_popup->base->surface->events.commit, &popup->commit);
	popup->destroy.notify = handle_popup_destroy;
	wl_signal_add(&xdg_popup->events.destroy, &popup->destroy);
}

/* --- Actions ---------------------------------------------------------------- */

void action_close(struct tw_server *server, const union tw_arg *arg) {
	if (server->focused != NULL) {
		wlr_xdg_toplevel_send_close(server->focused->xdg_toplevel);
	}
}

void action_focus(struct tw_server *server, const union tw_arg *arg) {
	if (server->focused == NULL) {
		return;
	}
	struct tw_window *neighbour = window_neighbour(server->focused, arg->i);
	if (neighbour != NULL) {
		window_focus(server, neighbour);
		strip_arrange(server);
	}
}

void action_move(struct tw_server *server, const union tw_arg *arg) {
	struct tw_window *window = server->focused;
	if (window == NULL) {
		return;
	}
	struct tw_window *neighbour = window_neighbour(window, arg->i);
	if (neighbour == NULL) {
		return;
	}
	wl_list_remove(&window->link);
	wl_list_insert(arg->i < 0 ? neighbour->link.prev : &neighbour->link, &window->link);
	strip_arrange(server);
}

void action_cycle_width(struct tw_server *server, const union tw_arg *arg) {
	struct tw_window *window = server->focused;
	if (window == NULL) {
		return;
	}
	window->width_idx = (window->width_idx + 1) % LENGTH(column_widths);
	strip_arrange(server);
}

void window_init(struct tw_server *server) {
	wl_list_init(&server->windows);

	/* Version 6 is the first with the `suspended` state. */
	server->xdg_shell = wlr_xdg_shell_create(server->display, 6);
	server->new_toplevel.notify = handle_new_toplevel;
	wl_signal_add(&server->xdg_shell->events.new_toplevel, &server->new_toplevel);
	server->new_popup.notify = handle_new_popup;
	wl_signal_add(&server->xdg_shell->events.new_popup, &server->new_popup);

	server->decoration_manager = wlr_xdg_decoration_manager_v1_create(server->display);
	server->new_decoration.notify = handle_new_decoration;
	wl_signal_add(&server->decoration_manager->events.new_toplevel_decoration,
		&server->new_decoration);
}

void window_finish(struct tw_server *server) {
	wl_list_remove(&server->new_toplevel.link);
	wl_list_remove(&server->new_popup.link);
	wl_list_remove(&server->new_decoration.link);
}
