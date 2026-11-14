#include <stdlib.h>
#include <wlr/types/wlr_compositor.h>
#include <wlr/types/wlr_keyboard.h>
#include <wlr/types/wlr_layer_shell_v1.h>
#include <wlr/types/wlr_output_layout.h>
#include <wlr/types/wlr_scene.h>
#include <wlr/types/wlr_seat.h>
#include <wlr/types/wlr_xdg_decoration_v1.h>
#include <wlr/types/wlr_xdg_shell.h>
#include <wlr/util/edges.h>
#include <wlr/util/log.h>

#include "tw.h"
#include "config.h"

/*
 * Scene layout of one window, under server->window_tree (or, while it is
 * fullscreen, server->fullscreen_tree, which sits above bars):
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
	struct wl_list link; /* tw_workspace.windows; valid only while mapped */
	struct tw_server *server;
	struct tw_workspace *workspace; /* NULL unless mapped */
	struct wlr_xdg_toplevel *xdg_toplevel;
	struct wlr_xdg_toplevel_decoration_v1 *decoration; /* NULL if none */

	struct wlr_scene_tree *scene_tree;
	struct wlr_scene_tree *surface_tree;
	struct wlr_scene_tree *popup_tree;
	struct wlr_scene_rect *border[4]; /* top, bottom, left, right */

	bool mapped;
	size_t width_idx; /* index into column_widths */
	bool maximized;   /* full width, overriding width_idx */
	bool fullscreen;  /* covers the whole output, no borders */

	/* The last state sent to the client or the scene, so that re-arranging
	 * an unchanged strip sends no configure and causes no damage. */
	int width, height; /* content size, borders excluded */
	bool suspended;
	bool fullscreen_applied;
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

bool surface_visible(struct wlr_surface *surface) {
	struct wlr_surface *root = wlr_surface_get_root_surface(surface);
	struct wlr_xdg_toplevel *toplevel = wlr_xdg_toplevel_try_from_wlr_surface(root);
	if (toplevel != NULL) {
		struct tw_window *window = window_from_toplevel(toplevel);
		return window != NULL && window->mapped && window->scene_tree->node.enabled;
	}
	return wlr_layer_surface_v1_try_from_wlr_surface(root) != NULL && root->mapped;
}

struct tw_window *window_at(struct tw_server *server, double lx, double ly,
		struct wlr_surface **surface, double *sx, double *sy) {
	/* While locked, only the lock screen can be pointed at. */
	struct wlr_scene_node *root = server->locked ?
		&server->lock_tree->node : &server->scene->tree.node;
	struct wlr_scene_node *node = wlr_scene_node_at(root, lx, ly, sx, sy);
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
	static const struct tw_fraction full = {1, 1};
	return tw_column_width(area->width, gap,
		window->maximized ? full : column_widths[window->width_idx]);
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

/*
 * The scene half of fullscreen: no borders, and a place above the bars. With
 * nothing else left to draw, wlroots hands the client's buffer straight to
 * the display instead of compositing it (direct scanout).
 */
static void window_apply_fullscreen_scene(struct tw_window *window, bool fullscreen) {
	struct tw_server *server = window->server;
	int inset = fullscreen ? 0 : border_width;
	for (size_t i = 0; i < LENGTH(window->border); i++) {
		wlr_scene_node_set_enabled(&window->border[i]->node, !fullscreen);
	}
	wlr_scene_node_set_position(&window->surface_tree->node, inset, inset);
	wlr_scene_node_set_position(&window->popup_tree->node, inset, inset);
	wlr_scene_node_reparent(&window->scene_tree->node,
		fullscreen ? server->fullscreen_tree : server->window_tree);
}

static void workspace_arrange(struct tw_workspace *workspace, const struct wlr_box *area,
		const struct wlr_box *full, bool shown) {
	struct tw_window *window;

	/* Scroll just far enough to show the focused column completely. A
	 * fullscreen column has no margin: it must cover the output exactly. */
	int x = gap;
	wl_list_for_each(window, &workspace->windows, link) {
		int width = window->fullscreen ? area->width : column_width(window, area);
		if (window == workspace->focused) {
			workspace->view_x = tw_view_fit(workspace->view_x, area->width, x, width,
				window->fullscreen ? 0 : gap);
		}
		x += width + gap;
	}
	if (wl_list_empty(&workspace->windows)) {
		workspace->view_x = 0;
	}

	x = gap;
	wl_list_for_each(window, &workspace->windows, link) {
		int width = window->fullscreen ? area->width : column_width(window, area);
		bool visible = shown &&
			tw_column_visible(workspace->view_x, area->width, x, width);

		if (window->fullscreen_applied != window->fullscreen) {
			window->fullscreen_applied = window->fullscreen;
			window_apply_fullscreen_scene(window, window->fullscreen);
			wlr_xdg_toplevel_set_fullscreen(window->xdg_toplevel, window->fullscreen);
		}
		if (window->fullscreen) {
			wlr_scene_node_set_position(&window->scene_tree->node,
				full->x + x - workspace->view_x, full->y);
			window_set_size(window, full->width, full->height);
		} else {
			wlr_scene_node_set_position(&window->scene_tree->node,
				area->x + x - workspace->view_x, area->y + gap);
			window_set_size(window, content_size(width),
				content_size(area->height - 2 * gap));
		}
		window_update_clip(window);

		/*
		 * The battery win of a scrolling layout: most windows are off-screen
		 * or on another workspace. A disabled node gets no frame callbacks,
		 * and `suspended` tells the client outright to stop drawing.
		 */
		wlr_scene_node_set_enabled(&window->scene_tree->node, visible);
		if (window->suspended == visible) {
			window->suspended = !visible;
			wlr_xdg_toplevel_set_suspended(window->xdg_toplevel, !visible);
		}

		x += width + gap;
	}
}

/* `area` is what windows may use, `full` the whole output (for fullscreen). */
static bool screen_boxes(struct tw_server *server, struct wlr_box *area, struct wlr_box *full) {
	struct tw_output *output = output_primary(server);
	if (output == NULL || !output_primary_box(server, area)) {
		return false;
	}
	wlr_output_layout_get_box(server->output_layout, output->wlr_output, full);
	return true;
}

void strip_arrange(struct tw_server *server) {
	struct wlr_box area, full;
	if (!screen_boxes(server, &area, &full)) {
		return;
	}

	/* Nothing is on screen while the outputs are off or the session is
	 * locked, so every window is suspended then. */
	bool screen_on = !server->blanked && !server->locked;
	for (size_t i = 0; i < TW_WORKSPACES; i++) {
		workspace_arrange(&server->workspaces[i], &area, &full,
			screen_on && i == server->active);
	}
	/* Visibility decides whether a video player may keep the screen on. */
	idle_update_inhibited(server);
}

/* --- Focus ------------------------------------------------------------------ */

static void window_set_active(struct tw_window *window, bool active) {
	const float *color = active ? border_focused : border_unfocused;
	for (size_t i = 0; i < LENGTH(window->border); i++) {
		wlr_scene_rect_set_color(window->border[i], color);
	}
	wlr_xdg_toplevel_set_activated(window->xdg_toplevel, active);
}

void focus_update(struct tw_server *server) {
	/* The active window is the focused one of the workspace on screen. It
	 * stays drawn as active while a layer surface borrows the keyboard. */
	struct tw_window *window = server->workspaces[server->active].focused;
	if (server->focused != window) {
		if (server->focused != NULL) {
			window_set_active(server->focused, false);
		}
		server->focused = window;
		if (window != NULL) {
			window_set_active(window, true);
		}
	}

	/* Keyboard: the lock screen if locked, else a layer surface that asked
	 * for it (a launcher), else the active window. */
	struct wlr_surface *surface;
	if (server->locked) {
		surface = lock_keyboard_surface(server);
	} else {
		surface = layer_keyboard_surface(server);
		if (surface == NULL && window != NULL) {
			surface = window->xdg_toplevel->base->surface;
		}
	}
	if (surface == server->seat->keyboard_state.focused_surface) {
		return;
	}
	if (surface == NULL) {
		wlr_seat_keyboard_notify_clear_focus(server->seat);
		return;
	}
	struct wlr_keyboard *keyboard = wlr_seat_get_keyboard(server->seat);
	if (keyboard != NULL) {
		wlr_seat_keyboard_notify_enter(server->seat, surface,
			keyboard->keycodes, keyboard->num_keycodes, &keyboard->modifiers);
	} else {
		wlr_seat_keyboard_notify_enter(server->seat, surface, NULL, 0, NULL);
	}
}

void window_focus(struct tw_server *server, struct tw_window *window) {
	window->workspace->focused = window;
	focus_update(server);
}

static struct tw_window *window_neighbour(struct tw_window *window, int direction) {
	struct wl_list *link = direction < 0 ? window->link.prev : window->link.next;
	if (link == &window->workspace->windows) {
		return NULL;
	}
	struct tw_window *neighbour = wl_container_of(link, neighbour, link);
	return neighbour;
}

/* Takes the window out of its workspace, handing that workspace's focus to
 * the column on its left, or failing that on its right. */
static void window_detach(struct tw_window *window) {
	struct tw_workspace *workspace = window->workspace;
	if (workspace->focused == window) {
		struct tw_window *next = window_neighbour(window, -1);
		if (next == NULL) {
			next = window_neighbour(window, +1);
		}
		workspace->focused = next;
	}
	wl_list_remove(&window->link);
	window->workspace = NULL;
}

/* Adds the window to a workspace as a new column to the right of the focused
 * one, and focuses it there. Existing columns are never resized. */
static void window_attach(struct tw_window *window, struct tw_workspace *workspace) {
	struct wl_list *after = workspace->focused != NULL ?
		&workspace->focused->link : workspace->windows.prev;
	wl_list_insert(after, &window->link);
	window->workspace = workspace;
	workspace->focused = window;
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

	window_detach(window);
	if (server->focused == window) {
		/* Cleared directly: this surface must not be sent a configure now. */
		server->focused = NULL;
	}
	window->mapped = false;
	wlr_scene_node_set_enabled(&window->scene_tree->node, false);

	/* If the client maps again it starts from a fresh initial commit. */
	window->width = window->height = 0;
	window->suspended = false;
	window->maximized = false;
	window->fullscreen = false;
	if (window->fullscreen_applied) {
		window->fullscreen_applied = false;
		window_apply_fullscreen_scene(window, false);
	}
	window->clip = (struct wlr_box){0};

	focus_update(server);
	strip_arrange(server);
}

static void handle_map(struct wl_listener *listener, void *data) {
	struct tw_window *window = wl_container_of(listener, window, map);
	struct tw_server *server = window->server;

	window_attach(window, &server->workspaces[server->active]);
	window->mapped = true;
	window->fullscreen = window->xdg_toplevel->requested.fullscreen;
	focus_update(server);
	strip_arrange(server);

	const char *app_id = window->xdg_toplevel->app_id;
	wlr_log(WLR_DEBUG, "map %s %dx%d workspace=%zu decorations=%s%s",
		app_id != NULL ? app_id : "(no app_id)", window->width, window->height,
		server->active + 1, window->decoration != NULL ? "server" : "client",
		window->fullscreen ? " fullscreen" : "");
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
		struct wlr_box area, full;
		wlr_xdg_toplevel_set_tiled(toplevel,
			WLR_EDGE_TOP | WLR_EDGE_BOTTOM | WLR_EDGE_LEFT | WLR_EDGE_RIGHT);
		decoration_apply(window);
		if (!screen_boxes(window->server, &area, &full)) {
			wlr_xdg_toplevel_set_size(toplevel, 0, 0);
		} else if (toplevel->requested.fullscreen) {
			/* It asked to start fullscreen (`mpv --fs`): size it for that. */
			window->fullscreen = window->fullscreen_applied = true;
			window_apply_fullscreen_scene(window, true);
			wlr_xdg_toplevel_set_fullscreen(toplevel, true);
			window_set_size(window, full.width, full.height);
		} else {
			window_set_size(window, content_size(column_width(window, &area)),
				content_size(area.height - 2 * gap));
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
	/* The client's own fullscreen button or key (F in a video player). */
	struct tw_window *window = wl_container_of(listener, window, request_fullscreen);
	struct wlr_xdg_toplevel *toplevel = window->xdg_toplevel;
	if (window->mapped) {
		window->fullscreen = toplevel->requested.fullscreen;
		strip_arrange(window->server);
	}
	/* xdg-shell wants a configure in reply even if nothing changed. A request
	 * made before mapping is picked up from `requested` when the window maps. */
	if (toplevel->base->initialized) {
		wlr_xdg_surface_schedule_configure(toplevel->base);
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

	bool ok = (window->scene_tree = wlr_scene_tree_create(server->window_tree)) != NULL;
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

	/* A layer surface's popup has no parent yet; layer.c gives it a scene
	 * tree when the client attaches it. Anything else stays invisible. */
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

static struct tw_window *active_window(struct tw_server *server) {
	return server->workspaces[server->active].focused;
}

void action_close(struct tw_server *server, const union tw_arg *arg) {
	struct tw_window *window = active_window(server);
	if (window != NULL) {
		wlr_xdg_toplevel_send_close(window->xdg_toplevel);
	}
}

void action_focus(struct tw_server *server, const union tw_arg *arg) {
	struct tw_window *window = active_window(server);
	struct tw_window *neighbour = window != NULL ? window_neighbour(window, arg->i) : NULL;
	if (neighbour != NULL) {
		window_focus(server, neighbour);
		strip_arrange(server);
	}
}

void action_focus_edge(struct tw_server *server, const union tw_arg *arg) {
	struct tw_workspace *workspace = &server->workspaces[server->active];
	if (wl_list_empty(&workspace->windows)) {
		return;
	}
	struct wl_list *link = arg->i < 0 ? workspace->windows.next : workspace->windows.prev;
	struct tw_window *window = wl_container_of(link, window, link);
	window_focus(server, window);
	strip_arrange(server);
}

void action_move(struct tw_server *server, const union tw_arg *arg) {
	struct tw_window *window = active_window(server);
	struct tw_window *neighbour = window != NULL ? window_neighbour(window, arg->i) : NULL;
	if (neighbour == NULL) {
		return;
	}
	wl_list_remove(&window->link);
	wl_list_insert(arg->i < 0 ? neighbour->link.prev : &neighbour->link, &window->link);
	strip_arrange(server);
}

void action_move_edge(struct tw_server *server, const union tw_arg *arg) {
	struct tw_window *window = active_window(server);
	if (window == NULL) {
		return;
	}
	struct wl_list *windows = &window->workspace->windows;
	wl_list_remove(&window->link);
	wl_list_insert(arg->i < 0 ? windows : windows->prev, &window->link);
	strip_arrange(server);
}

void action_cycle_width(struct tw_server *server, const union tw_arg *arg) {
	struct tw_window *window = active_window(server);
	if (window == NULL) {
		return;
	}
	/* Leaving the maximized state lands on the first preset. */
	window->width_idx = window->maximized ? 0 : (window->width_idx + 1) % LENGTH(column_widths);
	window->maximized = false;
	strip_arrange(server);
}

void action_maximize(struct tw_server *server, const union tw_arg *arg) {
	struct tw_window *window = active_window(server);
	if (window == NULL) {
		return;
	}
	window->maximized = !window->maximized;
	strip_arrange(server);
}

void action_fullscreen(struct tw_server *server, const union tw_arg *arg) {
	struct tw_window *window = active_window(server);
	if (window == NULL) {
		return;
	}
	window->fullscreen = !window->fullscreen;
	strip_arrange(server);
}

static void workspace_show(struct tw_server *server, size_t index) {
	if (index >= TW_WORKSPACES || index == server->active) {
		return;
	}
	server->active = index;
	focus_update(server);
	strip_arrange(server);
}

static void workspace_take_column(struct tw_server *server, size_t index) {
	struct tw_window *window = active_window(server);
	if (window == NULL || index >= TW_WORKSPACES || index == server->active) {
		return;
	}
	window_detach(window);
	window_attach(window, &server->workspaces[index]);
	/* The column stays the active window, so no focus events are sent. */
	server->active = index;
	focus_update(server);
	strip_arrange(server);
}

/* Index of the neighbouring workspace, or TW_WORKSPACES if there is none. */
static size_t workspace_relative(struct tw_server *server, int direction) {
	if (direction < 0) {
		return server->active > 0 ? server->active - 1 : TW_WORKSPACES;
	}
	return server->active + 1;
}

void action_workspace(struct tw_server *server, const union tw_arg *arg) {
	workspace_show(server, (size_t)arg->i);
}

void action_workspace_relative(struct tw_server *server, const union tw_arg *arg) {
	workspace_show(server, workspace_relative(server, arg->i));
}

void action_move_to_workspace(struct tw_server *server, const union tw_arg *arg) {
	workspace_take_column(server, (size_t)arg->i);
}

void action_move_to_workspace_relative(struct tw_server *server, const union tw_arg *arg) {
	workspace_take_column(server, workspace_relative(server, arg->i));
}

void window_init(struct tw_server *server) {
	for (size_t i = 0; i < TW_WORKSPACES; i++) {
		wl_list_init(&server->workspaces[i].windows);
	}

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
