#include <math.h>
#include <stdlib.h>
#include <wlr/types/wlr_compositor.h>
#include <wlr/types/wlr_keyboard.h>
#include <wlr/types/wlr_layer_shell_v1.h>
#include <wlr/types/wlr_output.h>
#include <wlr/types/wlr_output_layout.h>
#include <wlr/types/wlr_scene.h>
#include <wlr/types/wlr_seat.h>
#include <wlr/types/wlr_xdg_activation_v1.h>
#include <wlr/types/wlr_xdg_decoration_v1.h>
#include <wlr/types/wlr_xdg_shell.h>
#include <wlr/util/edges.h>
#include <wlr/util/log.h>

#include "tw.h"
#include "config.h"

/*
 * A workspace is a strip of columns, left to right. A column is a stack of
 * windows, top to bottom, that share the column's width and split its height.
 * A column is never empty: removing its last window frees it.
 */
struct tw_column {
	struct wl_list link; /* tw_workspace.columns */
	struct tw_workspace *workspace;
	struct wl_list windows; /* tw_window.link */
	struct tw_window *active; /* the window focused when the column is */
	size_t width_idx; /* index into column_widths */
	bool maximized;   /* full width, overriding width_idx */
};

/*
 * Scene layout of one window. Its scene_tree lives under server->window_tree,
 * or fullscreen_tree while fullscreen, or float_tree while floating.
 *
 *   scene_tree          positioned at the window; node.data = window
 *   ├── surface_tree    the client's surfaces, clipped to the window's box
 *   └── popup_tree      the client's popups, unclipped; node.data = window
 *
 * xdg_surface.data points at the tree its popups attach to: popup_tree for a
 * toplevel, the popup's own tree for a popup.
 */
struct tw_window {
	/* tw_column.windows, or tw_workspace.floating; valid only while mapped. */
	struct wl_list link;
	struct tw_server *server;
	struct tw_workspace *workspace; /* NULL unless mapped */
	struct tw_column *column;       /* NULL unless mapped and tiled */
	struct wlr_xdg_toplevel *xdg_toplevel;
	struct wlr_xdg_toplevel_decoration_v1 *decoration; /* NULL if none */

	struct wlr_scene_tree *scene_tree;
	struct wlr_scene_tree *surface_tree;
	struct wlr_scene_tree *popup_tree;

	bool mapped;
	bool floating;   /* a dialog: centred, sized by the client, not in the strip */
	bool fullscreen; /* covers the whole output; alone in its column */

	/* The last state sent to the client or the scene, so that re-arranging
	 * an unchanged strip sends no configure and causes no damage. */
	int width, height;
	bool suspended;
	bool fullscreen_applied;
	struct wlr_box clip;
	/* How much of the window's left and right side is cut off, because it
	 * sticks out of its output toward a neighbouring one. */
	int cut_left, cut_right;

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

/* --- Columns ---------------------------------------------------------------- */

static struct tw_column *column_neighbour(struct tw_column *column, int direction) {
	struct wl_list *link = direction < 0 ? column->link.prev : column->link.next;
	if (link == &column->workspace->columns) {
		return NULL;
	}
	struct tw_column *neighbour = wl_container_of(link, neighbour, link);
	return neighbour;
}

/* The window above (-1) or below (+1) in the same column. */
static struct tw_window *window_sibling(struct tw_window *window, int direction) {
	struct wl_list *link = direction < 0 ? window->link.prev : window->link.next;
	if (link == &window->column->windows) {
		return NULL;
	}
	struct tw_window *sibling = wl_container_of(link, sibling, link);
	return sibling;
}

static struct tw_window *column_first(struct tw_column *column) {
	struct tw_window *window = wl_container_of(column->windows.next, window, link);
	return window;
}

static bool column_single(struct tw_column *column) {
	return column->windows.next == column->windows.prev;
}

/* A fullscreen window is always alone in its column. */
static bool column_fullscreen(struct tw_column *column) {
	return column_first(column)->fullscreen;
}

/* An empty column linked after `after`; the caller must put a window in it. */
static struct tw_column *column_new(struct tw_workspace *workspace, struct wl_list *after) {
	struct tw_column *column = calloc(1, sizeof(*column));
	if (column == NULL) {
		return NULL;
	}
	column->workspace = workspace;
	column->width_idx = default_column_width;
	wl_list_init(&column->windows);
	wl_list_insert(after, &column->link);
	return column;
}

static void column_insert(struct tw_column *column, struct tw_window *window,
		struct wl_list *after) {
	wl_list_insert(after, &window->link);
	window->column = column;
	window->workspace = column->workspace;
}

/*
 * Unlinks the window from its column, freeing the column if that empties it.
 * Returns the window that takes its place: the one below, else above, else
 * the active window of the column to the left, else right; NULL if the strip
 * is now empty. Focus pointers are the caller's business.
 */
static struct tw_window *column_take(struct tw_window *window) {
	struct tw_column *column = window->column;
	struct tw_window *next = window_sibling(window, +1);
	if (next == NULL) {
		next = window_sibling(window, -1);
	}
	wl_list_remove(&window->link);
	window->column = NULL;

	if (next != NULL) {
		if (column->active == window) {
			column->active = next;
		}
		return next;
	}
	struct tw_column *other = column_neighbour(column, -1);
	if (other == NULL) {
		other = column_neighbour(column, +1);
	}
	wl_list_remove(&column->link);
	free(column);
	return other != NULL ? other->active : NULL;
}

/* Moves a window that shares its column into a new column on that side. */
static bool window_expel(struct tw_window *window, int direction) {
	struct tw_column *old = window->column;
	struct tw_column *column = column_new(old->workspace,
		direction < 0 ? old->link.prev : &old->link);
	if (column == NULL) {
		return false;
	}
	column->width_idx = old->width_idx;
	column_take(window);
	column_insert(column, window, &column->windows);
	column->active = window;
	return true;
}

/* Moves a window to the bottom of another column. */
static void window_join(struct tw_window *window, struct tw_column *column) {
	column_first(column)->fullscreen = false;
	window->fullscreen = false;
	column_take(window);
	column_insert(column, window, column->windows.prev);
}

static void window_set_fullscreen(struct tw_window *window, bool fullscreen) {
	if (window->floating || window->fullscreen == fullscreen) {
		return;
	}
	if (fullscreen && !column_single(window->column) && !window_expel(window, +1)) {
		return;
	}
	window->fullscreen = fullscreen;
}

/* --- Geometry --------------------------------------------------------------- */

static int column_width(const struct tw_column *column, const struct wlr_box *area) {
	static const struct tw_fraction full = {1, 1};
	return tw_column_width(area->width, gap,
		column->maximized ? full : column_widths[column->width_idx]);
}

static int at_least_zero(int value) {
	return value > 0 ? value : 0;
}

/* A client may ignore the size it was given; never let it draw outside its
 * box, or outside its output. The clip is in surface coordinates, hence the
 * geometry offset. */
static void window_update_clip(struct tw_window *window) {
	const struct wlr_box *geometry = &window->xdg_toplevel->base->geometry;
	int left = window->cut_left, right = window->cut_right;
	struct wlr_box clip = {
		.x = geometry->x + left,
		.y = geometry->y,
		.width = at_least_zero(window->width - left - right),
		.height = window->height,
	};
	if (wlr_box_equal(&clip, &window->clip)) {
		return;
	}
	window->clip = clip;
	/* An empty clip would mean "do not clip". */
	wlr_scene_node_set_enabled(&window->surface_tree->node, clip.width > 0);
	if (clip.width > 0) {
		wlr_scene_subsurface_tree_set_clip(&window->surface_tree->node, &clip);
	}
}

/* Cuts this much off the window's sides; see tw_window.cut_left. Takes
 * effect with the next window_update_clip(). */
static void window_trim(struct tw_window *window, int left, int right) {
	window->cut_left = at_least_zero(left);
	window->cut_right = at_least_zero(right);
}

/* Records the window's size. With `configure` the client is also asked for
 * that size; a floating window picks its own. */
static void window_resize(struct tw_window *window, int width, int height, bool configure) {
	if (window->width == width && window->height == height) {
		return;
	}
	window->width = width;
	window->height = height;
	if (configure) {
		wlr_xdg_toplevel_set_size(window->xdg_toplevel, width, height);
	}
}

/*
 * `on_screen`: draw it. `wanted`: it is, or is about to be, on screen, so the
 * client should be drawing. The two differ while the strip is moving.
 *
 * The battery win of a scrolling layout: most windows are off-screen or on
 * another workspace. A disabled node gets no frame callbacks, and `suspended`
 * tells the client outright to stop drawing.
 */
static void window_show(struct tw_window *window, bool on_screen, bool wanted) {
	wlr_scene_node_set_enabled(&window->scene_tree->node, on_screen);
	if (window->suspended == wanted) {
		window->suspended = !wanted;
		wlr_xdg_toplevel_set_suspended(window->xdg_toplevel, !wanted);
	}
}

/*
 * The scene half of fullscreen: a place above the bars. With nothing else
 * left to draw, wlroots hands the client's buffer straight to the display
 * instead of compositing it (direct scanout).
 */
static void window_apply_fullscreen_scene(struct tw_window *window, bool fullscreen) {
	struct tw_server *server = window->server;
	wlr_scene_node_reparent(&window->scene_tree->node,
		fullscreen ? server->fullscreen_tree : server->window_tree);
}

/* A dialog: has a parent, or cannot be resized. */
static bool wants_float(struct wlr_xdg_toplevel *toplevel) {
	const struct wlr_xdg_toplevel_state *state = &toplevel->current;
	return toplevel->parent != NULL ||
		(state->min_width > 0 && state->min_height > 0 &&
		state->min_width == state->max_width && state->min_height == state->max_height);
}

/* Centres a floating window, at whatever size its client chose. */
static void float_place(struct tw_window *window, const struct wlr_box *area) {
	const struct wlr_box *geometry = &window->xdg_toplevel->base->geometry;
	int width = geometry->width > 0 ? geometry->width : 1;
	int height = geometry->height > 0 ? geometry->height : 1;
	window_resize(window, width, height, false);
	window_update_clip(window);

	int x = area->x + (area->width - width) / 2;
	int y = area->y + (area->height - height) / 2;
	wlr_scene_node_set_position(&window->scene_tree->node,
		x > area->x ? x : area->x, y > area->y ? y : area->y);
}

/* The output boxes for a workspace: `area` is what its windows may use,
 * `full` the whole output (for fullscreen). False if no output shows it. */
static bool workspace_boxes(struct tw_server *server, const struct tw_workspace *workspace,
		struct wlr_box *area, struct wlr_box *full) {
	return output_boxes(output_showing(server, (size_t)(workspace - server->workspaces)),
		area, full);
}

/*
 * Puts every window of the workspace where the current view position says it
 * goes. Runs on every arrange and on every frame of a scroll animation, so
 * everything in it is a cached no-op when nothing changed.
 */
static void workspace_place(struct tw_server *server, struct tw_workspace *workspace,
		const struct wlr_box *area, const struct wlr_box *full, bool shown) {
	int view = (int)lround(workspace->view_pos);
	int x = gap;
	struct tw_column *column;
	struct tw_window *window;
	/* A column sticking out past the edge of this output would show up on
	 * the one next to it. With a single output there is nothing to trim. */
	bool trim = wl_list_length(&server->outputs) > 1;
	int edge_left = full->x, edge_right = full->x + full->width;

	wl_list_for_each(column, &workspace->columns, link) {
		bool fullscreen = column_fullscreen(column);
		int width = fullscreen ? area->width : column_width(column, area);
		bool on_screen = shown && tw_column_visible(view, area->width, x, width);
		/* Also wake the windows the view is heading for. */
		bool wanted = on_screen ||
			(shown && tw_column_visible(workspace->view_x, area->width, x, width));

		int count = wl_list_length(&column->windows), index = 0;
		int avail = area->height - 2 * gap;
		int y = area->y + gap;
		wl_list_for_each(window, &column->windows, link) {
			if (window->fullscreen_applied != window->fullscreen) {
				window->fullscreen_applied = window->fullscreen;
				window_apply_fullscreen_scene(window, window->fullscreen);
				wlr_xdg_toplevel_set_fullscreen(window->xdg_toplevel, window->fullscreen);
			}
			int left; /* where the window's outer box starts, and how wide it is */
			int outer;
			if (window->fullscreen) {
				left = full->x + x - view;
				outer = full->width;
				wlr_scene_node_set_position(&window->scene_tree->node, left, full->y);
				window_resize(window, full->width, full->height, true);
			} else {
				int height = tw_stack_height(avail, gap, count, index);
				left = area->x + x - view;
				outer = width;
				wlr_scene_node_set_position(&window->scene_tree->node, left, y);
				window_resize(window, width, height, true);
				y += height + gap;
			}
			window_trim(window, trim ? edge_left - left : 0,
				trim ? left + outer - edge_right : 0);
			window_update_clip(window);
			window_show(window, on_screen, wanted);
			index++;
		}
		x += width + gap;
	}

	wl_list_for_each(window, &workspace->floating, link) {
		float_place(window, area);
		window_show(window, shown, shown);
	}
}

/* Takes every window of a workspace that no output shows off screen. Its
 * layout is left alone until it is shown again: sizes depend on the output. */
static void workspace_hide(struct tw_workspace *workspace) {
	struct tw_column *column;
	struct tw_window *window;
	wl_list_for_each(column, &workspace->columns, link) {
		wl_list_for_each(window, &column->windows, link) {
			window_show(window, false, false);
		}
	}
	wl_list_for_each(window, &workspace->floating, link) {
		window_show(window, false, false);
	}
	workspace->on_output = false;
}

static void workspace_arrange(struct tw_server *server, struct tw_workspace *workspace,
		const struct wlr_box *area, const struct wlr_box *full, bool shown) {
	/* Scroll just far enough to show the focused column completely. A
	 * fullscreen column has no margin: it must cover the output exactly.
	 * During a swipe the fingers decide instead. */
	if (!workspace->swiping) {
		int x = gap;
		struct tw_column *column;
		wl_list_for_each(column, &workspace->columns, link) {
			bool fullscreen = column_fullscreen(column);
			int width = fullscreen ? area->width : column_width(column, area);
			if (workspace->tiled != NULL && workspace->tiled->column == column) {
				workspace->view_x = tw_view_fit(workspace->view_x, area->width, x, width,
					fullscreen ? 0 : gap);
			}
			x += width + gap;
		}
		/* x is now the width of the whole strip. Closing or merging columns
		 * shortens it; scroll back rather than show blank space. */
		workspace->view_x = tw_view_clamp(workspace->view_x, area->width, x);
		/* Only a workspace that is and was on screen is worth animating:
		 * one that just appeared is simply there. */
		if (!shown || !workspace->on_output || scroll_stiffness <= 0) {
			workspace->view_pos = workspace->view_x;
			workspace->view_velocity = 0;
		}
	}
	workspace->on_output = true;
	workspace_place(server, workspace, area, full, shown);
}

void strip_arrange(struct tw_server *server) {
	/* Nothing is on screen while the outputs are off or the session is
	 * locked, so every window is suspended then. */
	bool screen_on = !server->blanked && !server->locked;
	for (size_t i = 0; i < TW_WORKSPACES; i++) {
		struct tw_workspace *workspace = &server->workspaces[i];
		struct tw_output *output = output_showing(server, i);
		struct wlr_box area, full;
		if (!output_boxes(output, &area, &full)) {
			workspace_hide(workspace);
			continue;
		}
		workspace_arrange(server, workspace, &area, &full, screen_on);

		/* If the view is not where it should be, glide there. The animation
		 * is stepped from the output's frame handler, so it costs nothing
		 * once it has arrived. */
		if (screen_on && !workspace->swiping && workspace->view_pos != workspace->view_x) {
			if (!output->animating) {
				output->animating = true;
				output->animation_last_ms = now_ms();
			}
			wlr_output_schedule_frame(output->wlr_output);
		}
	}
	/* Visibility decides whether a video player may keep the screen on. */
	idle_update_inhibited(server);
}

bool strip_animate(struct tw_output *output) {
	struct tw_server *server = output->server;
	if (!output->animating) {
		return false;
	}
	struct wlr_box area, full;
	int64_t now = now_ms();
	double dt = (double)(now - output->animation_last_ms) / 1000.0;
	output->animation_last_ms = now;

	bool screen_on = !server->blanked && !server->locked;
	struct tw_workspace *workspace = output->workspace < TW_WORKSPACES ?
		&server->workspaces[output->workspace] : NULL;
	if (workspace == NULL || workspace->swiping || !screen_on ||
			!output_boxes(output, &area, &full)) {
		output->animating = false;
		return false;
	}
	tw_spring_step(&workspace->view_pos, &workspace->view_velocity, workspace->view_x,
		sqrt(scroll_stiffness), dt < 0.1 ? dt : 0.1);
	if (tw_spring_settled(workspace->view_pos, workspace->view_velocity, workspace->view_x)) {
		workspace->view_pos = workspace->view_x;
		workspace->view_velocity = 0;
		output->animating = false;
	}
	workspace_place(server, workspace, &area, &full, true);
	idle_update_inhibited(server);
	return output->animating;
}

/* --- Touchpad swipe --------------------------------------------------------- */

void strip_swipe_begin(struct tw_server *server) {
	struct tw_workspace *workspace = &server->workspaces[server->active];
	struct tw_output *output = output_focused(server);
	workspace->swiping = true;
	workspace->view_velocity = 0;
	if (output != NULL) {
		output->animating = false;
	}
}

void strip_swipe_update(struct tw_server *server, double dx, uint32_t time_msec) {
	struct tw_workspace *workspace = &server->workspaces[server->active];
	struct wlr_box area, full;
	if (!workspace->swiping || !workspace_boxes(server, workspace, &area, &full)) {
		return;
	}
	/* The strip follows the fingers, so the view moves the other way. Keep a
	 * smoothed speed for the glide after they lift. */
	double dt = (double)(time_msec - workspace->swipe_time_msec) / 1000.0;
	if (dt > 0 && dt < 0.1) {
		workspace->view_velocity = 0.5 * workspace->view_velocity + 0.5 * (-dx / dt);
	}
	workspace->swipe_time_msec = time_msec;
	workspace->view_pos -= dx;

	workspace_place(server, workspace, &area, &full, !server->blanked && !server->locked);
	idle_update_inhibited(server);
}

/*
 * Picks where the strip comes to rest after a swipe: the column edge nearest
 * to where it would have coasted. Focus then follows the view, staying on the
 * focused column if it is still entirely visible.
 */
static void workspace_snap(struct tw_workspace *workspace, const struct wlr_box *area) {
	double projected = workspace->view_pos + workspace->view_velocity * 0.15;
	struct tw_column *column;

	int total = gap;
	wl_list_for_each(column, &workspace->columns, link) {
		total += (column_fullscreen(column) ? area->width : column_width(column, area)) + gap;
	}

	/* Candidates: each column flush left, and the far end of the strip. */
	int best = tw_view_clamp(total, area->width, total);
	int x = gap;
	wl_list_for_each(column, &workspace->columns, link) {
		bool fullscreen = column_fullscreen(column);
		int candidate = tw_view_clamp(fullscreen ? x : x - gap, area->width, total);
		if (fabs(candidate - projected) < fabs(best - projected)) {
			best = candidate;
		}
		x += (fullscreen ? area->width : column_width(column, area)) + gap;
	}
	workspace->view_x = best;

	struct tw_column *focused = workspace->tiled != NULL ? workspace->tiled->column : NULL;
	struct tw_column *first_whole = NULL, *first_partial = NULL;
	x = gap;
	wl_list_for_each(column, &workspace->columns, link) {
		bool fullscreen = column_fullscreen(column);
		int width = fullscreen ? area->width : column_width(column, area);
		int margin = fullscreen ? 0 : gap;
		if (x - margin >= best && x + width + margin <= best + area->width) {
			if (column == focused) {
				return;
			}
			if (first_whole == NULL) {
				first_whole = column;
			}
		} else if (first_partial == NULL && tw_column_visible(best, area->width, x, width)) {
			first_partial = column;
		}
		x += width + gap;
	}
	struct tw_column *target = first_whole != NULL ? first_whole : first_partial;
	if (target != NULL) {
		workspace->tiled = workspace->focused = target->active;
	}
}

void strip_swipe_end(struct tw_server *server, bool cancelled) {
	struct tw_workspace *workspace = &server->workspaces[server->active];
	struct wlr_box area, full;
	if (!workspace->swiping) {
		return;
	}
	workspace->swiping = false;
	/* A cancelled swipe glides back to where it started. */
	if (!cancelled && workspace_boxes(server, workspace, &area, &full)) {
		workspace_snap(workspace, &area);
	}
	focus_update(server);
	strip_arrange(server);
}

/* --- Focus ------------------------------------------------------------------ */

/* Nothing is drawn around the active window; its client is told, and shows
 * it in its own way (a text cursor, a brighter title). */
static void window_set_active(struct tw_window *window, bool active) {
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
	struct wlr_keyboard *keyboard = wlr_seat_get_keyboard(server->seat);
	if (surface == NULL) {
		wlr_seat_keyboard_notify_clear_focus(server->seat);
	} else if (keyboard != NULL) {
		wlr_seat_keyboard_notify_enter(server->seat, surface,
			keyboard->keycodes, keyboard->num_keycodes, &keyboard->modifiers);
	} else {
		wlr_seat_keyboard_notify_enter(server->seat, surface, NULL, 0, NULL);
	}
	/* A pointer lock belongs to the window with the keyboard. */
	pointer_constraint_update(server);
}

/* Makes a workspace the active one: on the output that already shows it, or
 * else on the focused output, which then shows it in place of its own. */
static void workspace_activate(struct tw_server *server, size_t index) {
	if (output_showing(server, index) == NULL) {
		struct tw_output *output = output_focused(server);
		if (output != NULL) {
			output->workspace = index;
		}
	}
	server->active = index;
}

void window_focus(struct tw_server *server, struct tw_window *window) {
	struct tw_workspace *workspace = window->workspace;
	workspace_activate(server, (size_t)(workspace - server->workspaces));
	workspace->focused = window;
	if (window->floating) {
		/* The focused dialog is on top of the others. */
		wl_list_remove(&window->link);
		wl_list_insert(&workspace->floating, &window->link);
		wlr_scene_node_raise_to_top(&window->scene_tree->node);
	} else {
		workspace->tiled = window;
		window->column->active = window;
	}
	focus_update(server);
}

/*
 * A window asks for the focus (xdg-activation): a link was clicked and the
 * browser wants to come forward, or a notification was.
 */
static void handle_request_activate(struct wl_listener *listener, void *data) {
	struct tw_server *server = wl_container_of(listener, server, request_activate);
	struct wlr_xdg_activation_v1_request_activate_event *event = data;

	/* It gets the focus only with a token made from an input event: proof
	 * that the user did something in the program that issued it. wlroots
	 * has checked that the event was real and went to a focused surface. A
	 * token without one can be made by any client at any time, so honouring
	 * it would let any window steal the focus. */
	if (event->token->seat == NULL || server->locked) {
		return;
	}
	struct wlr_xdg_toplevel *toplevel = wlr_xdg_toplevel_try_from_wlr_surface(event->surface);
	struct tw_window *window = toplevel != NULL ? window_from_toplevel(toplevel) : NULL;
	/* A window that is not mapped yet takes the focus when it maps anyway. */
	if (window == NULL || !window->mapped) {
		return;
	}
	window_focus(server, window);
	strip_arrange(server);
}

/* --- Toplevel lifecycle ----------------------------------------------------- */

static void decoration_apply(struct tw_window *window) {
	/* Windows are tiled and moved by key, so ask clients not to draw title
	 * bars. A mode can only be sent once the surface has made its initial
	 * commit. */
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

static struct tw_window *first_floating(struct tw_workspace *workspace) {
	if (wl_list_empty(&workspace->floating)) {
		return NULL;
	}
	struct tw_window *window = wl_container_of(workspace->floating.next, window, link);
	return window;
}

/* Takes the window out of its workspace and passes its focus on: a closed
 * dialog to the next dialog or back to the strip, a closed tiled window to
 * whatever took its place. */
static void window_remove(struct tw_window *window) {
	struct tw_workspace *workspace = window->workspace;
	struct tw_window *next;
	if (window->floating) {
		wl_list_remove(&window->link);
		next = first_floating(workspace);
		if (next == NULL) {
			next = workspace->tiled;
		}
	} else {
		next = column_take(window);
		if (workspace->tiled == window) {
			workspace->tiled = next;
		}
		if (next == NULL) {
			next = first_floating(workspace);
		}
	}
	if (workspace->focused == window) {
		workspace->focused = next;
	}
	window->workspace = NULL;
}

static void window_unmap(struct tw_window *window) {
	struct tw_server *server = window->server;

	window_remove(window);
	if (server->focused == window) {
		/* Cleared directly: this surface must not be sent a configure now. */
		server->focused = NULL;
	}
	window->mapped = false;
	wlr_scene_node_set_enabled(&window->scene_tree->node, false);

	/* If the client maps again it starts from a fresh initial commit. */
	window->width = window->height = 0;
	window->suspended = false;
	window->fullscreen = false;
	if (window->fullscreen_applied) {
		window->fullscreen_applied = false;
		window_apply_fullscreen_scene(window, false);
	}
	if (window->floating) {
		window->floating = false;
		wlr_scene_node_reparent(&window->scene_tree->node, server->window_tree);
	}
	window->clip = (struct wlr_box){0};
	wlr_scene_node_set_enabled(&window->surface_tree->node, true);
	window_trim(window, 0, 0);

	focus_update(server);
	strip_arrange(server);
}

static void handle_map(struct wl_listener *listener, void *data) {
	struct tw_window *window = wl_container_of(listener, window, map);
	struct tw_server *server = window->server;
	struct tw_workspace *workspace = &server->workspaces[server->active];

	if (window->floating) {
		window->workspace = workspace;
		wl_list_insert(&workspace->floating, &window->link);
		wlr_scene_node_reparent(&window->scene_tree->node, server->float_tree);
	} else {
		/* A new column opens to the right of the focused one and never
		 * resizes the others. */
		struct tw_column *column = column_new(workspace, workspace->tiled != NULL ?
			&workspace->tiled->column->link : workspace->columns.prev);
		if (column == NULL) {
			wlr_log(WLR_ERROR, "out of memory, closing a new window");
			wlr_xdg_toplevel_send_close(window->xdg_toplevel);
			return;
		}
		column_insert(column, window, &column->windows);
		column->active = window;
		window->fullscreen = window->xdg_toplevel->requested.fullscreen;
		workspace->tiled = window;
	}
	window->mapped = true;
	workspace->focused = window;
	focus_update(server);
	strip_arrange(server);

	const char *app_id = window->xdg_toplevel->app_id;
	wlr_log(WLR_DEBUG, "map %s %dx%d workspace=%zu decorations=%s%s%s",
		app_id != NULL ? app_id : "(no app_id)", window->width, window->height,
		server->active + 1, window->decoration != NULL ? "server" : "client",
		window->fullscreen ? " fullscreen" : "", window->floating ? " floating" : "");
}

static void handle_unmap(struct wl_listener *listener, void *data) {
	struct tw_window *window = wl_container_of(listener, window, unmap);
	if (window->mapped) {
		window_unmap(window);
	}
}

static void handle_commit(struct wl_listener *listener, void *data) {
	struct tw_window *window = wl_container_of(listener, window, commit);
	struct wlr_xdg_toplevel *toplevel = window->xdg_toplevel;
	struct wlr_box area, full;

	if (toplevel->base->initial_commit) {
		window->floating = wants_float(toplevel);
		decoration_apply(window);
		struct tw_server *server = window->server;
		/* It will map on the active workspace: size it for that output. */
		if (window->floating ||
				!workspace_boxes(server, &server->workspaces[server->active], &area, &full)) {
			/* A dialog picks its own size. */
			wlr_xdg_toplevel_set_size(toplevel, 0, 0);
			return;
		}
		/* Tell the client its column size before it draws anything, so the
		 * first frame already fits and nothing visibly resizes on map. */
		wlr_xdg_toplevel_set_tiled(toplevel,
			WLR_EDGE_TOP | WLR_EDGE_BOTTOM | WLR_EDGE_LEFT | WLR_EDGE_RIGHT);
		if (toplevel->requested.fullscreen) {
			/* It asked to start fullscreen (`mpv --fs`): size it for that. */
			window->fullscreen = window->fullscreen_applied = true;
			window_apply_fullscreen_scene(window, true);
			wlr_xdg_toplevel_set_fullscreen(toplevel, true);
			window_resize(window, full.width, full.height, true);
		} else {
			/* It will get a column of its own, at the default width. */
			int width = tw_column_width(area.width, gap, column_widths[default_column_width]);
			window_resize(window, width, tw_stack_height(area.height - 2 * gap, gap, 1, 0), true);
		}
		return;
	}
	if (!window->mapped) {
		return;
	}
	if (window->floating) {
		/* Its size is its own, so re-centre when it changes. */
		if (workspace_boxes(window->server, window->workspace, &area, &full)) {
			float_place(window, &area);
		}
	} else {
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
		window_set_fullscreen(window, toplevel->requested.fullscreen);
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

	bool ok = (window->scene_tree = wlr_scene_tree_create(server->window_tree)) != NULL;
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
	struct wlr_box area, full;
	int lx, ly;
	if (window != NULL && window->workspace != NULL &&
			workspace_boxes(popup->server, window->workspace, &area, &full) &&
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

/* The strip's focused window, whether or not a dialog has the keyboard.
 * Moving focus along the strip starts from here and leaves the dialog. */
static struct tw_window *strip_window(struct tw_server *server) {
	return server->workspaces[server->active].tiled;
}

/* The same, but only if it really is the focused window: actions that change
 * a window do nothing while a dialog is in front. */
static struct tw_window *focused_tiled(struct tw_server *server) {
	struct tw_workspace *workspace = &server->workspaces[server->active];
	return workspace->focused == workspace->tiled ? workspace->tiled : NULL;
}

void action_close(struct tw_server *server, const union tw_arg *arg) {
	struct tw_window *window = server->workspaces[server->active].focused;
	if (window != NULL) {
		wlr_xdg_toplevel_send_close(window->xdg_toplevel);
	}
}

void action_focus(struct tw_server *server, const union tw_arg *arg) {
	struct tw_window *window = strip_window(server);
	if (window == NULL) {
		return;
	}
	struct tw_column *column = column_neighbour(window->column, arg->i);
	window_focus(server, column != NULL ? column->active : window);
	strip_arrange(server);
}

void action_focus_edge(struct tw_server *server, const union tw_arg *arg) {
	struct tw_workspace *workspace = &server->workspaces[server->active];
	if (wl_list_empty(&workspace->columns)) {
		return;
	}
	struct wl_list *link = arg->i < 0 ? workspace->columns.next : workspace->columns.prev;
	struct tw_column *column = wl_container_of(link, column, link);
	window_focus(server, column->active);
	strip_arrange(server);
}

void action_focus_window(struct tw_server *server, const union tw_arg *arg) {
	struct tw_window *window = strip_window(server);
	if (window == NULL) {
		return;
	}
	struct tw_window *sibling = window_sibling(window, arg->i);
	window_focus(server, sibling != NULL ? sibling : window);
	strip_arrange(server);
}

void action_move(struct tw_server *server, const union tw_arg *arg) {
	struct tw_window *window = focused_tiled(server);
	struct tw_column *neighbour = window != NULL ?
		column_neighbour(window->column, arg->i) : NULL;
	if (neighbour == NULL) {
		return;
	}
	wl_list_remove(&window->column->link);
	wl_list_insert(arg->i < 0 ? neighbour->link.prev : &neighbour->link, &window->column->link);
	strip_arrange(server);
}

void action_move_edge(struct tw_server *server, const union tw_arg *arg) {
	struct tw_window *window = focused_tiled(server);
	if (window == NULL) {
		return;
	}
	struct wl_list *columns = &window->workspace->columns;
	wl_list_remove(&window->column->link);
	wl_list_insert(arg->i < 0 ? columns : columns->prev, &window->column->link);
	strip_arrange(server);
}

void action_move_window(struct tw_server *server, const union tw_arg *arg) {
	struct tw_window *window = focused_tiled(server);
	struct tw_window *sibling = window != NULL ? window_sibling(window, arg->i) : NULL;
	if (sibling == NULL) {
		return;
	}
	wl_list_remove(&window->link);
	wl_list_insert(arg->i < 0 ? sibling->link.prev : &sibling->link, &window->link);
	strip_arrange(server);
}

void action_consume_or_expel(struct tw_server *server, const union tw_arg *arg) {
	struct tw_window *window = focused_tiled(server);
	if (window == NULL) {
		return;
	}
	if (!column_single(window->column)) {
		if (!window_expel(window, arg->i)) {
			return;
		}
	} else {
		struct tw_column *neighbour = column_neighbour(window->column, arg->i);
		if (neighbour == NULL) {
			return;
		}
		window_join(window, neighbour);
		neighbour->active = window;
	}
	strip_arrange(server);
}

void action_consume(struct tw_server *server, const union tw_arg *arg) {
	struct tw_window *window = focused_tiled(server);
	struct tw_column *right = window != NULL ? column_neighbour(window->column, +1) : NULL;
	if (right == NULL) {
		return;
	}
	window_join(column_first(right), window->column);
	strip_arrange(server);
}

void action_expel(struct tw_server *server, const union tw_arg *arg) {
	struct tw_window *window = focused_tiled(server);
	if (window == NULL || column_single(window->column) || !window_expel(window, +1)) {
		return;
	}
	strip_arrange(server);
}

void action_cycle_width(struct tw_server *server, const union tw_arg *arg) {
	struct tw_window *window = focused_tiled(server);
	if (window == NULL) {
		return;
	}
	struct tw_column *column = window->column;
	/* Leaving the maximized state lands on the first preset. */
	column->width_idx = column->maximized ? 0 : (column->width_idx + 1) % LENGTH(column_widths);
	column->maximized = false;
	strip_arrange(server);
}

void action_maximize(struct tw_server *server, const union tw_arg *arg) {
	struct tw_window *window = focused_tiled(server);
	if (window == NULL) {
		return;
	}
	window->column->maximized = !window->column->maximized;
	strip_arrange(server);
}

void action_fullscreen(struct tw_server *server, const union tw_arg *arg) {
	struct tw_window *window = focused_tiled(server);
	if (window == NULL) {
		return;
	}
	window_set_fullscreen(window, !window->fullscreen);
	strip_arrange(server);
}

/* Goes to a workspace. One that another output shows stays there, and the
 * focus moves to that output. */
static void workspace_show(struct tw_server *server, size_t index) {
	if (index >= TW_WORKSPACES || index == server->active) {
		return;
	}
	workspace_activate(server, index);
	focus_update(server);
	strip_arrange(server);
}

void strip_focus_output(struct tw_server *server, struct tw_output *output) {
	workspace_show(server, output->workspace);
}

/* Sends the focused column (or the focused dialog) to another workspace and
 * follows it there. */
static void workspace_take_column(struct tw_server *server, size_t index) {
	struct tw_workspace *from = &server->workspaces[server->active];
	struct tw_window *window = from->focused;
	if (window == NULL || index >= TW_WORKSPACES || index == server->active) {
		return;
	}
	struct tw_workspace *to = &server->workspaces[index];

	if (window->floating) {
		window_remove(window);
		wl_list_insert(&to->floating, &window->link);
		window->workspace = to;
	} else {
		struct tw_column *column = window->column;
		struct tw_column *other = column_neighbour(column, -1);
		if (other == NULL) {
			other = column_neighbour(column, +1);
		}
		from->tiled = other != NULL ? other->active : NULL;
		from->focused = from->tiled != NULL ? from->tiled : first_floating(from);

		wl_list_remove(&column->link);
		wl_list_insert(to->tiled != NULL ? &to->tiled->column->link : to->columns.prev,
			&column->link);
		column->workspace = to;
		struct tw_window *member;
		wl_list_for_each(member, &column->windows, link) {
			member->workspace = to;
		}
		to->tiled = window;
	}
	to->focused = window;
	/* The column stays the active window, so no focus events are sent. */
	workspace_activate(server, index);
	focus_update(server);
	strip_arrange(server);
}

/* Index of the next workspace in that direction that no other output shows,
 * or TW_WORKSPACES if there is none. */
static size_t workspace_relative(struct tw_server *server, int direction) {
	size_t index = server->active;
	while (direction < 0 ? index > 0 : index + 1 < TW_WORKSPACES) {
		index = direction < 0 ? index - 1 : index + 1;
		if (output_showing(server, index) == NULL) {
			return index;
		}
	}
	return TW_WORKSPACES;
}

/* The workspace in a direction (enum wlr_direction): the one on the monitor
 * on that side if there is such a monitor, otherwise the previous (up, left)
 * or next (down, right) workspace. TW_WORKSPACES if there is none. */
static size_t workspace_toward(struct tw_server *server, int direction) {
	struct tw_output *output = output_adjacent(output_focused(server), direction);
	if (output != NULL) {
		return output->workspace;
	}
	bool back = direction == WLR_DIRECTION_UP || direction == WLR_DIRECTION_LEFT;
	return workspace_relative(server, back ? -1 : +1);
}

void action_workspace_toward(struct tw_server *server, const union tw_arg *arg) {
	workspace_show(server, workspace_toward(server, arg->i));
}

void action_move_to_workspace_toward(struct tw_server *server, const union tw_arg *arg) {
	workspace_take_column(server, workspace_toward(server, arg->i));
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
		wl_list_init(&server->workspaces[i].columns);
		wl_list_init(&server->workspaces[i].floating);
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

	server->activation = wlr_xdg_activation_v1_create(server->display);
	server->request_activate.notify = handle_request_activate;
	wl_signal_add(&server->activation->events.request_activate, &server->request_activate);
}

void window_finish(struct tw_server *server) {
	wl_list_remove(&server->new_toplevel.link);
	wl_list_remove(&server->new_popup.link);
	wl_list_remove(&server->new_decoration.link);
	wl_list_remove(&server->request_activate.link);
}
