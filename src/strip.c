/*
 * Where windows go: workspaces, the strip of columns each one is, the focus,
 * the view that scrolls along the strip, and the actions bound to keys.
 *
 * A workspace is a strip of columns, left to right. A column is a stack of
 * windows, top to bottom, that share the column's width and split its height.
 * A column is never empty: removing its last window frees it. Dialogs float
 * in front of the strip, outside any column.
 *
 * strip_arrange() is the only thing that positions windows, and
 * focus_update() the only thing that sends focus events. What a window is to
 * its client (its size, its scene nodes, what it is told) is window.c's.
 */
#include <math.h>
#include <stdlib.h>
#include <wlr/types/wlr_keyboard.h>
#include <wlr/types/wlr_output.h>
#include <wlr/types/wlr_output_layout.h>
#include <wlr/types/wlr_scene.h>
#include <wlr/types/wlr_seat.h>
#include <wlr/types/wlr_xdg_shell.h>

#include "tw.h"
#include "config.h"

struct tw_column {
	struct wl_list link; /* tw_workspace.columns */
	struct tw_workspace *workspace;
	struct wl_list windows; /* tw_window.link */
	struct tw_window *active; /* the window focused when the column is */
	size_t width_idx; /* index into column_widths */
	bool maximized;   /* full width, overriding width_idx */
};

static struct tw_workspace *active_workspace(struct tw_server *server) {
	return &server->workspaces[server->active];
}

static size_t workspace_index(struct tw_server *server, const struct tw_workspace *workspace) {
	return (size_t)(workspace - server->workspaces);
}

bool workspace_boxes(struct tw_server *server, const struct tw_workspace *workspace,
		struct wlr_box *area, struct wlr_box *full) {
	return output_boxes(output_showing(server, workspace_index(server, workspace)), area, full);
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

static struct tw_window *column_first(const struct tw_column *column) {
	struct tw_window *window = wl_container_of(column->windows.next, window, link);
	return window;
}

static bool column_single(const struct tw_column *column) {
	return column->windows.next == column->windows.prev;
}

/* A fullscreen window is always alone in its column. */
static bool column_fullscreen(const struct tw_column *column) {
	return column_first(column)->fullscreen;
}

/* The column whose window gets the focus when this one leaves the strip:
 * the one on its left, else the one on its right, else NULL. */
static struct tw_column *column_heir(struct tw_column *column) {
	struct tw_column *heir = column_neighbour(column, -1);
	return heir != NULL ? heir : column_neighbour(column, +1);
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

/* Puts the window into the strip, in a column of its own to the right of the
 * focused one: a new column never resizes the others. */
static bool strip_insert(struct tw_workspace *workspace, struct tw_window *window) {
	struct tw_column *column = column_new(workspace, workspace->tiled != NULL ?
		&workspace->tiled->column->link : workspace->columns.prev);
	if (column == NULL) {
		return false;
	}
	column_insert(column, window, &column->windows);
	column->active = window;
	return true;
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
	struct tw_column *heir = column_heir(column);
	wl_list_remove(&column->link);
	free(column);
	return heir != NULL ? heir->active : NULL;
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

/* Moves a window to the bottom of another column. A column of several is
 * never fullscreen, and what the user put there stays there. */
static void window_join(struct tw_window *window, struct tw_column *column) {
	struct tw_window *first = column_first(column);
	first->fullscreen = window->fullscreen = false;
	first->float_after_fullscreen = window->float_after_fullscreen = false;
	column_take(window);
	column_insert(column, window, column->windows.prev);
}

static struct tw_window *first_floating(struct tw_workspace *workspace) {
	if (wl_list_empty(&workspace->floating)) {
		return NULL;
	}
	struct tw_window *window = wl_container_of(workspace->floating.next, window, link);
	return window;
}

/* Puts a dialog that is in no list in front of the workspace's others: first
 * in the list and on top in the scene, as the focused one is. A dialog that
 * maps again is still wherever it was in the scene. */
static void floating_push(struct tw_workspace *workspace, struct tw_window *window) {
	wl_list_insert(&workspace->floating, &window->link);
	wlr_scene_node_raise_to_top(&window->scene_tree->node);
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

/* --- Geometry --------------------------------------------------------------- */

/* How much of the strip a column takes on an output whose usable area is
 * `area`. A fullscreen column takes one screenful. */
static int column_width(const struct tw_column *column, const struct wlr_box *area) {
	static const struct tw_fraction full = {1, 1};
	if (column_fullscreen(column)) {
		return area->width;
	}
	return tw_column_width(area->width, gap,
		column->maximized ? full : column_widths[column->width_idx]);
}

/* The space kept free on either side of a column that is scrolled into view.
 * A fullscreen column has none: it must cover the output exactly. */
static int column_margin(const struct tw_column *column) {
	return column_fullscreen(column) ? 0 : gap;
}

bool strip_initial_size(struct tw_server *server, bool fullscreen, int *width, int *height) {
	struct wlr_box area, full;
	if (!workspace_boxes(server, active_workspace(server), &area, &full)) {
		return false;
	}
	if (fullscreen) {
		*width = full.width;
		*height = full.height;
	} else {
		*width = tw_column_width(area.width, gap, column_widths[default_column_width]);
		*height = tw_stack_height(area.height - 2 * gap, gap, 1, 0);
	}
	return true;
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
	 * the one next to it. With a single output in the layout (the laptop's
	 * own one may be out of it, behind the lid) there is nothing to trim. */
	bool trim = wl_list_length(&server->output_layout->outputs) > 1;
	int edge_left = full->x, edge_right = full->x + full->width;

	wl_list_for_each(column, &workspace->columns, link) {
		int width = column_width(column, area);
		bool on_screen = shown && tw_column_visible(view, area->width, x, width);
		/* Also wake the windows the view is heading for. */
		bool wanted = on_screen ||
			(shown && tw_column_visible(workspace->view_x, area->width, x, width));

		int count = wl_list_length(&column->windows), index = 0;
		int avail = area->height - 2 * gap;
		int y = area->y + gap;
		wl_list_for_each(window, &column->windows, link) {
			window_apply_fullscreen(window);
			struct wlr_box box;
			if (window->fullscreen) {
				box = (struct wlr_box){full->x + x - view, full->y, full->width, full->height};
			} else {
				int height = tw_stack_height(avail, gap, count, index);
				box = (struct wlr_box){area->x + x - view, y, width, height};
				y += height + gap;
			}
			window_place(window, &box, true, trim ? edge_left - box.x : 0,
				trim ? box.x + box.width - edge_right : 0);
			window_show(window, on_screen, wanted);
			index++;
		}
		x += width + gap;
	}

	wl_list_for_each(window, &workspace->floating, link) {
		window_apply_fullscreen(window);
		window_centre(window, area);
		window_show(window, shown, shown);
	}
}

/* Takes every window of a workspace that no output shows off screen. Its
 * layout is left alone until it is shown again: sizes depend on the output. */
static void workspace_suspend(struct tw_workspace *workspace) {
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
	/* Scroll just far enough to show the focused column completely. During
	 * a swipe the fingers decide instead. */
	if (!workspace->swiping) {
		int x = gap;
		struct tw_column *column;
		wl_list_for_each(column, &workspace->columns, link) {
			int width = column_width(column, area);
			if (workspace->tiled != NULL && workspace->tiled->column == column) {
				workspace->view_x = tw_view_fit(workspace->view_x, area->width, x, width,
					column_margin(column));
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

/*
 * Windows were placed anew. What is on screen decides whether a video player
 * may keep the screen on, and what is under the pointer may have changed
 * without the pointer moving: a lock, another workspace, a window closing.
 */
static void windows_moved(struct tw_server *server) {
	idle_update_inhibited(server);
	pointer_refocus(server);
}

void strip_arrange(struct tw_server *server) {
	/* With the screen off or locked every window is suspended. */
	bool shown = screen_on(server);
	for (size_t i = 0; i < TW_WORKSPACES; i++) {
		struct tw_workspace *workspace = &server->workspaces[i];
		struct tw_output *output = output_showing(server, i);
		struct wlr_box area, full;
		if (!output_boxes(output, &area, &full)) {
			workspace_suspend(workspace);
			continue;
		}
		workspace_arrange(server, workspace, &area, &full, shown);

		/* If the view is not where it should be, glide there. The animation
		 * is stepped from the output's frame handler, so it costs nothing
		 * once it has arrived. */
		if (shown && !workspace->swiping && workspace->view_pos != workspace->view_x) {
			if (!output->animating) {
				output->animating = true;
				output->animation_last_ms = now_ms();
			}
			wlr_output_schedule_frame(output->wlr_output);
		}
	}
	windows_moved(server);
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

	struct tw_workspace *workspace = output->workspace < TW_WORKSPACES ?
		&server->workspaces[output->workspace] : NULL;
	if (workspace == NULL || workspace->swiping || !screen_on(server) ||
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
	windows_moved(server);
	return output->animating;
}

/* --- Touchpad swipe --------------------------------------------------------- */

/* The workspace a swipe began on, which may no longer be the active one. */
static struct tw_workspace *swiping_workspace(struct tw_server *server) {
	for (size_t i = 0; i < TW_WORKSPACES; i++) {
		if (server->workspaces[i].swiping) {
			return &server->workspaces[i];
		}
	}
	return NULL;
}

/* Time between two swipe events, in seconds. */
static double swipe_seconds(const struct tw_workspace *workspace, uint32_t time_msec) {
	return (double)(time_msec - workspace->swipe_time_msec) / 1000.0;
}

void strip_swipe_begin(struct tw_server *server, uint32_t time_msec) {
	struct tw_workspace *workspace = active_workspace(server);
	struct tw_output *output = output_focused(server);
	struct tw_workspace *earlier = swiping_workspace(server);
	if (earlier != NULL) {
		earlier->swiping = false; /* its end never came */
	}
	workspace->swiping = true;
	workspace->view_velocity = 0;
	workspace->swipe_time_msec = time_msec;
	if (output != NULL) {
		output->animating = false;
	}
}

void strip_swipe_update(struct tw_server *server, double dx, uint32_t time_msec) {
	struct tw_workspace *workspace = swiping_workspace(server);
	struct wlr_box area, full;
	if (workspace == NULL || !workspace_boxes(server, workspace, &area, &full)) {
		return;
	}
	/* The strip follows the fingers, so the view moves the other way. Keep a
	 * smoothed speed for the glide after they lift. */
	workspace->view_velocity = tw_swipe_speed(workspace->view_velocity, -dx,
		swipe_seconds(workspace, time_msec));
	workspace->swipe_time_msec = time_msec;
	workspace->view_pos -= dx;

	workspace_place(server, workspace, &area, &full, screen_on(server));
	windows_moved(server);
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
		total += column_width(column, area) + gap;
	}

	/* Candidates: each column flush left, and the far end of the strip. */
	int best = tw_view_clamp(total, area->width, total);
	int x = gap;
	wl_list_for_each(column, &workspace->columns, link) {
		int candidate = tw_view_clamp(x - column_margin(column), area->width, total);
		if (fabs(candidate - projected) < fabs(best - projected)) {
			best = candidate;
		}
		x += column_width(column, area) + gap;
	}
	workspace->view_x = best;

	struct tw_column *focused = workspace->tiled != NULL ? workspace->tiled->column : NULL;
	struct tw_column *first_whole = NULL, *first_partial = NULL;
	x = gap;
	wl_list_for_each(column, &workspace->columns, link) {
		int width = column_width(column, area);
		int margin = column_margin(column);
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
		/* A dialog in front keeps the keyboard; the strip's focus moves
		 * behind it. */
		if (workspace->focused == workspace->tiled) {
			workspace->focused = target->active;
		}
		workspace->tiled = target->active;
	}
}

void strip_swipe_end(struct tw_server *server, bool cancelled, uint32_t time_msec) {
	struct tw_workspace *workspace = swiping_workspace(server);
	struct wlr_box area, full;
	if (workspace == NULL) {
		return;
	}
	workspace->swiping = false;
	/* Fingers that came to rest before lifting set the strip down there,
	 * whatever speed they had before. */
	workspace->view_velocity = tw_swipe_release(workspace->view_velocity,
		swipe_seconds(workspace, time_msec));
	/* A cancelled swipe glides back to where it started. */
	if (!cancelled && workspace_boxes(server, workspace, &area, &full)) {
		workspace_snap(workspace, &area);
	}
	focus_update(server);
	strip_arrange(server);
}

/* --- Focus ------------------------------------------------------------------ */

void focus_update(struct tw_server *server) {
	/* The active window is the focused one of the active workspace. tw
	 * draws nothing to mark it: its client is told, and shows it in its own
	 * way (a text cursor, a brighter title). It stays the active window
	 * while a layer surface borrows the keyboard. */
	struct tw_window *window = active_workspace(server)->focused;
	if (server->focused != window) {
		if (server->focused != NULL) {
			wlr_xdg_toplevel_set_activated(server->focused->xdg_toplevel, false);
		}
		server->focused = window;
		if (window != NULL) {
			wlr_xdg_toplevel_set_activated(window->xdg_toplevel, true);
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
	/* An open menu holds the keyboard and the pointer (a grab), and would
	 * keep them through the change, through a lock too. It closes. So does
	 * a drag, but only for a lock: a window opening during one (a download
	 * finishing) should not make the user drop what they carry. */
	if (server->seat->drag == NULL || server->locked) {
		wlr_seat_keyboard_end_grab(server->seat);
		wlr_seat_pointer_end_grab(server->seat);
	}
	if (surface == NULL) {
		wlr_seat_keyboard_notify_clear_focus(server->seat);
	} else {
		/* No keys are said to be held. The one down is most often the
		 * binding's that moved the focus, whose release goes to nobody, and
		 * a client told of it would take it as stuck. */
		struct wlr_keyboard *keyboard = wlr_seat_get_keyboard(server->seat);
		wlr_seat_keyboard_notify_enter(server->seat, surface, NULL, 0,
			keyboard != NULL ? &keyboard->modifiers : NULL);
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
	workspace_activate(server, workspace_index(server, workspace));
	workspace->focused = window;
	if (window->floating) {
		wl_list_remove(&window->link);
		floating_push(workspace, window);
	} else {
		workspace->tiled = window;
		window->column->active = window;
	}
	focus_update(server);
}

/* --- Windows coming and going ----------------------------------------------- */

/* Takes a window out of the strip to float in front of it, at a size of its
 * own choosing. */
static void window_float(struct tw_window *window) {
	struct tw_workspace *workspace = window->workspace;
	struct tw_window *next = column_take(window);
	if (workspace->tiled == window) {
		workspace->tiled = next;
	}
	window_set_floating(window, true);
	floating_push(workspace, window);
}

/* Puts a floating window into the strip. The strip's focus moves to it if
 * it had the focus, and is never left NULL while the strip has a column. */
static bool window_tile(struct tw_window *window) {
	struct tw_workspace *workspace = window->workspace;
	struct wl_list *prev = window->link.prev;
	wl_list_remove(&window->link);
	if (!strip_insert(workspace, window)) {
		wl_list_insert(prev, &window->link);
		return false;
	}
	window_set_floating(window, false);
	if (workspace->focused == window || workspace->tiled == NULL) {
		workspace->tiled = window;
	}
	return true;
}

void window_set_fullscreen(struct tw_window *window, bool fullscreen) {
	if (window->fullscreen == fullscreen) {
		return;
	}
	if (fullscreen && window->floating) {
		if (window->xdg_toplevel->parent != NULL || !window_tile(window)) {
			return;
		}
		window->float_after_fullscreen = true;
	} else if (fullscreen && !column_single(window->column) && !window_expel(window, +1)) {
		return;
	}
	window->fullscreen = fullscreen;
	if (!fullscreen && window->float_after_fullscreen) {
		window->float_after_fullscreen = false;
		window_float(window);
	}
}

bool strip_add(struct tw_window *window) {
	struct tw_server *server = window->server;
	struct tw_workspace *workspace = active_workspace(server);
	/* Its state is settled here as if it had been asked for now: the
	 * initial commit only told the client what to expect. A client may have
	 * changed its mind about fullscreen since. */
	bool fullscreen = window->xdg_toplevel->requested.fullscreen;
	window->fullscreen = window->float_after_fullscreen = false;
	if (window->floating) {
		window->workspace = workspace;
		floating_push(workspace, window);
	} else if (strip_insert(workspace, window)) {
		workspace->tiled = window;
	} else {
		return false;
	}
	workspace->focused = window;
	window_set_fullscreen(window, fullscreen);
	if (window->floating && window->fullscreen_applied) {
		/* Told it was fullscreen at its first commit, which it no longer
		 * wants or, having a parent now, may not have: it is a dialog
		 * after all, sized by itself. */
		window_set_floating(window, true);
	}
	focus_update(server);
	strip_arrange(server);
	return true;
}

void strip_remove(struct tw_window *window) {
	struct tw_server *server = window->server;
	window_remove(window);
	if (server->moved == window) {
		server->moved = NULL; /* the mouse lets go of nothing */
	}
	if (server->focused == window) {
		/* Cleared without telling the client: unmapped, it is sent nothing
		 * (wlroots would drop it), and initial_commit() tells it it is not
		 * activated if it maps again. */
		server->focused = NULL;
	}
	focus_update(server);
	strip_arrange(server);
}

/*
 * Takes the window's column, or the dialog itself, to workspace `to` and
 * gives it the focus there. The column goes next to `beside` (before or after
 * it), or with `beside` NULL after the column focused there. What it leaves
 * behind keeps its own focus unless the focus went with it. Whether or not
 * the window had the focus, its client is told only if that changes.
 */
static void window_carry(struct tw_server *server, struct tw_window *window,
		struct tw_workspace *to, struct tw_column *beside, bool before) {
	struct tw_workspace *from = window->workspace;
	if (window->floating) {
		window_remove(window);
		window->workspace = to;
		floating_push(to, window);
	} else {
		struct tw_column *column = window->column;
		if (from != to) {
			struct tw_column *heir = column_heir(column);
			if (from->tiled != NULL && from->tiled->column == column) {
				from->tiled = heir != NULL ? heir->active : NULL;
			}
			if (from->focused != NULL && from->focused->column == column) {
				from->focused = from->tiled != NULL ? from->tiled : first_floating(from);
			}
		}
		wl_list_remove(&column->link);
		struct wl_list *after;
		if (beside != NULL) {
			after = before ? beside->link.prev : &beside->link;
		} else {
			after = to->tiled != NULL ? &to->tiled->column->link : to->columns.prev;
		}
		wl_list_insert(after, &column->link);
		column->workspace = to;
		struct tw_window *member;
		wl_list_for_each(member, &column->windows, link) {
			member->workspace = to;
		}
		column->active = window;
		to->tiled = window;
	}
	to->focused = window;
	workspace_activate(server, workspace_index(server, to));
	focus_update(server);
	strip_arrange(server);
}

/*
 * A window dropped with the mouse at lx,ly (input.c). Over a column, its own
 * column goes beside that one, on the side of its middle the pointer is on,
 * and onto that column's workspace. Anywhere else on a monitor (an empty
 * part, a dialog), it goes to the workspace that monitor shows, after the
 * column focused there; a dialog always goes that way. Dropped on itself or
 * on its own workspace's empty part, nothing moves.
 */
void strip_drop(struct tw_server *server, struct tw_window *window, double lx, double ly) {
	struct tw_output *output = output_at(server, lx, ly);
	if (output == NULL || output->workspace >= TW_WORKSPACES) {
		return;
	}
	struct tw_workspace *to = &server->workspaces[output->workspace];
	double sx, sy;
	struct wlr_surface *surface = NULL;
	struct tw_window *under = window_at(server, lx, ly, &surface, &sx, &sy);
	struct tw_column *beside = NULL;
	bool before = false;
	if (!window->floating && under != NULL && !under->floating && under->workspace == to) {
		if (under->column == window->column) {
			return;
		}
		int x, y;
		wlr_scene_node_coords(&under->scene_tree->node, &x, &y);
		beside = under->column;
		before = lx < x + under->width / 2.0;
	} else if (to == window->workspace) {
		return;
	}
	window_carry(server, window, to, beside, before);
}

/* --- Actions ---------------------------------------------------------------- */

static void workspace_show(struct tw_server *server, size_t index);

/* The output on the focused one's left (direction < 0) or right. */
static struct tw_output *output_beside(struct tw_server *server, int direction) {
	return output_adjacent(output_focused(server),
		direction < 0 ? WLR_DIRECTION_LEFT : WLR_DIRECTION_RIGHT);
}

/* The strip's focused window, whether or not a dialog has the keyboard.
 * Moving focus along the strip starts from here and leaves the dialog. */
static struct tw_window *strip_window(struct tw_server *server) {
	return active_workspace(server)->tiled;
}

/* The same, but only if it really is the focused window: actions that change
 * a window do nothing while a dialog is in front. */
static struct tw_window *focused_tiled(struct tw_server *server) {
	struct tw_workspace *workspace = active_workspace(server);
	return workspace->focused == workspace->tiled ? workspace->tiled : NULL;
}

void action_close(struct tw_server *server, const union tw_arg *arg) {
	struct tw_window *window = active_workspace(server)->focused;
	if (window != NULL) {
		wlr_xdg_toplevel_send_close(window->xdg_toplevel);
	}
}

/* Past either end of the strip, or on an empty one, the focus goes on to the
 * monitor on that side, if there is one. */
void action_focus(struct tw_server *server, const union tw_arg *arg) {
	struct tw_window *window = strip_window(server);
	struct tw_column *column = window != NULL ? column_neighbour(window->column, arg->i) : NULL;
	if (column == NULL) {
		struct tw_output *output = output_beside(server, arg->i);
		if (output != NULL) {
			workspace_show(server, output->workspace);
			return;
		}
		if (window == NULL) {
			return;
		}
	}
	window_focus(server, column != NULL ? column->active : window);
	strip_arrange(server);
}

void action_focus_edge(struct tw_server *server, const union tw_arg *arg) {
	struct tw_workspace *workspace = active_workspace(server);
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

/* Past either end of the strip the column goes on to the monitor on that
 * side, if there is one, and comes in at the near end of the strip there:
 * moving it back the other way returns it. */
void action_move(struct tw_server *server, const union tw_arg *arg) {
	struct tw_window *window = focused_tiled(server);
	if (window == NULL) {
		return;
	}
	struct tw_column *neighbour = column_neighbour(window->column, arg->i);
	if (neighbour == NULL) {
		struct tw_output *output = output_beside(server, arg->i);
		if (output == NULL || output->workspace >= TW_WORKSPACES) {
			return;
		}
		struct tw_workspace *to = &server->workspaces[output->workspace];
		struct tw_column *end = NULL;
		if (!wl_list_empty(&to->columns)) {
			end = wl_container_of(arg->i > 0 ? to->columns.next : to->columns.prev, end, link);
		}
		window_carry(server, window, to, end, arg->i > 0);
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
	struct tw_window *window = active_workspace(server)->focused;
	if (window == NULL || index >= TW_WORKSPACES || index == server->active) {
		return;
	}
	window_carry(server, window, &server->workspaces[index], NULL, false);
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

void strip_init(struct tw_server *server) {
	for (size_t i = 0; i < TW_WORKSPACES; i++) {
		wl_list_init(&server->workspaces[i].columns);
		wl_list_init(&server->workspaces[i].floating);
	}
}
