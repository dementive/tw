/*
 * Layer shell: surfaces that are not windows, such as launchers, notification
 * popups, wallpapers and bars. tw draws none of these itself; this is what
 * lets ordinary programs provide them.
 */
#include <stdlib.h>
#include <wlr/types/wlr_compositor.h>
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
	bool mapped;
	/* Set from unmap until the next initial commit. A configure sent in that
	 * window would carry a serial wlroots is about to forget, and a client
	 * that acked it would be killed with a protocol error. */
	bool awaiting_initial_commit;
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
 * that asks for exclusive focus (a launcher, a lock prompt). On-demand focus
 * is not implemented: such surfaces get pointer input only.
 */
static void layers_update_keyboard(struct tw_server *server) {
	struct tw_layer_surface *best = NULL, *surface;
	wl_list_for_each(surface, &server->layer_surfaces, link) {
		const struct wlr_layer_surface_v1_state *state = &surface->layer_surface->current;
		if (!surface->mapped ||
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
			/* Positions the surface, sends it a configure, and shrinks
			 * `usable` by its exclusive zone. wlroots sends the configure
			 * even if nothing changed, which is why this runs only when a
			 * layer surface or an output changes, never per window. */
			wlr_scene_layer_surface_v1_configure(surface->scene, full, usable);
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
	struct tw_layer_surface *surface, *tmp;
	wl_list_for_each_safe(surface, tmp, &server->layer_surfaces, link) {
		if (surface->layer_surface->output == output) {
			/* Sends `closed` and runs handle_destroy(), which unlinks it. */
			wlr_layer_surface_v1_destroy(surface->layer_surface);
		}
	}
}

static void handle_map(struct wl_listener *listener, void *data) {
	struct tw_layer_surface *surface = wl_container_of(listener, surface, map);
	surface->mapped = true;
	layers_arrange(surface->server);
	strip_arrange(surface->server);
}

static void handle_unmap(struct wl_listener *listener, void *data) {
	struct tw_layer_surface *surface = wl_container_of(listener, surface, unmap);
	surface->mapped = false;
	surface->awaiting_initial_commit = true;
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
	} else if (layer_surface->current.committed == 0) {
		return;
	}
	if (layer_surface->current.committed & WLR_LAYER_SURFACE_V1_STATE_LAYER) {
		wlr_scene_node_reparent(&surface->scene->tree->node,
			surface->server->layer_trees[layer_surface->current.layer]);
	}
	layers_arrange(surface->server);
	strip_arrange(surface->server);
}

static void handle_new_popup(struct wl_listener *listener, void *data) {
	/* window.c already tracks the popup; it only lacked a place in the scene. */
	struct tw_layer_surface *surface = wl_container_of(listener, surface, new_popup);
	struct wlr_xdg_popup *popup = data;
	if (popup->base->data == NULL) {
		popup->base->data = wlr_scene_xdg_surface_create(surface->scene->tree, popup->base);
	}
}

static void handle_destroy(struct wl_listener *listener, void *data) {
	struct tw_layer_surface *surface = wl_container_of(listener, surface, destroy);
	struct tw_server *server = surface->server;

	wl_list_remove(&surface->map.link);
	wl_list_remove(&surface->unmap.link);
	wl_list_remove(&surface->commit.link);
	wl_list_remove(&surface->destroy.link);
	wl_list_remove(&surface->new_popup.link);
	wl_list_remove(&surface->link);
	/* Unmap normally ran first and already handed the keyboard back. */
	if (server->keyboard_layer == surface) {
		server->keyboard_layer = NULL;
		focus_update(server);
	}
	/* wlroots destroys the scene node together with the layer surface. */
	free(surface);

	layers_arrange(server);
	strip_arrange(server);
}

static void handle_new_layer_surface(struct wl_listener *listener, void *data) {
	struct tw_server *server = wl_container_of(listener, server, new_layer_surface);
	struct wlr_layer_surface_v1 *layer_surface = data;

	/* A client that names no output gets the one showing the strip. */
	if (layer_surface->output == NULL) {
		struct tw_output *output = output_primary(server);
		if (output == NULL) {
			wlr_layer_surface_v1_destroy(layer_surface);
			return;
		}
		layer_surface->output = output->wlr_output;
	}

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
	if (surface->scene == NULL) {
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
