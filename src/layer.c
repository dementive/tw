/*
 * Layer shell: surfaces that are not windows, such as launchers, notification
 * popups, wallpapers and bars. tw draws none of these itself; this is what
 * lets ordinary programs provide them.
 */
#include <math.h>
#include <stdlib.h>
#include <wlr/types/wlr_compositor.h>
#include <wlr/types/wlr_fractional_scale_v1.h>
#include <wlr/types/wlr_layer_shell_v1.h>
#include <wlr/types/wlr_output_layout.h>
#include <wlr/types/wlr_scene.h>
#include <wlr/types/wlr_xdg_shell.h>
#include <wlr/util/log.h>

#include "tw.h"

struct tw_layer_surface {
	struct wl_list link; /* tw_server.layer_surfaces */
	struct tw_server *server;
	struct wlr_layer_surface_v1 *layer_surface;
	struct wlr_scene_layer_surface_v1 *scene;
	/* Its menus: above every window, as a window's are, unless it is on the
	 * overlay layer, where they go just above it. Kept where `scene` is. */
	struct wlr_scene_tree *popup_tree;
	/* Set from unmap, or as its output goes, until the next initial commit.
	 * A configure sent in that window would carry a serial wlroots is about
	 * to forget, and a client that acked it would be killed with a protocol
	 * error. */
	bool awaiting_initial_commit;
	/* What it was last configured for: the output's box, the room left by
	 * the surfaces before it, and the room it left. While neither those
	 * nor the surface's own state change, a configure would say nothing
	 * new, and wlroots sends one whenever asked. */
	bool configured;
	struct wlr_box configured_full, configured_room, configured_left;
	struct wl_listener map;
	struct wl_listener unmap;
	struct wl_listener commit;
	struct wl_listener destroy;
	struct wl_listener new_popup;
};

struct wlr_surface *layer_keyboard_surface(struct tw_server *server) {
	return server->keyboard_layer != NULL ?
		server->keyboard_layer->layer_surface->surface : NULL;
}

/*
 * The keyboard goes to the topmost mapped surface on the top or overlay layer
 * that asks for exclusive focus (a launcher). On-demand focus is not
 * implemented: such surfaces get pointer input only.
 */
static void layers_update_keyboard(struct tw_server *server) {
	struct tw_layer_surface *best = NULL, *surface;
	wl_list_for_each(surface, &server->layer_surfaces, link) {
		const struct wlr_layer_surface_v1_state *state = &surface->layer_surface->current;
		if (!surface->layer_surface->surface->mapped ||
				state->keyboard_interactive !=
					ZWLR_LAYER_SURFACE_V1_KEYBOARD_INTERACTIVITY_EXCLUSIVE ||
				state->layer < ZWLR_LAYER_SHELL_V1_LAYER_TOP) {
			continue;
		}
		/* The list is newest first, so among equals the newest wins. */
		if (best == NULL || state->layer > best->layer_surface->current.layer) {
			best = surface;
		}
	}
	if (best != server->keyboard_layer) {
		server->keyboard_layer = best;
		focus_update(server);
	}
}

static void arrange_pass(struct tw_server *server, struct wlr_output *output,
		const struct wlr_box *full, struct wlr_box *usable, bool exclusive) {
	/* Overlay first: higher layers claim their screen edges first. */
	for (int layer = ZWLR_LAYER_SHELL_V1_LAYER_OVERLAY;
			layer >= ZWLR_LAYER_SHELL_V1_LAYER_BACKGROUND; layer--) {
		struct tw_layer_surface *surface;
		wl_list_for_each(surface, &server->layer_surfaces, link) {
			struct wlr_layer_surface_v1 *layer_surface = surface->layer_surface;
			if (layer_surface->output != output || !layer_surface->initialized ||
					surface->awaiting_initial_commit ||
					(int)layer_surface->current.layer != layer ||
					(layer_surface->current.exclusive_zone > 0) != exclusive) {
				continue;
			}
			if (surface->configured && wlr_box_equal(&surface->configured_full, full) &&
					wlr_box_equal(&surface->configured_room, usable)) {
				*usable = surface->configured_left;
				continue;
			}
			/* Positions the surface, sends it a configure, and shrinks
			 * `usable` by its exclusive zone. */
			surface->configured_full = *full;
			surface->configured_room = *usable;
			wlr_scene_layer_surface_v1_configure(surface->scene, full, usable);
			wlr_scene_node_set_position(&surface->popup_tree->node,
				surface->scene->tree->node.x, surface->scene->tree->node.y);
			surface->configured_left = *usable;
			surface->configured = true;
		}
	}
}

void layers_arrange(struct tw_server *server) {
	struct tw_output *output;
	wl_list_for_each(output, &server->outputs, link) {
		struct wlr_box full;
		wlr_output_layout_get_box(server->output_layout, output->wlr_output, &full);
		struct wlr_box usable = full;
		if (!wlr_box_empty(&full)) {
			/* Surfaces that reserve space go first, so the rest are laid
			 * out in what remains. */
			arrange_pass(server, output->wlr_output, &full, &usable, true);
			arrange_pass(server, output->wlr_output, &full, &usable, false);
		}
		output->usable = usable;
	}
	layers_update_keyboard(server);
}

void layers_output_destroyed(struct tw_server *server, struct wlr_output *output) {
	/* Each one closing rearranges and gives the others on the output more
	 * room; they are closing too, and are not told of it first. */
	struct tw_layer_surface *surface, *tmp;
	wl_list_for_each(surface, &server->layer_surfaces, link) {
		if (surface->layer_surface->output == output) {
			surface->awaiting_initial_commit = true;
		}
	}
	wl_list_for_each_safe(surface, tmp, &server->layer_surfaces, link) {
		if (surface->layer_surface->output == output) {
			/* Sends `closed` and runs handle_destroy(), which unlinks it. */
			wlr_layer_surface_v1_destroy(surface->layer_surface);
		}
	}
}

/* Where a layer surface's menus go. Under the windows, a bar on the bottom
 * layer would open its menus behind them, and a bar on the top layer behind a
 * dialog. On the overlay layer, they go right above it, which is still above
 * the windows' own menus. */
static struct wlr_scene_tree *layer_popup_parent(struct tw_server *server, uint32_t layer) {
	return layer == ZWLR_LAYER_SHELL_V1_LAYER_OVERLAY ?
		server->layer_trees[ZWLR_LAYER_SHELL_V1_LAYER_OVERLAY] : server->popup_tree;
}

static void handle_map(struct wl_listener *listener, void *data) {
	/* Its place was settled at its initial commit, which arranges; what
	 * mapping changes is who may have the keyboard. And the room it keeps:
	 * wlroots takes a surface's exclusive zone off the room only once it
	 * is mapped, so a bar is arranged again now (and told the same size
	 * once more). strip_arrange() also hands the pointer to the surface now
	 * under it and recounts the visible idle inhibitors. */
	struct tw_layer_surface *surface = wl_container_of(listener, surface, map);
	if (surface->layer_surface->current.exclusive_zone > 0) {
		surface->configured = false;
		layers_arrange(surface->server);
	} else {
		layers_update_keyboard(surface->server);
	}
	strip_arrange(surface->server);
}

static void handle_unmap(struct wl_listener *listener, void *data) {
	struct tw_layer_surface *surface = wl_container_of(listener, surface, unmap);
	surface->awaiting_initial_commit = true;
	surface->configured = false;
	layers_arrange(surface->server);
	strip_arrange(surface->server);
}

static void handle_commit(struct wl_listener *listener, void *data) {
	struct tw_layer_surface *surface = wl_container_of(listener, surface, commit);
	struct wlr_layer_surface_v1 *layer_surface = surface->layer_surface;

	/* The initial commit needs a configure in reply; later ones matter only
	 * if they changed size, anchor, layer and the like. */
	if (layer_surface->initial_commit) {
		surface->awaiting_initial_commit = false;
		/* A surface over the windows that does not ask for the keyboard is
		 * a notification (tw-notify, mako) or an on-screen display: what it
		 * shows wants to be seen, so the screen comes on for it. A
		 * launcher asks for the keyboard and comes from input anyway. */
		if (layer_surface->current.layer >= ZWLR_LAYER_SHELL_V1_LAYER_TOP &&
				layer_surface->current.keyboard_interactive ==
					ZWLR_LAYER_SURFACE_V1_KEYBOARD_INTERACTIVITY_NONE) {
			idle_wake(surface->server);
		}
	} else if (layer_surface->current.committed == 0) {
		return;
	}
	surface->configured = false;
	if (layer_surface->current.committed & WLR_LAYER_SURFACE_V1_STATE_LAYER) {
		wlr_scene_node_reparent(&surface->scene->tree->node,
			surface->server->layer_trees[layer_surface->current.layer]);
		wlr_scene_node_reparent(&surface->popup_tree->node,
			layer_popup_parent(surface->server, layer_surface->current.layer));
	}
	layers_arrange(surface->server);
	strip_arrange(surface->server);
}

static void handle_new_popup(struct wl_listener *listener, void *data) {
	/* window.c already tracks the popup; it only lacked a place in the scene. */
	struct tw_layer_surface *surface = wl_container_of(listener, surface, new_popup);
	popup_attach(data, surface->popup_tree);
}

bool layer_popup_box(struct tw_server *server, struct wlr_xdg_popup *popup, struct wlr_box *box) {
	/* The layer surface the chain of popups hangs from. */
	struct wlr_surface *root = popup->parent;
	struct wlr_xdg_surface *xdg_surface;
	while (root != NULL && (xdg_surface = wlr_xdg_surface_try_from_wlr_surface(root)) != NULL &&
			xdg_surface->role == WLR_XDG_SURFACE_ROLE_POPUP && xdg_surface->popup != NULL) {
		root = xdg_surface->popup->parent;
	}
	struct tw_layer_surface *surface;
	wl_list_for_each(surface, &server->layer_surfaces, link) {
		int lx, ly;
		if (surface->layer_surface->surface != root || surface->layer_surface->output == NULL ||
				!wlr_scene_node_coords(&surface->scene->tree->node, &lx, &ly)) {
			continue;
		}
		wlr_output_layout_get_box(server->output_layout, surface->layer_surface->output, box);
		box->x -= lx;
		box->y -= ly;
		return !wlr_box_empty(box);
	}
	return false;
}

/*
 * wlroots unmaps a layer surface before it destroys it, whichever way it
 * goes, and handle_unmap() gave back the keyboard and the room it kept. One
 * that never mapped had neither. The scene node goes with the layer surface;
 * the menus' tree is tw's own, and their trees go with it.
 */
static void handle_destroy(struct wl_listener *listener, void *data) {
	struct tw_layer_surface *surface = wl_container_of(listener, surface, destroy);
	wl_list_remove(&surface->map.link);
	wl_list_remove(&surface->unmap.link);
	wl_list_remove(&surface->commit.link);
	wl_list_remove(&surface->destroy.link);
	wl_list_remove(&surface->new_popup.link);
	wl_list_remove(&surface->link);
	wlr_scene_node_destroy(&surface->popup_tree->node);
	free(surface);
}

static void handle_new_layer_surface(struct wl_listener *listener, void *data) {
	struct tw_server *server = wl_container_of(listener, server, new_layer_surface);
	struct wlr_layer_surface_v1 *layer_surface = data;

	/* A client that names no output (a launcher, tw-notify) gets the
	 * focused one, else one in use: never the laptop's own screen behind a
	 * closed lid, where it would wait unseen for a configure. */
	if (layer_surface->output == NULL) {
		struct tw_output *output = output_focused(server);
		if (output == NULL) {
			output = output_oldest(server, NULL);
		}
		if (output == NULL) {
			wlr_layer_surface_v1_destroy(layer_surface);
			return;
		}
		layer_surface->output = output->wlr_output;
	}
	/* Tell the client the screen's scale now, before it draws. Left to the
	 * scene it would hear of it once its first picture is on screen, drawn
	 * for scale 1, and would draw again: every launcher and notification
	 * twice as it opens on a screen at 1.25. */
	float scale = layer_surface->output->scale;
	wlr_fractional_scale_v1_notify_scale(layer_surface->surface, scale);
	wlr_surface_set_preferred_buffer_scale(layer_surface->surface, (int32_t)ceilf(scale));

	struct tw_layer_surface *surface = calloc(1, sizeof(*surface));
	if (surface == NULL) {
		wl_resource_post_no_memory(layer_surface->resource);
		return;
	}
	surface->server = server;
	surface->layer_surface = layer_surface;
	/* `pending.layer` is the layer named in the creation request. */
	surface->scene = wlr_scene_layer_surface_v1_create(
		server->layer_trees[layer_surface->pending.layer], layer_surface);
	surface->popup_tree = surface->scene == NULL ? NULL :
		wlr_scene_tree_create(layer_popup_parent(server, layer_surface->pending.layer));
	if (surface->popup_tree == NULL) {
		if (surface->scene != NULL) {
			wlr_scene_node_destroy(&surface->scene->tree->node);
		}
		free(surface);
		wl_resource_post_no_memory(layer_surface->resource);
		return;
	}

	surface->map.notify = handle_map;
	wl_signal_add(&layer_surface->surface->events.map, &surface->map);
	surface->unmap.notify = handle_unmap;
	wl_signal_add(&layer_surface->surface->events.unmap, &surface->unmap);
	surface->commit.notify = handle_commit;
	wl_signal_add(&layer_surface->surface->events.commit, &surface->commit);
	surface->destroy.notify = handle_destroy;
	wl_signal_add(&layer_surface->events.destroy, &surface->destroy);
	surface->new_popup.notify = handle_new_popup;
	wl_signal_add(&layer_surface->events.new_popup, &surface->new_popup);

	wl_list_insert(&server->layer_surfaces, &surface->link);
}

void layer_init(struct tw_server *server) {
	wl_list_init(&server->layer_surfaces);
	server->layer_shell = wlr_layer_shell_v1_create(server->display, 4);
	server->new_layer_surface.notify = handle_new_layer_surface;
	wl_signal_add(&server->layer_shell->events.new_surface, &server->new_layer_surface);
}

void layer_finish(struct tw_server *server) {
	wl_list_remove(&server->new_layer_surface.link);
}
