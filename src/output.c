#include <stdlib.h>
#include <time.h>
#include <wlr/backend.h>
#include <wlr/types/wlr_output.h>
#include <wlr/types/wlr_output_layout.h>
#include <wlr/types/wlr_scene.h>
#include <wlr/util/log.h>

#include "tw.h"
#include "config.h"

struct tw_output {
	struct wl_list link; /* tw_server.outputs */
	struct tw_server *server;
	struct wlr_output *wlr_output;
	struct wl_listener frame;
	struct wl_listener request_state;
	struct wl_listener destroy;
};

bool output_primary_box(struct tw_server *server, struct wlr_box *box) {
	struct tw_output *output;
	wl_list_for_each(output, &server->outputs, link) {
		wlr_output_layout_get_box(server->output_layout, output->wlr_output, box);
		if (!wlr_box_empty(box)) {
			return true;
		}
	}
	return false;
}

static void handle_frame(struct wl_listener *listener, void *data) {
	/*
	 * Fires only after something scheduled a frame (damage, or a previous
	 * commit). wlr_scene_output_commit() returns without touching the GPU
	 * when nothing changed, so an idle desktop renders nothing and this
	 * handler stops being called.
	 */
	struct tw_output *output = wl_container_of(listener, output, frame);
	struct wlr_scene_output *scene_output =
		wlr_scene_get_scene_output(output->server->scene, output->wlr_output);

	wlr_scene_output_commit(scene_output, NULL);

	struct timespec now;
	clock_gettime(CLOCK_MONOTONIC, &now);
	wlr_scene_output_send_frame_done(scene_output, &now);
}

static void handle_request_state(struct wl_listener *listener, void *data) {
	/* The backend wants a new mode, e.g. the nested window was resized. The
	 * output layout notices the change and re-arranges the strip. */
	struct tw_output *output = wl_container_of(listener, output, request_state);
	const struct wlr_output_event_request_state *event = data;
	wlr_output_commit_state(output->wlr_output, event->state);
}

static void handle_destroy(struct wl_listener *listener, void *data) {
	struct tw_output *output = wl_container_of(listener, output, destroy);
	wl_list_remove(&output->frame.link);
	wl_list_remove(&output->request_state.link);
	wl_list_remove(&output->destroy.link);
	wl_list_remove(&output->link);
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
	output->server = server;
	output->wlr_output = wlr_output;

	wlr_output_init_render(wlr_output, server->allocator, server->renderer);

	struct wlr_output_state state;
	wlr_output_state_init(&state);
	wlr_output_state_set_enabled(&state, true);
	wlr_output_state_set_scale(&state, output_scale);
	struct wlr_output_mode *mode = wlr_output_preferred_mode(wlr_output);
	if (mode != NULL) {
		wlr_output_state_set_mode(&state, mode);
	}
	wlr_output_commit_state(wlr_output, &state);
	wlr_output_state_finish(&state);

	output->frame.notify = handle_frame;
	wl_signal_add(&wlr_output->events.frame, &output->frame);
	output->request_state.notify = handle_request_state;
	wl_signal_add(&wlr_output->events.request_state, &output->request_state);
	output->destroy.notify = handle_destroy;
	wl_signal_add(&wlr_output->events.destroy, &output->destroy);

	/* Append, so the output that shows the strip does not change when
	 * another one is plugged in. */
	wl_list_insert(server->outputs.prev, &output->link);

	struct wlr_output_layout_output *layout_output =
		wlr_output_layout_add_auto(server->output_layout, wlr_output);
	struct wlr_scene_output *scene_output = wlr_scene_output_create(server->scene, wlr_output);
	wlr_scene_output_layout_add_output(server->scene_layout, layout_output, scene_output);
}

static void handle_layout_change(struct wl_listener *listener, void *data) {
	struct tw_server *server = wl_container_of(listener, server, layout_change);
	strip_arrange(server);
}

void output_init(struct tw_server *server) {
	wl_list_init(&server->outputs);
	server->output_layout = wlr_output_layout_create(server->display);
	server->scene = wlr_scene_create();
	server->scene_layout = wlr_scene_attach_output_layout(server->scene, server->output_layout);

	server->new_output.notify = handle_new_output;
	wl_signal_add(&server->backend->events.new_output, &server->new_output);
	server->layout_change.notify = handle_layout_change;
	wl_signal_add(&server->output_layout->events.change, &server->layout_change);
}

void output_finish(struct tw_server *server) {
	wl_list_remove(&server->new_output.link);
	wl_list_remove(&server->layout_change.link);
}
