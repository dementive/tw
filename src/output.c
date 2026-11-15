#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <wlr/backend.h>
#include <wlr/types/wlr_output.h>
#include <wlr/types/wlr_output_layout.h>
#include <wlr/types/wlr_scene.h>
#include <wlr/util/log.h>

#include "tw.h"
#include "config.h"

/*
 * Several outputs. There are TW_WORKSPACES workspaces in all, and each output
 * shows one of them; the rest are hidden. The output showing the active
 * workspace has the focus. Unplugging an output only hides its workspace, so
 * no window ever has to be moved because a monitor went away.
 */

struct tw_output *output_showing(struct tw_server *server, size_t workspace) {
	struct tw_output *output;
	wl_list_for_each(output, &server->outputs, link) {
		struct wlr_box box;
		if (output->workspace != workspace) {
			continue;
		}
		wlr_output_layout_get_box(server->output_layout, output->wlr_output, &box);
		if (!wlr_box_empty(&box)) {
			return output;
		}
	}
	return NULL;
}

struct tw_output *output_focused(struct tw_server *server) {
	return output_showing(server, server->active);
}

struct tw_output *output_at(struct tw_server *server, double lx, double ly) {
	struct wlr_output *wlr_output = wlr_output_layout_output_at(server->output_layout, lx, ly);
	return wlr_output != NULL ? wlr_output->data : NULL;
}

struct tw_output *output_adjacent(struct tw_output *output, int direction) {
	if (output == NULL) {
		return NULL;
	}
	struct wlr_box box;
	struct wlr_output_layout *layout = output->server->output_layout;
	wlr_output_layout_get_box(layout, output->wlr_output, &box);
	struct wlr_output *next = wlr_output_layout_adjacent_output(layout, direction,
		output->wlr_output, box.x + box.width / 2.0, box.y + box.height / 2.0);
	return next != NULL ? next->data : NULL;
}

bool output_boxes(struct tw_output *output, struct wlr_box *area, struct wlr_box *full) {
	if (output == NULL || wlr_box_empty(&output->usable)) {
		return false;
	}
	*area = output->usable;
	wlr_output_layout_get_box(output->server->output_layout, output->wlr_output, full);
	return !wlr_box_empty(full);
}

static const struct tw_output_rule *output_rule(const char *name) {
	static const struct tw_output_rule fallback = {NULL, 1.0f, TW_AUTO, TW_AUTO};
	const struct tw_output_rule *rest = &fallback;
	for (size_t i = 0; i < LENGTH(output_rules); i++) {
		if (output_rules[i].name == NULL) {
			rest = &output_rules[i];
		} else if (strcmp(output_rules[i].name, name) == 0) {
			return &output_rules[i];
		}
	}
	return rest;
}

struct wlr_output_mode *output_mode(struct tw_output *output) {
	struct wlr_output_mode *best = wlr_output_preferred_mode(output->wlr_output);
	if (best == NULL || !output->server->on_battery || battery_refresh_hz <= 0) {
		return best;
	}
	/* The same picture, drawn less often. */
	int width = best->width, height = best->height;
	struct wlr_output_mode *mode;
	wl_list_for_each(mode, &output->wlr_output->modes, link) {
		if (mode->width == width && mode->height == height &&
				tw_refresh_saves(mode->refresh, best->refresh, battery_refresh_hz)) {
			best = mode;
		}
	}
	return best;
}

void outputs_update_mode(struct tw_server *server) {
	struct tw_output *output;
	wl_list_for_each(output, &server->outputs, link) {
		struct wlr_output *wlr_output = output->wlr_output;
		struct wlr_output_mode *mode = output_mode(output);
		/* An output that is off takes no mode; it gets this one as it comes
		 * back on. */
		if (mode == NULL || mode == wlr_output->current_mode || !wlr_output->enabled) {
			continue;
		}
		struct wlr_output_state state;
		wlr_output_state_init(&state);
		wlr_output_state_set_mode(&state, mode);
		/* The scene redraws the output by itself after a change of mode. */
		if (wlr_output_commit_state(wlr_output, &state)) {
			wlr_log(WLR_INFO, "output %s: now at %d.%03d Hz",
				wlr_output->name, mode->refresh / 1000, mode->refresh % 1000);
		} else {
			wlr_log(WLR_ERROR, "output %s: could not switch to %d.%03d Hz",
				wlr_output->name, mode->refresh / 1000, mode->refresh % 1000);
		}
		wlr_output_state_finish(&state);
	}
}

bool output_set_power(struct tw_output *output, bool on) {
	struct wlr_output_mode *mode = on ? output_mode(output) : NULL;
	for (;;) {
		struct wlr_output_state state;
		wlr_output_state_init(&state);
		wlr_output_state_set_enabled(&state, on);
		if (mode != NULL) {
			wlr_output_state_set_mode(&state, mode);
		}
		bool done = wlr_output_commit_state(output->wlr_output, &state);
		wlr_output_state_finish(&state);
		if (done || mode == NULL) {
			return done;
		}
		/* On in the mode it had beats staying dark. */
		mode = NULL;
	}
}

static void handle_frame(struct wl_listener *listener, void *data) {
	/*
	 * Fires only after something scheduled a frame (damage, or a previous
	 * commit). wlr_scene_output_commit() returns without touching the GPU
	 * when nothing changed, so an idle desktop renders nothing and this
	 * handler stops being called.
	 */
	struct tw_output *output = wl_container_of(listener, output, frame);
	struct tw_server *server = output->server;
	struct wlr_scene_output *scene_output =
		wlr_scene_get_scene_output(server->scene, output->wlr_output);

	/* A scroll animation moves one step per frame of the output showing the
	 * strip, and asks for the next frame only while it is still moving. */
	if (output->animating && strip_animate(output)) {
		wlr_output_schedule_frame(output->wlr_output);
	}

	wlr_scene_output_commit(scene_output, NULL);

	struct timespec now;
	clock_gettime(CLOCK_MONOTONIC, &now);
	wlr_scene_output_send_frame_done(scene_output, &now);
}

static void handle_request_state(struct wl_listener *listener, void *data) {
	/* The backend wants a new mode, e.g. the nested window was resized. The
	 * output layout notices the change and everything is re-arranged. */
	struct tw_output *output = wl_container_of(listener, output, request_state);
	const struct wlr_output_event_request_state *event = data;
	wlr_output_commit_state(output->wlr_output, event->state);
}

static void handle_destroy(struct wl_listener *listener, void *data) {
	struct tw_output *output = wl_container_of(listener, output, destroy);
	struct tw_server *server = output->server;
	layers_output_destroyed(server, output->wlr_output);
	wl_list_remove(&output->frame.link);
	wl_list_remove(&output->request_state.link);
	wl_list_remove(&output->destroy.link);
	wl_list_remove(&output->link);

	/* Its workspace is hidden from here on. If that is where the user was
	 * working, the oldest remaining output shows it instead of its own, so
	 * unplugging a monitor never takes the current work out of sight. The
	 * output layout raises its change event next, which arranges. */
	if (output->workspace == server->active && !wl_list_empty(&server->outputs)) {
		struct tw_output *heir = wl_container_of(server->outputs.next, heir, link);
		heir->workspace = server->active;
	}
	output->wlr_output->data = NULL;
	free(output);
}

static void handle_new_output(struct wl_listener *listener, void *data) {
	struct tw_server *server = wl_container_of(listener, server, new_output);
	struct wlr_output *wlr_output = data;

	struct tw_output *output = calloc(1, sizeof(*output));
	if (output == NULL) {
		wlr_log(WLR_ERROR, "out of memory, ignoring output %s", wlr_output->name);
		return;
	}
	if (!wlr_output_init_render(wlr_output, server->allocator, server->renderer)) {
		wlr_log(WLR_ERROR, "cannot draw on output %s, ignoring it", wlr_output->name);
		free(output);
		return;
	}
	output->server = server;
	output->wlr_output = wlr_output;
	wlr_output->data = output;
	const struct tw_output_rule *rule = output_rule(wlr_output->name);

	/* The first output shows the active workspace; each further one the
	 * lowest-numbered workspace that is not on screen yet. */
	output->workspace = TW_WORKSPACES;
	if (output_focused(server) == NULL) {
		output->workspace = server->active;
	} else {
		for (size_t i = 0; i < TW_WORKSPACES; i++) {
			if (output_showing(server, i) == NULL) {
				output->workspace = i;
				break;
			}
		}
	}

	struct wlr_output_state state;
	wlr_output_state_init(&state);
	wlr_output_state_set_enabled(&state, true);
	wlr_output_state_set_scale(&state, rule->scale);
	struct wlr_output_mode *mode = output_mode(output);
	if (mode != NULL) {
		wlr_output_state_set_mode(&state, mode);
	}
	if (!wlr_output_commit_state(wlr_output, &state) &&
			mode != wlr_output_preferred_mode(wlr_output)) {
		/* It refused the slower mode for running on battery. */
		wlr_output_state_set_mode(&state, wlr_output_preferred_mode(wlr_output));
		wlr_output_commit_state(wlr_output, &state);
	}
	wlr_output_state_finish(&state);

	output->frame.notify = handle_frame;
	wl_signal_add(&wlr_output->events.frame, &output->frame);
	output->request_state.notify = handle_request_state;
	wl_signal_add(&wlr_output->events.request_state, &output->request_state);
	output->destroy.notify = handle_destroy;
	wl_signal_add(&wlr_output->events.destroy, &output->destroy);

	wl_list_insert(server->outputs.prev, &output->link);
	wlr_log(WLR_DEBUG, "output %s: scale %.2f, shows workspace %zu",
		wlr_output->name, rule->scale, output->workspace + 1);

	/* Adding to the layout raises its change event, which arranges. */
	struct wlr_output_layout_output *layout_output = rule->x == TW_AUTO || rule->y == TW_AUTO ?
		wlr_output_layout_add_auto(server->output_layout, wlr_output) :
		wlr_output_layout_add(server->output_layout, wlr_output, rule->x, rule->y);
	struct wlr_scene_output *scene_output = wlr_scene_output_create(server->scene, wlr_output);
	wlr_scene_output_layout_add_output(server->scene_layout, layout_output, scene_output);
}

static void handle_layout_change(struct wl_listener *listener, void *data) {
	/* Layer surfaces first: they decide how much of the output is left. */
	struct tw_server *server = wl_container_of(listener, server, layout_change);
	layers_arrange(server);
	lock_arrange(server);
	strip_arrange(server);
}

void output_init(struct tw_server *server) {
	wl_list_init(&server->outputs);
	server->output_layout = wlr_output_layout_create(server->display);
	server->scene = wlr_scene_create();
	server->scene_layout = wlr_scene_attach_output_layout(server->scene, server->output_layout);
	if (server->linux_dmabuf != NULL) {
		/* Lets the scene tell each client which buffer formats the display
		 * can show directly, so a fullscreen window skips compositing. */
		wlr_scene_set_linux_dmabuf_v1(server->scene, server->linux_dmabuf);
	}

	/* Children stack in creation order. The layer indices are the values of
	 * zwlr_layer_shell_v1_layer: background, bottom, top, overlay. */
	struct wlr_scene_tree *root = &server->scene->tree;
	server->layer_trees[0] = wlr_scene_tree_create(root);
	server->layer_trees[1] = wlr_scene_tree_create(root);
	server->window_tree = wlr_scene_tree_create(root);
	server->layer_trees[2] = wlr_scene_tree_create(root);
	server->fullscreen_tree = wlr_scene_tree_create(root);
	server->float_tree = wlr_scene_tree_create(root);
	server->layer_trees[3] = wlr_scene_tree_create(root);
	server->lock_tree = wlr_scene_tree_create(root);

	server->new_output.notify = handle_new_output;
	wl_signal_add(&server->backend->events.new_output, &server->new_output);
	server->layout_change.notify = handle_layout_change;
	wl_signal_add(&server->output_layout->events.change, &server->layout_change);
}

void output_finish(struct tw_server *server) {
	wl_list_remove(&server->new_output.link);
	wl_list_remove(&server->layout_change.link);
}
