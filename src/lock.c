/*
 * Session lock (ext-session-lock-v1), the protocol swaylock uses.
 *
 * tw draws no lock screen itself. While locked it hides every window behind
 * an opaque curtain, gives input only to the lock client's surfaces, and runs
 * no key bindings. If the lock client dies the session stays locked behind
 * the curtain; a new lock client can then take over and unlock.
 */
#include <stdlib.h>
#include <wlr/types/wlr_compositor.h>
#include <wlr/types/wlr_output.h>
#include <wlr/types/wlr_output_layout.h>
#include <wlr/types/wlr_scene.h>
#include <wlr/types/wlr_session_lock_v1.h>
#include <wlr/util/log.h>

#include "tw.h"

struct tw_lock {
	struct tw_server *server;
	struct wlr_session_lock_v1 *session_lock;
	/* Parent of the lock surfaces; destroying it removes them all. */
	struct wlr_scene_tree *tree;
	struct wl_list surfaces; /* tw_lock_surface.link */
	struct wl_listener new_surface;
	struct wl_listener unlock;
	struct wl_listener destroy;
};

struct tw_lock_surface {
	struct wl_list link; /* tw_lock.surfaces */
	struct tw_lock *lock;
	struct wlr_session_lock_surface_v1 *lock_surface;
	struct wlr_scene_tree *tree;
	int width, height; /* last size sent, to avoid redundant configures */
	struct wl_listener destroy;
};

struct wlr_surface *lock_keyboard_surface(struct tw_server *server) {
	if (server->lock == NULL) {
		return NULL;
	}
	/* Prefer the surface on the output the user works on. */
	struct tw_output *primary = output_primary(server);
	struct tw_lock_surface *surface, *any = NULL;
	wl_list_for_each(surface, &server->lock->surfaces, link) {
		if (primary != NULL && surface->lock_surface->output == primary->wlr_output) {
			return surface->lock_surface->surface;
		}
		any = surface;
	}
	return any != NULL ? any->lock_surface->surface : NULL;
}

static void lock_surface_arrange(struct tw_lock_surface *surface) {
	struct tw_server *server = surface->lock->server;
	struct wlr_box box;
	wlr_output_layout_get_box(server->output_layout, surface->lock_surface->output, &box);
	if (wlr_box_empty(&box)) {
		return;
	}
	wlr_scene_node_set_position(&surface->tree->node, box.x, box.y);
	if (box.width != surface->width || box.height != surface->height) {
		surface->width = box.width;
		surface->height = box.height;
		wlr_session_lock_surface_v1_configure(surface->lock_surface,
			(uint32_t)box.width, (uint32_t)box.height);
	}
}

void lock_arrange(struct tw_server *server) {
	/* The curtain covers every output, whatever the lock client draws. */
	struct wlr_box extents;
	wlr_output_layout_get_box(server->output_layout, NULL, &extents);
	wlr_scene_node_set_position(&server->lock_background->node, extents.x, extents.y);
	wlr_scene_rect_set_size(server->lock_background, extents.width, extents.height);

	if (server->lock != NULL) {
		struct tw_lock_surface *surface;
		wl_list_for_each(surface, &server->lock->surfaces, link) {
			lock_surface_arrange(surface);
		}
	}
}

static void handle_surface_destroy(struct wl_listener *listener, void *data) {
	struct tw_lock_surface *surface = wl_container_of(listener, surface, destroy);
	struct tw_server *server = surface->lock->server;
	wl_list_remove(&surface->destroy.link);
	wl_list_remove(&surface->link);
	/* Its scene tree goes with the wl_surface, or with lock->tree. */
	free(surface);
	focus_update(server);
}

static void handle_new_surface(struct wl_listener *listener, void *data) {
	struct tw_lock *lock = wl_container_of(listener, lock, new_surface);
	struct wlr_session_lock_surface_v1 *lock_surface = data;

	struct tw_lock_surface *surface = calloc(1, sizeof(*surface));
	if (surface == NULL) {
		wl_resource_post_no_memory(lock_surface->resource);
		return;
	}
	surface->lock = lock;
	surface->lock_surface = lock_surface;
	surface->tree = wlr_scene_subsurface_tree_create(lock->tree, lock_surface->surface);
	if (surface->tree == NULL) {
		free(surface);
		wl_resource_post_no_memory(lock_surface->resource);
		return;
	}
	surface->destroy.notify = handle_surface_destroy;
	wl_signal_add(&lock_surface->events.destroy, &surface->destroy);
	wl_list_insert(&lock->surfaces, &surface->link);

	lock_surface_arrange(surface);
	focus_update(lock->server);
}

static void handle_unlock(struct wl_listener *listener, void *data) {
	/* The lock object is destroyed right after; handle_destroy() frees it. */
	struct tw_lock *lock = wl_container_of(listener, lock, unlock);
	struct tw_server *server = lock->server;
	server->locked = false;
	wlr_scene_node_set_enabled(&server->lock_tree->node, false);
	wlr_log(WLR_DEBUG, "session unlocked");
}

static void handle_destroy(struct wl_listener *listener, void *data) {
	struct tw_lock *lock = wl_container_of(listener, lock, destroy);
	struct tw_server *server = lock->server;

	/* wlroots destroyed the lock surfaces first, so the list is empty. */
	wl_list_remove(&lock->new_surface.link);
	wl_list_remove(&lock->unlock.link);
	wl_list_remove(&lock->destroy.link);
	wlr_scene_node_destroy(&lock->tree->node);
	server->lock = NULL;
	free(lock);

	if (server->locked) {
		wlr_log(WLR_ERROR, "the lock client went away without unlocking; staying locked");
	}
	/* Unlocked: windows and focus come back. Abandoned: the keyboard goes
	 * nowhere until a new lock client takes over. */
	focus_update(server);
	strip_arrange(server);
}

static void handle_new_lock(struct wl_listener *listener, void *data) {
	struct tw_server *server = wl_container_of(listener, server, new_lock);
	struct wlr_session_lock_v1 *session_lock = data;

	/* One lock client at a time. Taking over is allowed only when the
	 * previous one is gone. */
	if (server->lock != NULL) {
		wlr_session_lock_v1_destroy(session_lock);
		return;
	}
	struct tw_lock *lock = calloc(1, sizeof(*lock));
	struct wlr_scene_tree *tree = lock != NULL ? wlr_scene_tree_create(server->lock_tree) : NULL;
	if (tree == NULL) {
		free(lock);
		wlr_session_lock_v1_destroy(session_lock);
		return;
	}
	lock->server = server;
	lock->session_lock = session_lock;
	lock->tree = tree;
	wl_list_init(&lock->surfaces);

	lock->new_surface.notify = handle_new_surface;
	wl_signal_add(&session_lock->events.new_surface, &lock->new_surface);
	lock->unlock.notify = handle_unlock;
	wl_signal_add(&session_lock->events.unlock, &lock->unlock);
	lock->destroy.notify = handle_destroy;
	wl_signal_add(&session_lock->events.destroy, &lock->destroy);

	server->lock = lock;
	server->locked = true;
	lock_arrange(server);
	wlr_scene_node_set_enabled(&server->lock_tree->node, true);
	/* Takes the keyboard from the windows, then hides and suspends them. */
	focus_update(server);
	strip_arrange(server);
	wlr_session_lock_v1_send_locked(session_lock);
	wlr_log(WLR_DEBUG, "session locked");
}

void lock_init(struct tw_server *server) {
	static const float black[4] = {0.0f, 0.0f, 0.0f, 1.0f};
	server->lock_background = wlr_scene_rect_create(server->lock_tree, 0, 0, black);
	wlr_scene_node_set_enabled(&server->lock_tree->node, false);

	server->lock_manager = wlr_session_lock_manager_v1_create(server->display);
	server->new_lock.notify = handle_new_lock;
	wl_signal_add(&server->lock_manager->events.new_lock, &server->new_lock);
}

void lock_finish(struct tw_server *server) {
	wl_list_remove(&server->new_lock.link);
}
