/*
 * One window: its client (xdg-shell), its nodes in the scene, the size and
 * state it is told, its menus and its title bar. Where it goes and whether it
 * has the focus is strip.c's business; this file carries that out, sending
 * the client only what has changed.
 */
#include <stdlib.h>
#include <wlr/types/wlr_compositor.h>
#include <wlr/types/wlr_ext_foreign_toplevel_list_v1.h>
#include <wlr/types/wlr_ext_image_capture_source_v1.h>
#include <wlr/types/wlr_ext_image_copy_capture_v1.h>
#include <wlr/types/wlr_layer_shell_v1.h>
#include <wlr/types/wlr_scene.h>
#include <wlr/types/wlr_xdg_activation_v1.h>
#include <wlr/types/wlr_xdg_decoration_v1.h>
#include <wlr/types/wlr_xdg_shell.h>
#include <wlr/util/edges.h>
#include <wlr/util/log.h>

#include "tw.h"

#define ALL_EDGES (WLR_EDGE_TOP | WLR_EDGE_BOTTOM | WLR_EDGE_LEFT | WLR_EDGE_RIGHT)

struct tw_popup {
	struct tw_server *server;
	struct wlr_xdg_popup *xdg_popup;
	/* Its place in the scene, also in xdg_popup->base->data: NULL until it
	 * has one, and again once the tree is gone, whichever goes first, the
	 * popup or what it hangs from. */
	struct wlr_scene_tree *tree;
	struct wl_listener commit;
	struct wl_listener reposition;
	struct wl_listener destroy;
	struct wl_listener tree_destroy; /* linked while tree != NULL */
};

static struct tw_window *window_from_toplevel(struct wlr_xdg_toplevel *toplevel) {
	struct wlr_scene_tree *tree = toplevel->base->data;
	return tree != NULL ? tree->node.data : NULL;
}

bool surface_visible(struct tw_server *server, struct wlr_surface *surface) {
	struct wlr_surface *root = wlr_surface_get_root_surface(surface);
	struct wlr_xdg_toplevel *toplevel = wlr_xdg_toplevel_try_from_wlr_surface(root);
	if (toplevel != NULL) {
		struct tw_window *window = window_from_toplevel(toplevel);
		return window != NULL && window->mapped && window->scene_tree->node.enabled;
	}
	/* Layer surfaces stay in the scene behind the lock curtain and on a
	 * screen that is off; nobody sees them there either. */
	return wlr_layer_surface_v1_try_from_wlr_surface(root) != NULL && root->mapped &&
		screen_on(server);
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

/* --- What the window is told, and how it is drawn --------------------------- */

/* Moves the scene node to the tree for what the window is now (dialog,
 * fullscreen, or tiled). */
static void window_restack(struct tw_window *window) {
	struct tw_server *server = window->server;
	struct wlr_scene_tree *layer = server->window_tree;
	if (window->floating) {
		layer = server->float_tree;
	} else if (window->fullscreen_applied) {
		layer = server->fullscreen_tree;
	}
	wlr_scene_node_reparent(&window->scene_tree->node, layer);
}

/* Moves the window, and its menus with it. */
static void window_move(struct tw_window *window, int x, int y) {
	wlr_scene_node_set_position(&window->scene_tree->node, x, y);
	wlr_scene_node_set_position(&window->popup_tree->node, x, y);
}

/* Draws the window and its menus, or leaves them out of the picture. */
static void window_draw(struct tw_window *window, bool drawn) {
	wlr_scene_node_set_enabled(&window->scene_tree->node, drawn);
	wlr_scene_node_set_enabled(&window->popup_tree->node, drawn);
}

void window_apply_fullscreen(struct tw_window *window) {
	if (window->fullscreen_applied == window->fullscreen) {
		return;
	}
	window->fullscreen_applied = window->fullscreen;
	window_restack(window);
	wlr_xdg_toplevel_set_fullscreen(window->xdg_toplevel, window->fullscreen);
}

static int at_least_zero(int value) {
	return value > 0 ? value : 0;
}

/* A client may ignore the size it was given; never let a tiled window draw
 * outside its box, or outside its output. The clip is in surface
 * coordinates, hence the geometry offset. A dialog is not clipped: it has no
 * neighbours, its size is its own, and the shadow it draws outside its
 * geometry is meant to be seen. window->clip is then the zero box, which is
 * also what an unmapped window starts from with no clip set. */
static void window_update_clip(struct tw_window *window) {
	struct wlr_box clip = {0};
	if (!window->floating) {
		const struct wlr_box *geometry = &window->xdg_toplevel->base->geometry;
		int left = window->cut_left, right = window->cut_right;
		clip = (struct wlr_box){
			.x = geometry->x + left,
			.y = geometry->y,
			.width = at_least_zero(window->width - left - right),
			.height = window->height,
		};
	}
	if (wlr_box_equal(&clip, &window->clip)) {
		return;
	}
	window->clip = clip;
	if (window->floating) {
		wlr_scene_node_set_enabled(&window->surface_tree->node, true);
		wlr_scene_subsurface_tree_set_clip(&window->surface_tree->node, NULL);
		return;
	}
	/* An empty clip would mean "do not clip": a window cut off entirely is
	 * left out of the picture instead. */
	wlr_scene_node_set_enabled(&window->surface_tree->node, clip.width > 0);
	if (clip.width > 0) {
		wlr_scene_subsurface_tree_set_clip(&window->surface_tree->node, &clip);
	}
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

void window_place(struct tw_window *window, const struct wlr_box *box, bool configure,
		int cut_left, int cut_right) {
	window_move(window, box->x, box->y);
	window_resize(window, box->width, box->height, configure);
	window->cut_left = at_least_zero(cut_left);
	window->cut_right = at_least_zero(cut_right);
	window_update_clip(window);
}

void window_centre(struct tw_window *window, const struct wlr_box *area) {
	const struct wlr_box *geometry = &window->xdg_toplevel->base->geometry;
	int width = geometry->width > 0 ? geometry->width : 1;
	int height = geometry->height > 0 ? geometry->height : 1;
	int x = area->x + (area->width - width) / 2;
	int y = area->y + (area->height - height) / 2;
	struct wlr_box box = {x > area->x ? x : area->x, y > area->y ? y : area->y, width, height};
	window_place(window, &box, false, 0, 0);
}

void window_show(struct tw_window *window, bool on_screen, bool wanted) {
	window_draw(window, on_screen);
	/* Being recorded, it is seen even off screen. */
	wanted = wanted || window->captures > 0;
	if (window->suspended == wanted) {
		window->suspended = !wanted;
		wlr_xdg_toplevel_set_suspended(window->xdg_toplevel, !wanted);
	}
}

void window_set_floating(struct tw_window *window, bool floating) {
	window->floating = floating;
	window_restack(window);
	/* A tiled window is told so, and draws no shadow or rounded corners
	 * where it meets its neighbours; a dialog keeps its own. */
	wlr_xdg_toplevel_set_tiled(window->xdg_toplevel, floating ? WLR_EDGE_NONE : ALL_EDGES);
	/* A dialog was last told 0x0, whatever size window_centre() recorded
	 * for it since, so a window going into the strip is sent its size by
	 * the next window_place() even if it happens to match. */
	window->width = window->height = 0;
	if (floating) {
		/* Its size is its own from now on; window_centre() records it. */
		window->cut_left = window->cut_right = 0;
		wlr_xdg_toplevel_set_size(window->xdg_toplevel, 0, 0);
	}
}

/* A dialog: has a parent, or cannot be resized. */
static bool wants_float(struct wlr_xdg_toplevel *toplevel) {
	const struct wlr_xdg_toplevel_state *state = &toplevel->current;
	return toplevel->parent != NULL ||
		(state->min_width > 0 && state->min_height > 0 &&
		state->min_width == state->max_width && state->min_height == state->max_height);
}

/* --- The window list and window capture ------------------------------------- */

/*
 * A screen recorder (OBS through xdg-desktop-portal-wlr) offers the windows
 * of ext-foreign-toplevel-list to choose from, and records the one chosen
 * through a capture source made for it. The source draws a scene of its own
 * that holds this window alone, so that what covers the window, or its being
 * scrolled off screen, does not show in the recording; the client draws for
 * that scene as for an output. Nothing of it exists until a recorder asks.
 */
struct tw_capture {
	struct tw_window *window;
	struct wl_listener destroy;
};

static void window_capture_stop(struct tw_window *window) {
	if (window->capture_scene == NULL) {
		return;
	}
	/* Takes the source along, which ends its recordings: their destroy
	 * handlers below run before this returns. */
	wl_list_remove(&window->capture_link);
	wlr_scene_node_destroy(&window->capture_scene->tree.node);
	window->capture_scene = NULL;
	window->capture_source = NULL;
}

void windows_capture_stop(struct tw_server *server) {
	struct tw_window *window, *tmp;
	wl_list_for_each_safe(window, tmp, &server->captured, capture_link) {
		window_capture_stop(window);
	}
}

static bool window_capture_start(struct tw_window *window) {
	struct tw_server *server = window->server;
	struct wlr_scene *scene = wlr_scene_create();
	if (scene == NULL) {
		return false;
	}
	struct wlr_ext_image_capture_source_v1 *source = NULL;
	if (wlr_scene_xdg_surface_create(&scene->tree, window->xdg_toplevel->base) != NULL) {
		source = wlr_ext_image_capture_source_v1_create_with_scene_node(&scene->tree.node,
			server->loop, server->allocator, server->renderer);
	}
	if (source == NULL) {
		wlr_scene_node_destroy(&scene->tree.node);
		return false;
	}
	window->capture_scene = scene;
	window->capture_source = source;
	wl_list_insert(&server->captured, &window->capture_link);
	return true;
}

static void handle_capture_request(struct wl_listener *listener, void *data) {
	struct tw_server *server = wl_container_of(listener, server, new_capture_request);
	struct wlr_ext_foreign_toplevel_image_capture_source_manager_v1_request *request = data;
	/* A handle exists only while its window is mapped. */
	struct tw_window *window = request->toplevel_handle->data;
	if (window->capture_scene == NULL && !window_capture_start(window)) {
		wlr_log(WLR_ERROR, "out of memory, a window cannot be recorded");
	}
	/* Without a source the recorder gets one that records nothing. */
	wlr_ext_foreign_toplevel_image_capture_source_manager_v1_request_accept(request,
		window->capture_source);
}

static void handle_capture_destroy(struct wl_listener *listener, void *data) {
	struct tw_capture *capture = wl_container_of(listener, capture, destroy);
	struct tw_window *window = capture->window;
	wl_list_remove(&capture->destroy.link);
	free(capture);
	/* Off screen, it may be suspended again; not while it is unmapping. */
	if (--window->captures == 0 && window->mapped) {
		strip_arrange(window->server);
	}
}

/* A recording begins, of an output or of a window. */
static void handle_capture_session(struct wl_listener *listener, void *data) {
	struct tw_server *server = wl_container_of(listener, server, new_capture_session);
	struct wlr_ext_image_copy_capture_session_v1 *session = data;
	struct tw_window *window;
	wl_list_for_each(window, &server->captured, capture_link) {
		if (window->capture_source != session->source) {
			continue;
		}
		struct tw_capture *capture = calloc(1, sizeof(*capture));
		if (capture == NULL) {
			return; /* recorded all the same, only maybe suspended */
		}
		capture->window = window;
		capture->destroy.notify = handle_capture_destroy;
		wl_signal_add(&session->events.destroy, &capture->destroy);
		if (window->captures++ == 0) {
			strip_arrange(server);
		}
		return;
	}
}

static struct wlr_ext_foreign_toplevel_handle_v1_state window_names(struct tw_window *window) {
	return (struct wlr_ext_foreign_toplevel_handle_v1_state){
		.title = window->xdg_toplevel->title,
		.app_id = window->xdg_toplevel->app_id,
	};
}

static void handle_set_title(struct wl_listener *listener, void *data) {
	struct tw_window *window = wl_container_of(listener, window, set_title);
	if (window->handle != NULL) {
		struct wlr_ext_foreign_toplevel_handle_v1_state names = window_names(window);
		wlr_ext_foreign_toplevel_handle_v1_update_state(window->handle, &names);
	}
}

static void handle_set_app_id(struct wl_listener *listener, void *data) {
	struct tw_window *window = wl_container_of(listener, window, set_app_id);
	if (window->handle != NULL) {
		struct wlr_ext_foreign_toplevel_handle_v1_state names = window_names(window);
		wlr_ext_foreign_toplevel_handle_v1_update_state(window->handle, &names);
	}
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

static void window_unmap(struct tw_window *window) {
	struct tw_server *server = window->server;
	window->mapped = false;
	window_draw(window, false);
	strip_remove(window);
	x11_window_closed(server);
	window_capture_stop(window);
	if (window->handle != NULL) {
		wlr_ext_foreign_toplevel_handle_v1_destroy(window->handle);
		window->handle = NULL;
	}

	/* Nothing may be sent to the client until it commits again, and
	 * initial_commit() then tells it its whole state afresh. What the scene
	 * shows of it goes back to how a new window starts. */
	window->clip = (struct wlr_box){0};
	wlr_scene_node_set_enabled(&window->surface_tree->node, true);
	wlr_scene_subsurface_tree_set_clip(&window->surface_tree->node, NULL);
	window->cut_left = window->cut_right = 0;
}

static void handle_map(struct wl_listener *listener, void *data) {
	struct tw_window *window = wl_container_of(listener, window, map);
	struct tw_server *server = window->server;
	/* Mapped before it is arranged: an idle inhibitor it holds counts. */
	window->mapped = true;
	if (!strip_add(window)) {
		window->mapped = false;
		wlr_log(WLR_ERROR, "out of memory, closing a new window");
		wlr_xdg_toplevel_send_close(window->xdg_toplevel);
		return;
	}

	/* Out of memory, it is missing from the list. */
	struct wlr_ext_foreign_toplevel_handle_v1_state names = window_names(window);
	window->handle = wlr_ext_foreign_toplevel_handle_v1_create(server->toplevel_list, &names);
	if (window->handle != NULL) {
		window->handle->data = window;
	}

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

/*
 * The first commit, before the window has drawn anything: what it is decides
 * how it is configured. A tiled window is told the size of the column it will
 * get, so that its first frame already fits and nothing visibly resizes as it
 * maps. A dialog picks its own size. A window that asks to start fullscreen
 * (`mpv --fs`) is sized for that, even a game of a fixed size, which would
 * otherwise float: it is in the strip for as long as it stays fullscreen.
 * strip_add() settles where it goes, and whether it is fullscreen, as it maps.
 *
 * The whole state goes in one configure. A window that was mapped before
 * would otherwise be sent what it was last told then, which wlroots keeps:
 * suspended (it was hidden), a client that draws nothing while suspended
 * would never show up again; tiled, a dialog would draw no shadow.
 */
static void initial_commit(struct tw_window *window) {
	struct wlr_xdg_toplevel *toplevel = window->xdg_toplevel;
	decoration_apply(window);

	window->floating = wants_float(toplevel);
	bool fullscreen = toplevel->requested.fullscreen &&
		(!window->floating || toplevel->parent == NULL);
	bool tiled = !window->floating || fullscreen;
	/* With no output showing the workspace, it picks its own size for now. */
	int width = 0, height = 0;
	if (tiled) {
		strip_initial_size(window->server, fullscreen, &width, &height);
	}
	struct wlr_xdg_toplevel_configure configure = {
		/* tw has no maximized or minimized state and no window menu: told
		 * otherwise, a program that draws its own title bar shows buttons
		 * for them that do nothing. */
		.fields = WLR_XDG_TOPLEVEL_CONFIGURE_WM_CAPABILITIES,
		.wm_capabilities = WLR_XDG_TOPLEVEL_WM_CAPABILITIES_FULLSCREEN,
		.fullscreen = fullscreen,
		.tiled = tiled ? ALL_EDGES : WLR_EDGE_NONE,
		.width = width,
		.height = height,
	};
	wlr_xdg_toplevel_configure(toplevel, &configure);

	window->width = width;
	window->height = height;
	window->suspended = false;
	window->fullscreen_applied = fullscreen;
	window_restack(window);
}

static void handle_commit(struct wl_listener *listener, void *data) {
	struct tw_window *window = wl_container_of(listener, window, commit);
	if (window->xdg_toplevel->base->initial_commit) {
		initial_commit(window);
		return;
	}
	if (!window->mapped) {
		return;
	}
	struct wlr_box area, full;
	if (!window->floating) {
		/* Its geometry may have moved within its surface. */
		window_update_clip(window);
	} else if (workspace_boxes(window->server, window->workspace, &area, &full)) {
		/* Its size is its own, so re-centre when it changes. */
		window_centre(window, &area);
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
	wl_list_remove(&window->set_title.link);
	wl_list_remove(&window->set_app_id.link);

	window->xdg_toplevel->base->data = NULL;
	wlr_scene_node_destroy(&window->popup_tree->node);
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
	ok = ok && (window->popup_tree = wlr_scene_tree_create(server->popup_tree)) != NULL;
	if (!ok) {
		if (window->scene_tree != NULL) {
			wlr_scene_node_destroy(&window->scene_tree->node);
		}
		free(window);
		wl_resource_post_no_memory(toplevel->resource);
		return;
	}

	/* Hidden until mapped and arranged. */
	window_draw(window, false);
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
	window->set_title.notify = handle_set_title;
	wl_signal_add(&toplevel->events.set_title, &window->set_title);
	window->set_app_id.notify = handle_set_app_id;
	wl_signal_add(&toplevel->events.set_app_id, &window->set_app_id);
}

/*
 * A window asks for the focus (xdg-activation): a link was clicked and the
 * browser wants to come forward, or a notification was.
 */
static void handle_request_activate(struct wl_listener *listener, void *data) {
	struct tw_server *server = wl_container_of(listener, server, request_activate);
	struct wlr_xdg_activation_v1_request_activate_event *event = data;

	/* It gets the focus only with a token made from an input event in a
	 * surface that had the keyboard or the pointer: proof that the user did
	 * something in the program that issued it. wlroots has checked both,
	 * but the surface only if the token names one. Without a surface, any
	 * serial the client was ever sent would do, the pointer merely passing
	 * over a window in the background; without an event, any client could
	 * make one at any time. Either would let a window steal the focus. */
	if (event->token->seat == NULL || event->token->surface == NULL || server->locked) {
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

/*
 * Keeps the popup on its output: hands wlroots the output's box, in the
 * coordinates of the surface the popup chain hangs from. For a window,
 * popup_tree is at the window's geometry, which a client with a shadow drawn
 * around its window puts away from the surface's corner.
 */
static void popup_unconstrain(struct tw_popup *popup) {
	struct wlr_xdg_popup *xdg_popup = popup->xdg_popup;
	struct tw_window *window = popup_window(xdg_popup);
	struct wlr_box area, full;
	if (window != NULL) {
		/* Every window of a shown workspace is placed, drawn or not: a menu
		 * opened while the window is off screen, or the screen is off, is
		 * kept on the output all the same. */
		if (window->workspace != NULL &&
				workspace_boxes(popup->server, window->workspace, &area, &full)) {
			int lx, ly;
			wlr_scene_node_coords(&window->popup_tree->node, &lx, &ly);
			area.x -= lx - window->xdg_toplevel->base->geometry.x;
			area.y -= ly - window->xdg_toplevel->base->geometry.y;
			wlr_xdg_popup_unconstrain_from_box(xdg_popup, &area);
		}
	} else if (layer_popup_box(popup->server, xdg_popup, &area)) {
		wlr_xdg_popup_unconstrain_from_box(xdg_popup, &area);
	}
}

static void handle_popup_commit(struct wl_listener *listener, void *data) {
	struct tw_popup *popup = wl_container_of(listener, popup, commit);
	if (popup->xdg_popup->base->initial_commit) {
		popup_unconstrain(popup);
		wlr_xdg_surface_schedule_configure(popup->xdg_popup->base);
	}
}

static void handle_popup_reposition(struct wl_listener *listener, void *data) {
	/* The client moved it (a menu that follows what it belongs to): wlroots
	 * has taken the new place as asked and is about to send it, so the
	 * place is checked against the output again first. */
	struct tw_popup *popup = wl_container_of(listener, popup, reposition);
	popup_unconstrain(popup);
}

/* What the popup hangs from has gone, and its tree with it. */
static void handle_popup_tree_destroy(struct wl_listener *listener, void *data) {
	struct tw_popup *popup = wl_container_of(listener, popup, tree_destroy);
	wl_list_remove(&popup->tree_destroy.link);
	popup->tree = NULL;
	popup->xdg_popup->base->data = NULL;
}

static void handle_popup_destroy(struct wl_listener *listener, void *data) {
	struct tw_popup *popup = wl_container_of(listener, popup, destroy);
	/* Its surface may live on and become another popup, which gets a tree
	 * of its own. */
	if (popup->tree != NULL) {
		wl_list_remove(&popup->tree_destroy.link);
		popup->xdg_popup->base->data = NULL;
		wlr_scene_node_destroy(&popup->tree->node);
	}
	wl_list_remove(&popup->commit.link);
	wl_list_remove(&popup->reposition.link);
	wl_list_remove(&popup->destroy.link);
	free(popup);
}

void popup_attach(struct wlr_xdg_popup *xdg_popup, struct wlr_scene_tree *parent) {
	struct wl_listener *listener = wl_signal_get(&xdg_popup->events.destroy, handle_popup_destroy);
	if (listener == NULL) {
		return;
	}
	struct tw_popup *popup = wl_container_of(listener, popup, destroy);
	if (popup->tree != NULL) {
		return;
	}
	/* Out of memory, the menu stays invisible. */
	popup->tree = wlr_scene_xdg_surface_create(parent, xdg_popup->base);
	if (popup->tree == NULL) {
		return;
	}
	xdg_popup->base->data = popup->tree;
	popup->tree_destroy.notify = handle_popup_tree_destroy;
	wl_signal_add(&popup->tree->node.events.destroy, &popup->tree_destroy);
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
	popup->commit.notify = handle_popup_commit;
	wl_signal_add(&xdg_popup->base->surface->events.commit, &popup->commit);
	popup->reposition.notify = handle_popup_reposition;
	wl_signal_add(&xdg_popup->events.reposition, &popup->reposition);
	popup->destroy.notify = handle_popup_destroy;
	wl_signal_add(&xdg_popup->events.destroy, &popup->destroy);

	/* A layer surface's popup has no parent yet; layer.c attaches it when
	 * the client does. Anything else stays invisible. */
	struct wlr_xdg_surface *parent = xdg_popup->parent != NULL ?
		wlr_xdg_surface_try_from_wlr_surface(xdg_popup->parent) : NULL;
	if (parent != NULL && parent->data != NULL) {
		popup_attach(xdg_popup, parent->data);
	}
}

void window_init(struct tw_server *server) {
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

	wl_list_init(&server->captured);
	server->toplevel_list = wlr_ext_foreign_toplevel_list_v1_create(server->display, 1);
	server->toplevel_capture =
		wlr_ext_foreign_toplevel_image_capture_source_manager_v1_create(server->display, 1);
	server->new_capture_request.notify = handle_capture_request;
	wl_signal_add(&server->toplevel_capture->events.new_request, &server->new_capture_request);
	server->new_capture_session.notify = handle_capture_session;
	wl_signal_add(&server->image_copy_capture->events.new_session, &server->new_capture_session);
}

void window_finish(struct tw_server *server) {
	wl_list_remove(&server->new_toplevel.link);
	wl_list_remove(&server->new_popup.link);
	wl_list_remove(&server->new_decoration.link);
	wl_list_remove(&server->request_activate.link);
	wl_list_remove(&server->new_capture_request.link);
	wl_list_remove(&server->new_capture_session.link);
}
