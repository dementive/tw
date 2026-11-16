#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <wlr/backend.h>
#include <wlr/render/allocator.h>
#include <wlr/render/wlr_renderer.h>
#include <wlr/types/wlr_compositor.h>
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

/*
 * The mode an output should be in now: its preferred one, or on battery the
 * slowest of that size that config.h allows. NULL for an output without
 * modes (a nested or headless one).
 */
static struct wlr_output_mode *output_mode(struct tw_output *output) {
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

/*
 * Turns an output on, in the mode it should be in now, or off; a scale above
 * 0 is set as well. A dark screen is the one failure this must not have, so
 * a mode the output refuses gives way to its preferred one, and that to the
 * mode it had.
 */
static bool output_commit(struct tw_output *output, bool on, float scale) {
	struct wlr_output_mode *mode = on ? output_mode(output) : NULL;
	for (;;) {
		struct wlr_output_state state;
		wlr_output_state_init(&state);
		wlr_output_state_set_enabled(&state, on);
		if (scale > 0) {
			wlr_output_state_set_scale(&state, scale);
		}
		if (mode != NULL) {
			wlr_output_state_set_mode(&state, mode);
		}
		bool done = wlr_output_commit_state(output->wlr_output, &state);
		wlr_output_state_finish(&state);
		if (done && !on) {
			/* A screen that is off needs no curtain drawn on it. */
			lock_confirm(output->server);
		}
		if (done || mode == NULL) {
			return done;
		}
		struct wlr_output_mode *preferred = wlr_output_preferred_mode(output->wlr_output);
		mode = mode != preferred ? preferred : NULL;
	}
}

bool output_set_power(struct tw_output *output, bool on) {
	return output_commit(output, on, 0);
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
	if (scene_output == NULL) {
		return; /* left dark; see output_enter() */
	}

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

/* The lock client may only hear that the session is locked once the curtain
 * is on screen (lock.c): it is when its first frame has been presented. */
static void handle_present(struct wl_listener *listener, void *data) {
	struct tw_output *output = wl_container_of(listener, output, present);
	const struct wlr_output_event_present *event = data;
	if (!output->server->locked || output->curtain_shown || !event->presented ||
			event->commit_seq <= output->lock_seq) {
		return;
	}
	output->curtain_shown = true;
	lock_confirm(output->server);
}

static void handle_request_state(struct wl_listener *listener, void *data) {
	/* The backend wants a new mode, e.g. the nested window was resized. The
	 * output layout notices the change and everything is re-arranged. */
	struct tw_output *output = wl_container_of(listener, output, request_state);
	const struct wlr_output_event_request_state *event = data;
	wlr_output_commit_state(output->wlr_output, event->state);
}

/* The oldest output in use, other than `except`; NULL if there is none. */
static struct tw_output *output_oldest(struct tw_server *server, struct tw_output *except) {
	struct tw_output *output;
	wl_list_for_each(output, &server->outputs, link) {
		if (output != except && !output->lid_off) {
			return output;
		}
	}
	return NULL;
}

/*
 * An output is leaving the layout, or going away. Its workspace is hidden
 * from here on. If that is where the user was working, the oldest remaining
 * output shows it instead of its own, so that an output going never takes
 * the current work out of sight.
 */
static void output_hand_over(struct tw_output *output) {
	struct tw_server *server = output->server;
	if (output->workspace != server->active || output->lid_off) {
		return;
	}
	struct tw_output *heir = output_oldest(server, output);
	if (heir != NULL) {
		heir->workspace = server->active;
		output->workspace = TW_WORKSPACES;
	}
}

static void handle_destroy(struct wl_listener *listener, void *data) {
	struct tw_output *output = wl_container_of(listener, output, destroy);
	struct tw_server *server = output->server;
	layers_output_destroyed(server, output->wlr_output);
	wl_list_remove(&output->frame.link);
	wl_list_remove(&output->present.link);
	wl_list_remove(&output->request_state.link);
	wl_list_remove(&output->destroy.link);
	/* The output layout raises its change event next, which arranges. */
	output_hand_over(output);
	wl_list_remove(&output->link);
	output->wlr_output->data = NULL;
	free(output);
	/* The last monitor of a docked laptop with its lid closed: the
	 * laptop's own screen comes back. */
	outputs_lid(server, server->lid.closed);
	/* The lock may have been waiting for this one. */
	lock_confirm(server);
}

/*
 * The workspace an output shows as it comes into the layout: the active one
 * if no output shows it, then the one it showed before if that is hidden,
 * then the lowest-numbered hidden one. TW_WORKSPACES if there are more
 * outputs than workspaces.
 */
static size_t output_pick_workspace(struct tw_output *output) {
	struct tw_server *server = output->server;
	if (output_showing(server, server->active) == NULL) {
		return server->active;
	}
	if (output->workspace < TW_WORKSPACES && output_showing(server, output->workspace) == NULL) {
		return output->workspace;
	}
	for (size_t i = 0; i < TW_WORKSPACES; i++) {
		if (output_showing(server, i) == NULL) {
			return i;
		}
	}
	return TW_WORKSPACES;
}

/* Turns an output on (unless the screens are off) and puts it in the layout,
 * showing a workspace. */
static void output_enter(struct tw_output *output) {
	struct tw_server *server = output->server;
	struct wlr_output *wlr_output = output->wlr_output;
	const struct tw_output_rule *rule = output_rule(wlr_output->name);
	output->lid_off = false;
	output->workspace = output_pick_workspace(output);

	/* While the screens are off a new one stays off with them, until the
	 * next input turns them all on (idle.c). */
	if (!output_commit(output, !server->blanked, rule->scale)) {
		wlr_log(WLR_ERROR, "output %s: could not be set up", wlr_output->name);
	}
	/* Coming in while locked: its first frame has the curtain on it. */
	output->curtain_shown = false;
	output->lock_seq = wlr_output->commit_seq;
	wlr_log(WLR_DEBUG, "output %s: scale %.2f, shows workspace %zu",
		wlr_output->name, rule->scale, output->workspace + 1);

	/* Adding to the layout raises its change event, which arranges. */
	struct wlr_output_layout_output *layout_output = rule->x == TW_AUTO || rule->y == TW_AUTO ?
		wlr_output_layout_add_auto(server->output_layout, wlr_output) :
		wlr_output_layout_add(server->output_layout, wlr_output, rule->x, rule->y);
	struct wlr_scene_output *scene_output = wlr_scene_output_create(server->scene, wlr_output);
	if (layout_output == NULL || scene_output == NULL) {
		/* Out of memory. The output stays dark; its frame handler copes. */
		wlr_log(WLR_ERROR, "output %s: out of memory, left dark", wlr_output->name);
		wlr_scene_output_destroy(scene_output);
		return;
	}
	wlr_scene_output_layout_add_output(server->scene_layout, layout_output, scene_output);
}

/*
 * Takes the laptop's own screen out of the layout and turns it off. Its
 * workspace goes as it does when an output is unplugged, and so do the
 * layer surfaces on it: with the output out of the layout its wl_output is
 * gone for clients too.
 */
static void output_leave(struct tw_output *output) {
	struct tw_server *server = output->server;
	struct wlr_output *wlr_output = output->wlr_output;
	output_hand_over(output);
	output->lid_off = true;
	output->animating = false;
	layers_output_destroyed(server, wlr_output);
	/* The scene would go on drawing on it where it was. */
	wlr_scene_output_destroy(wlr_scene_get_scene_output(server->scene, wlr_output));
	/* Raises the layout's change event, which arranges. */
	wlr_output_layout_remove(server->output_layout, wlr_output);
	if (!output_set_power(output, false)) {
		wlr_log(WLR_ERROR, "output %s: could not be turned off", wlr_output->name);
	}
}

/*
 * A laptop's own screen behind a closed lid is of no use while another output
 * is there, and in the layout it would keep a workspace, the pointer and new
 * windows where nobody can see them. Like niri, tw takes it out of use then
 * and brings it back when the lid opens or the last other output goes, so
 * that there is always a screen. The decision is tw_lid_look()'s.
 */
void outputs_lid(struct tw_server *server, bool closed) {
	int panels = 0, others = 0;
	struct tw_output *output;
	wl_list_for_each(output, &server->outputs, link) {
		if (output->builtin) {
			panels++;
		} else {
			others++;
		}
	}
	int due = tw_lid_look(&server->lid, closed, panels, others);
	bool in_use = due & TW_LID_PANEL;
	wl_list_for_each(output, &server->outputs, link) {
		if (!output->builtin || output->lid_off != in_use) {
			continue;
		}
		wlr_log(WLR_DEBUG, "output %s: %s", output->wlr_output->name,
			in_use ? "back in use" : "out of use behind the closed lid");
		if (in_use) {
			output_enter(output);
		} else {
			output_leave(output);
		}
	}
	/* The laptop's own screen is the only one and the lid is closed, so
	 * logind suspends the machine: the session has to be locked before it
	 * does. */
	if ((due & TW_LID_LOCK) && lock_on_lid_close) {
		lock_now(server);
	}
}

static void handle_new_output(struct wl_listener *listener, void *data) {
	struct tw_server *server = wl_container_of(listener, server, new_output);
	struct wlr_output *wlr_output = data;

	/* A VR headset: not a screen to put a desktop on. */
	if (wlr_output->non_desktop) {
		wlr_log(WLR_INFO, "output %s is not for a desktop, ignoring it", wlr_output->name);
		return;
	}
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
	output->workspace = TW_WORKSPACES;
	output->builtin = tw_connector_builtin(wlr_output->name);
	wlr_output->data = output;

	output->frame.notify = handle_frame;
	wl_signal_add(&wlr_output->events.frame, &output->frame);
	output->present.notify = handle_present;
	wl_signal_add(&wlr_output->events.present, &output->present);
	output->request_state.notify = handle_request_state;
	wl_signal_add(&wlr_output->events.request_state, &output->request_state);
	output->destroy.notify = handle_destroy;
	wl_signal_add(&wlr_output->events.destroy, &output->destroy);
	wl_list_insert(server->outputs.prev, &output->link);

	/* The laptop's own screen behind a closed lid waits for outputs_lid()
	 * to say whether it is wanted; any other output may be what takes it
	 * out of use. */
	if (output->builtin && server->lid.closed) {
		output->lid_off = true;
		output_set_power(output, false);
	} else {
		output_enter(output);
	}
	outputs_lid(server, server->lid.closed);
}

/*
 * The GPU was reset (a driver hang, a bad wake-up), and the renderer and all
 * it made are gone. A new renderer takes over, or every screen would stay
 * black until tw was started again. The scene drops the textures of the old
 * one by itself and clients' buffers are imported again as they are drawn.
 */
static void handle_renderer_lost(struct wl_listener *listener, void *data) {
	struct tw_server *server = wl_container_of(listener, server, renderer_lost);
	wlr_log(WLR_INFO, "the GPU was reset; starting the renderer again");
	struct wlr_renderer *renderer = wlr_renderer_autocreate(server->backend);
	struct wlr_allocator *allocator = renderer != NULL ?
		wlr_allocator_autocreate(server->backend, renderer) : NULL;
	if (allocator == NULL) {
		wlr_log(WLR_ERROR, "could not start the renderer again; restart tw");
		wlr_renderer_destroy(renderer);
		return;
	}
	struct wlr_renderer *old_renderer = server->renderer;
	struct wlr_allocator *old_allocator = server->allocator;
	server->renderer = renderer;
	server->allocator = allocator;
	wl_list_remove(&server->renderer_lost.link);
	wl_signal_add(&renderer->events.lost, &server->renderer_lost);
	wlr_compositor_set_renderer(server->compositor, renderer);
	bool all = true;
	struct tw_output *output;
	wl_list_for_each(output, &server->outputs, link) {
		if (!wlr_output_init_render(output->wlr_output, allocator, renderer)) {
			wlr_log(WLR_ERROR, "output %s: cannot draw on it again", output->wlr_output->name);
			all = false;
		}
	}
	/* An output that could not switch still draws with the old ones: they
	 * fail on a GPU that was reset, but freed they would crash tw. */
	if (all) {
		wlr_allocator_destroy(old_allocator);
		wlr_renderer_destroy(old_renderer);
	}
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
	server->popup_tree = wlr_scene_tree_create(root);
	server->layer_trees[3] = wlr_scene_tree_create(root);
	server->lock_tree = wlr_scene_tree_create(root);
	server->drag_icon_tree = wlr_scene_tree_create(root);

	server->new_output.notify = handle_new_output;
	wl_signal_add(&server->backend->events.new_output, &server->new_output);
	server->layout_change.notify = handle_layout_change;
	wl_signal_add(&server->output_layout->events.change, &server->layout_change);
	server->renderer_lost.notify = handle_renderer_lost;
	wl_signal_add(&server->renderer->events.lost, &server->renderer_lost);
}

void output_finish(struct tw_server *server) {
	wl_list_remove(&server->new_output.link);
	wl_list_remove(&server->layout_change.link);
	wl_list_remove(&server->renderer_lost.link);
}
