#include <EGL/egl.h>
#include <GLES2/gl2.h>
#include <malloc.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <wlr/backend.h>
#include <wlr/render/allocator.h>
#include <wlr/render/egl.h>
#include <wlr/render/gles2.h>
#include <wlr/render/wlr_renderer.h>
#include <wlr/types/wlr_compositor.h>
#include <wlr/types/wlr_content_type_v1.h>
#include <wlr/types/wlr_cursor.h>
#include <wlr/types/wlr_layer_shell_v1.h>
#include <wlr/types/wlr_output.h>
#include <wlr/types/wlr_output_layout.h>
#include <wlr/types/wlr_output_management_v1.h>
#include <wlr/types/wlr_scene.h>
#include <wlr/types/wlr_session_lock_v1.h>
#include <wlr/types/wlr_tearing_control_v1.h>
#include <wlr/types/wlr_xdg_shell.h>
#include <wlr/util/log.h>

#include "tw.h"
#include "config.h"

/*
 * Several outputs. Each monitor has nine workspaces of its own (a desk,
 * strip.c's) and shows one of them. The output showing the active workspace
 * has the focus.
 *
 * Whether an output holds its desk depends only on its being in use, not on
 * its having a mode yet: one plugged in while the screens are off has none
 * until they come on. output_boxes() fails for it, so its workspace waits
 * suspended.
 */
struct tw_output *output_focused(struct tw_server *server) {
	return server->active->desk->output;
}

struct tw_output *output_oldest(struct tw_server *server, struct tw_output *except) {
	struct tw_output *output;
	wl_list_for_each(output, &server->outputs, link) {
		if (output != except && output_in_use(output)) {
			return output;
		}
	}
	return NULL;
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

static bool rule_for(const struct tw_output_rule *rule, const struct wlr_output *output) {
	return tw_output_named(rule->name, output->name, output->make, output->model,
		output->serial);
}

/* What machines.h says about this output: the machine's entry for it, else
 * the monitor's, else scale 1 at AUTO. */
static const struct tw_output_rule *output_rule(struct tw_output *output) {
	static const struct tw_output_rule other = {NULL, 1.0f, TW_AUTO, TW_AUTO};
	const struct tw_output_rule *rules = output->server->machine->outputs;
	for (size_t i = 0; i < TW_MACHINE_OUTPUTS && rules[i].name != NULL; i++) {
		if (rule_for(&rules[i], output->wlr_output)) {
			return &rules[i];
		}
	}
	for (size_t i = 0; monitors[i].name != NULL; i++) {
		if (rule_for(&monitors[i], output->wlr_output)) {
			return &monitors[i];
		}
	}
	return &other;
}

/* Its scale: the monitors tool's choice, else machines.h's. */
static float output_scale(struct tw_output *output) {
	return output->chosen.scale > 0 ? output->chosen.scale : output_rule(output)->scale;
}

/*
 * The mode an output should be in now: the one the monitors tool chose, else
 * its preferred one; on battery the slowest of that size that config.h
 * allows. NULL for an output without modes (a nested or headless one).
 */
static struct wlr_output_mode *output_mode(struct tw_output *output) {
	struct wlr_output_mode *best = output->chosen.mode != NULL ? output->chosen.mode :
		wlr_output_preferred_mode(output->wlr_output);
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

/* Commits power, the output's scale (output_scale()) and a mode (NULL keeps
 * the one it has), or with `test` only asks whether it would take them.
 * wlroots leaves out of the commit what would not change. */
static bool output_try(struct tw_output *output, bool on,
		struct wlr_output_mode *mode, bool test) {
	struct wlr_output *wlr_output = output->wlr_output;
	struct wlr_output_state state;
	wlr_output_state_init(&state);
	wlr_output_state_set_enabled(&state, on);
	wlr_output_state_set_scale(&state, output_scale(output));
	if (mode != NULL) {
		wlr_output_state_set_mode(&state, mode);
	}
	bool done = test ? wlr_output_test_state(wlr_output, &state) :
		wlr_output_commit_state(wlr_output, &state);
	wlr_output_state_finish(&state);
	return done;
}

/*
 * A dark screen is the one failure this must not have, so a mode the output
 * refuses gives way to its preferred one, then to the mode it had, then to
 * the first of its modes it takes: a dock, a hub or a cable may not carry
 * the preferred one, and a monitor just plugged in has no mode to keep. The
 * scale goes with every commit, so that an output that refused to come on
 * the first time still gets it when it does.
 */
bool output_set_power(struct tw_output *output, bool on) {
	struct wlr_output *wlr_output = output->wlr_output;
	struct wlr_output_mode *wanted = on ? output_mode(output) : NULL;
	bool done = output_try(output, on, wanted, false);
	/* Only an output with modes has others to fall back on. */
	if (!done && wanted != NULL) {
		struct wlr_output_mode *preferred = wlr_output_preferred_mode(wlr_output);
		struct wlr_output_mode *had = wlr_output->current_mode;
		if (preferred != wanted) {
			done = output_try(output, on, preferred, false);
		}
		if (!done && had != NULL && had != wanted && had != preferred) {
			done = output_try(output, on, had, false);
		}
		struct wlr_output_mode *mode;
		wl_list_for_each(mode, &wlr_output->modes, link) {
			if (done) {
				break;
			}
			if (mode != wanted && mode != preferred && mode != had &&
					output_try(output, on, mode, true)) {
				done = output_try(output, on, mode, false);
			}
		}
	}
	if (done && !on) {
		/* A screen that is off needs no curtain drawn on it. */
		lock_confirm(output->server);
	}
	return done;
}

/* The fullscreen window this output shows, or NULL: one that is on screen,
 * not scrolled away or hidden behind the lock. */
static struct tw_window *output_fullscreen(const struct tw_output *output) {
	struct tw_workspace *workspace = output_workspace(output);
	if (workspace == NULL) {
		return NULL;
	}
	struct tw_window *window = workspace_fullscreen(workspace);
	return window != NULL && window->scene_tree->node.enabled ? window : NULL;
}

struct content_look {
	struct wlr_content_type_manager_v1 *manager;
	bool moving;
};

static void buffer_content(struct wlr_scene_buffer *buffer, int sx, int sy, void *data) {
	struct content_look *look = data;
	struct wlr_scene_surface *scene_surface = wlr_scene_surface_try_from_buffer(buffer);
	if (look->moving || scene_surface == NULL) {
		return;
	}
	/* A subsurface (a browser's video) may say it itself, else its window's word counts. */
	struct wlr_surface *surface = scene_surface->surface;
	struct wlr_surface *root = wlr_surface_get_root_surface(surface);
	enum wp_content_type_v1_type type = wlr_surface_get_content_type_v1(look->manager, surface);
	if (type == WP_CONTENT_TYPE_V1_TYPE_NONE) {
		type = wlr_surface_get_content_type_v1(look->manager, root);
	}
	look->moving = type == WP_CONTENT_TYPE_V1_TYPE_VIDEO || type == WP_CONTENT_TYPE_V1_TYPE_GAME;
}

/* Something on this output says it shows a video or a game (content-type-v1:
 * mpv, browsers playing a video, games through SDL). */
static bool output_plays(struct wlr_scene_output *scene_output, struct tw_server *server) {
	struct content_look look = {server->content_type, false};
	wlr_scene_output_for_each_buffer(scene_output, buffer_content, &look);
	return look.moving;
}

/*
 * The frame cap on battery (battery_idle_fps): whether this output shows at
 * most that many frames a second now. Not while anything is going on that
 * wants every frame: input lately, an animation, a visible window keeping the
 * screen on or saying it shows a video or a game, a fullscreen window, a
 * program holding the pointer, the lock screen.
 */
static bool frame_capped(const struct tw_output *output, struct wlr_scene_output *scene_output,
		int64_t now) {
	struct tw_server *server = output->server;
	if (battery_idle_fps <= 0 || output->frame_timer == NULL || !server->on_battery ||
			server->locked || server->idle_inhibited || server->active_constraint != NULL ||
			output->animating ||
			now - server->last_activity_ms < (int64_t)battery_idle_after_seconds * 1000) {
		return false;
	}
	return (output_workspace(output) == NULL ||
		workspace_fullscreen(output_workspace(output)) == NULL) &&
		!output_plays(scene_output, server);
}

static bool frame_unhold(struct tw_output *output) {
	if (!output->frame_held) {
		return false;
	}
	output->frame_held = false;
	wl_event_source_timer_update(output->frame_timer, 0);
	return true;
}

void outputs_release_frames(struct tw_server *server) {
	struct tw_output *output;
	wl_list_for_each(output, &server->outputs, link) {
		if (frame_unhold(output)) {
			wlr_output_schedule_frame(output->wlr_output);
		}
	}
}

static int handle_frame_timer(void *data) {
	struct tw_output *output = data;
	output->frame_held = false;
	wlr_output_schedule_frame(output->wlr_output);
	return 0;
}

/*
 * Draws a frame, and with it asks the display for what the fullscreen
 * window on it calls for:
 *
 * A variable refresh rate (fullscreen_vrr), while the window is on screen:
 * the screen then waits for the game's next frame. Asked for in the commit
 * of a frame whenever the display's state differs from what is wanted; one
 * the display refused is not asked again until what is wanted changes.
 *
 * Tearing, when the window asks for it (tearing-control-v1, as a game with
 * vsync off does): its frame is shown at once rather than at the next
 * refresh. The display may refuse it for a frame (a moving pointer, a
 * screen it cannot flip early), so each frame is tested first.
 */
static void output_draw(struct tw_output *output, struct wlr_scene_output *scene_output) {
	struct wlr_output *wlr_output = output->wlr_output;
	struct wlr_output_state state;
	wlr_output_state_init(&state);
	if (!wlr_scene_output_build_state(scene_output, &state, NULL)) {
		wlr_output_state_finish(&state);
		return;
	}
	struct tw_window *fullscreen = output_fullscreen(output);
	bool vrr = fullscreen_vrr && fullscreen != NULL;
	if (vrr == (wlr_output->adaptive_sync_status == WLR_OUTPUT_ADAPTIVE_SYNC_ENABLED)) {
		output->vrr_refused = false;
	} else if (!output->vrr_refused && wlr_output->adaptive_sync_supported) {
		wlr_output_state_set_adaptive_sync_enabled(&state, vrr);
		if (wlr_output_test_state(wlr_output, &state)) {
			wlr_log(WLR_DEBUG, "output %s: variable refresh rate %s",
				wlr_output->name, vrr ? "on" : "off");
		} else {
			state.committed &= ~WLR_OUTPUT_STATE_ADAPTIVE_SYNC_ENABLED;
			output->vrr_refused = true;
			wlr_log(WLR_INFO, "output %s: the variable refresh rate could not be turned %s",
				wlr_output->name, vrr ? "on" : "off");
		}
	}
	if (fullscreen != NULL && wlr_tearing_control_manager_v1_surface_hint_from_surface(
			output->server->tearing_control, fullscreen->xdg_toplevel->base->surface) ==
			WP_TEARING_CONTROL_V1_PRESENTATION_HINT_ASYNC) {
		state.tearing_page_flip = true;
		state.tearing_page_flip = wlr_output_test_state(wlr_output, &state);
	}
	wlr_output_commit_state(wlr_output, &state);
	wlr_output_state_finish(&state);
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

	/* Too soon after the last frame while capped: held back until its turn,
	 * and so is the frame-done event, so that a client waiting for it (most
	 * do) draws nothing meanwhile. Input lets it through at once. */
	int64_t now_msec = now_ms();
	if (frame_capped(output, scene_output, now_msec)) {
		int64_t wait = 1000 / battery_idle_fps - (now_msec - output->last_frame_ms);
		if (wait > 0) {
			if (!output->frame_held) {
				output->frame_held = true;
				wl_event_source_timer_update(output->frame_timer, (int)wait);
			}
			return;
		}
	}
	frame_unhold(output);

	/* A scroll animation moves one step per frame of the output showing the
	 * strip, and asks for the next frame only while it is still moving. */
	if (strip_animate(output)) {
		wlr_output_schedule_frame(output->wlr_output);
	}

	if (wlr_scene_output_needs_frame(scene_output)) {
		output->last_frame_ms = now_msec;
		output_draw(output, scene_output);
	}

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
			(int32_t)(event->commit_seq - output->lock_seq) <= 0) { /* wraps */
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

/* Detaches an output from its wlr_output and frees it. */
static void output_free(struct tw_output *output) {
	if (output->frame_timer != NULL) {
		wl_event_source_remove(output->frame_timer);
	}
	wl_list_remove(&output->frame.link);
	wl_list_remove(&output->present.link);
	wl_list_remove(&output->request_state.link);
	wl_list_remove(&output->destroy.link);
	wl_list_remove(&output->link);
	output->wlr_output->data = NULL;
	free(output);
}

static void handle_destroy(struct wl_listener *listener, void *data) {
	struct tw_output *output = wl_container_of(listener, output, destroy);
	struct tw_server *server = output->server;
	layers_output_destroyed(server, output->wlr_output);
	/* The output layout raises its change event next, which arranges. */
	strip_output_leave(output);
	output_free(output);
	/* The last monitor of a docked laptop with its lid closed: the
	 * laptop's own screen comes back. */
	outputs_lid(server, server->lid.closed);
	/* The lock may have been waiting for this one. */
	lock_confirm(server);
	outputs_restore(server);
}

/* Turns an output on (unless the screens are off) and puts it in the layout,
 * showing a workspace of its monitor's desk. */
static void output_enter(struct tw_output *output) {
	struct tw_server *server = output->server;
	struct wlr_output *wlr_output = output->wlr_output;
	const struct tw_output_rule *rule = output_rule(output);
	output->lid_off = false;

	/* While the screens are off a new one stays off with them, until the
	 * next input turns them all on (idle.c). */
	if (!output_set_power(output, !server->blanked)) {
		wlr_log(WLR_ERROR, "output %s: could not be set up", wlr_output->name);
	}
	/* Coming in while locked: its first frame has the curtain on it. */
	output->curtain_shown = false;
	output->lock_seq = wlr_output->commit_seq;
	if (wlr_output->make != NULL && wlr_output->model != NULL) {
		wlr_log(WLR_DEBUG, "output %s is %s %s%s%s", wlr_output->name, wlr_output->make,
			wlr_output->model, wlr_output->serial != NULL ? " " : "",
			wlr_output->serial != NULL ? wlr_output->serial : "");
	}
	wlr_log(WLR_DEBUG, "output %s: scale %.2f", wlr_output->name, output_scale(output));
	strip_output_enter(output);

	/* Where the monitors tool put it, else where machines.h does. Adding to
	 * the layout raises its change event, which arranges. */
	int x = output->chosen.placed ? output->chosen.x : rule->x;
	int y = output->chosen.placed ? output->chosen.y : rule->y;
	struct wlr_output_layout_output *layout_output = x == TW_AUTO || y == TW_AUTO ?
		wlr_output_layout_add_auto(server->output_layout, wlr_output) :
		wlr_output_layout_add(server->output_layout, wlr_output, x, y);
	struct wlr_scene_output *scene_output = wlr_scene_output_create(server->scene, wlr_output);
	if (layout_output == NULL || scene_output == NULL) {
		/* Out of memory, or no sync timeline. The output stays dark; its
		 * frame handler copes. */
		wlr_log(WLR_ERROR, "output %s: cannot draw on it, left dark", wlr_output->name);
		wlr_scene_output_destroy(scene_output);
		return;
	}
	wlr_scene_output_layout_add_output(server->scene_layout, layout_output, scene_output);
}

/*
 * Takes an output out of the layout and turns it off, setting `why` (lid_off
 * or chosen.off). Its workspace goes as it does when an output is unplugged,
 * and so do the layer surfaces on it: with the output out of the layout its
 * wl_output is gone for clients too.
 */
static void output_leave(struct tw_output *output, bool *why) {
	struct tw_server *server = output->server;
	struct wlr_output *wlr_output = output->wlr_output;
	strip_output_leave(output);
	*why = true;
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
	/* Never no screen at all: outputs turned off with the monitors tool come
	 * back when they are all that is left. */
	struct tw_output *output;
	bool any = false;
	wl_list_for_each(output, &server->outputs, link) {
		any = any || !output->chosen.off;
	}
	wl_list_for_each(output, &server->outputs, link) {
		if (!any && output->chosen.off) {
			wlr_log(WLR_DEBUG, "output %s: back on, the last one left", output->wlr_output->name);
			output->chosen.off = false;
			if (!output->builtin) {
				output_enter(output); /* the lid decides about the laptop's own */
			}
		}
	}
	/* One turned off counts for nothing here: behind a closed lid, the
	 * laptop's own screen stays in use if all the others are off. */
	int panels = 0, others = 0;
	wl_list_for_each(output, &server->outputs, link) {
		if (output->chosen.off) {
			continue;
		}
		if (output->builtin) {
			panels++;
		} else {
			others++;
		}
	}
	int due = tw_lid_look(&server->lid, closed, panels, others);
	bool in_use = due & TW_LID_PANEL;
	wl_list_for_each(output, &server->outputs, link) {
		if (!output->builtin || output->chosen.off || output->lid_off != in_use) {
			continue;
		}
		wlr_log(WLR_DEBUG, "output %s: %s", output->wlr_output->name,
			in_use ? "back in use" : "out of use behind the closed lid");
		if (in_use) {
			output_enter(output);
		} else {
			output_leave(output, &output->lid_off);
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
	output->builtin = tw_connector_builtin(wlr_output->name);
	/* Without it the output is simply never capped. */
	output->frame_timer = wl_event_loop_add_timer(server->loop, handle_frame_timer, output);
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
	outputs_restore(server);
}

/*
 * Asks the client of every mapped surface under `node`, shown or hidden, to
 * draw again, by telling it again what it was last told. A client's picture
 * lost its texture with the old renderer, and the scene re-damages a buffer
 * it cannot draw on every frame: until the client commits a new one, its
 * output would repaint at full rate (a wallpaper never would by itself).
 * A subsurface is reached through its parent; `last` keeps one surface's
 * nodes, which sit together, from asking it more than once.
 */
static void surfaces_redraw(struct wlr_scene_node *node, struct wlr_surface **last) {
	if (node->type == WLR_SCENE_NODE_TREE) {
		struct wlr_scene_node *child;
		wl_list_for_each(child, &wlr_scene_tree_from_node(node)->children, link) {
			surfaces_redraw(child, last);
		}
		return;
	}
	if (node->type != WLR_SCENE_NODE_BUFFER) {
		return;
	}
	struct wlr_scene_surface *scene_surface =
		wlr_scene_surface_try_from_buffer(wlr_scene_buffer_from_node(node));
	if (scene_surface == NULL) {
		return;
	}
	struct wlr_surface *surface = wlr_surface_get_root_surface(scene_surface->surface);
	if (surface == *last || !surface->mapped) {
		return;
	}
	*last = surface;
	struct wlr_xdg_surface *xdg = wlr_xdg_surface_try_from_wlr_surface(surface);
	struct wlr_layer_surface_v1 *layer = wlr_layer_surface_v1_try_from_wlr_surface(surface);
	struct wlr_session_lock_surface_v1 *lock =
		wlr_session_lock_surface_v1_try_from_wlr_surface(surface);
	if (xdg != NULL) {
		wlr_xdg_surface_schedule_configure(xdg);
	} else if (layer != NULL) {
		/* The last size sent, acked or not, so as not to undo a newer one. */
		uint32_t width = layer->current.actual_width, height = layer->current.actual_height;
		if (!wl_list_empty(&layer->configure_list)) {
			struct wlr_layer_surface_v1_configure *configure =
				wl_container_of(layer->configure_list.prev, configure, link);
			width = configure->width;
			height = configure->height;
		}
		wlr_layer_surface_v1_configure(layer, width, height);
	} else if (lock != NULL) {
		uint32_t width = lock->current.width, height = lock->current.height;
		if (!wl_list_empty(&lock->configure_list)) {
			struct wlr_session_lock_surface_v1_configure *configure =
				wl_container_of(lock->configure_list.prev, configure, link);
			width = configure->width;
			height = configure->height;
		}
		wlr_session_lock_surface_v1_configure(lock, width, height);
	}
}

/*
 * The GPU was reset (a driver hang, a bad wake-up), and the renderer and all
 * it made are gone. A new renderer takes over, or every screen would stay
 * black until tw was started again. The scene drops the textures of the old
 * one by itself; clients' buffers are imported again when they are next
 * committed, which surfaces_redraw() asks for.
 */
/*
 * wlroots compiles its few shaders when it creates the renderer and never
 * again, but Mesa keeps its GLSL compiler's library of built-in functions
 * for as long as the process lives: 5.7 MB of tw's own memory. Releasing the
 * compiler frees it into the heap, and trimming the heap gives it back to
 * the system (trimming alone frees nothing worth having). Were a shader
 * compiled later, Mesa would build the library again.
 */
void renderer_release_compiler(struct wlr_renderer *renderer) {
	if (!wlr_renderer_is_gles2(renderer)) {
		return;
	}
	struct wlr_egl *egl = wlr_gles2_renderer_get_egl(renderer);
	EGLDisplay display = wlr_egl_get_display(egl);
	if (eglMakeCurrent(display, EGL_NO_SURFACE, EGL_NO_SURFACE, wlr_egl_get_context(egl))) {
		glReleaseShaderCompiler();
		eglMakeCurrent(display, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
	}
	malloc_trim(0);
}

static void renderer_replace(void *data) {
	struct tw_server *server = data;
	server->renderer_restart = NULL;
	struct wlr_renderer *renderer = wlr_renderer_autocreate(server->backend);
	struct wlr_allocator *allocator = renderer != NULL ?
		wlr_allocator_autocreate(server->backend, renderer) : NULL;
	if (allocator == NULL) {
		wlr_log(WLR_ERROR, "could not start the renderer again; restart tw");
		wlr_renderer_destroy(renderer);
		return;
	}
	renderer_release_compiler(renderer);
	struct wlr_renderer *old_renderer = server->renderer;
	struct wlr_allocator *old_allocator = server->allocator;
	server->renderer = renderer;
	server->allocator = allocator;
	wl_signal_add(&renderer->events.lost, &server->renderer_lost);
	wlr_compositor_set_renderer(server->compositor, renderer);
	/* Window capture draws with the renderer it was made with. */
	windows_capture_stop(server);
	bool all = true;
	struct tw_output *output;
	wl_list_for_each(output, &server->outputs, link) {
		if (!wlr_output_init_render(output->wlr_output, allocator, renderer)) {
			wlr_log(WLR_ERROR, "output %s: cannot draw on it again", output->wlr_output->name);
			all = false;
		} else if (output->wlr_output->enabled) {
			/* Nothing else asks for a frame: the scene took no damage.
			 * The new buffers have no age, so the frame is drawn whole. */
			wlr_output_schedule_frame(output->wlr_output);
		}
	}
	/* An output that could not switch still points at the old ones, so
	 * they are kept (until tw exits; freed they would crash tw) and it
	 * draws with a context the GPU reset lost. This cannot happen while
	 * the new pair has the old one's buffer capabilities, which the same
	 * backend gives it. */
	if (all) {
		wlr_allocator_destroy(old_allocator);
		wlr_renderer_destroy(old_renderer);
	}
	/* The pointer's image went with the old renderer, and wlr_cursor
	 * uploads it again only when it changes or an output joins the layout:
	 * joining them all anew puts it back. */
	wlr_cursor_attach_output_layout(server->cursor, server->output_layout);
	struct wlr_surface *last = NULL;
	surfaces_redraw(&server->scene->tree.node, &last);
}

/*
 * The old renderer says it is lost in the middle of drawing a frame, so the
 * new one takes over once that frame has been given up: freed under its own
 * signal, the old one would crash tw.
 */
static void handle_renderer_lost(struct wl_listener *listener, void *data) {
	struct tw_server *server = wl_container_of(listener, server, renderer_lost);
	wlr_log(WLR_INFO, "the GPU was reset; starting the renderer again");
	/* Every output drawing until then hears the same: once is enough. */
	wl_list_remove(&server->renderer_lost.link);
	wl_list_init(&server->renderer_lost.link);
	server->renderer_restart = wl_event_loop_add_idle(server->loop, renderer_replace, server);
	if (server->renderer_restart == NULL) {
		wlr_log(WLR_ERROR, "out of memory; restart tw");
	}
}

/* --- The monitors tool ------------------------------------------------------ */

/*
 * A program may turn outputs on and off and set their mode, scale and place
 * (wlr-output-management: tw-monitors, wlr-randr, kanshi). What it chooses
 * is kept with the output (output->chosen) until the output is unplugged, so
 * that tw's own commits (screen off and on, a mode for the battery, the lid)
 * keep it. It is not saved: the program saves what it wants kept, and tw
 * runs monitors_command, which puts it back, whenever the outputs change.
 */

/* Tells the programs listening how the outputs are now. */
static void outputs_publish(struct tw_server *server) {
	struct wlr_output_configuration_v1 *config = wlr_output_configuration_v1_create();
	if (config == NULL) {
		return;
	}
	struct tw_output *output;
	wl_list_for_each(output, &server->outputs, link) {
		struct wlr_output_configuration_head_v1 *head =
			wlr_output_configuration_head_v1_create(config, output->wlr_output);
		if (head == NULL) {
			wlr_output_configuration_v1_destroy(config);
			return;
		}
		/* On while in use, also while the screens are off for idleness. */
		head->state.enabled = output_in_use(output);
		struct wlr_box box;
		wlr_output_layout_get_box(server->output_layout, output->wlr_output, &box);
		head->state.x = box.x;
		head->state.y = box.y;
	}
	wlr_output_manager_v1_set_configuration(server->output_manager, config);
}

/*
 * Whether tw can take a configuration, and with `apply` takes it. At least
 * one output must stay on, out from behind a closed lid; only the modes an
 * output offers are taken; the transform and the variable refresh rate stay
 * tw's.
 */
static bool outputs_configure(struct tw_server *server,
		struct wlr_output_configuration_v1 *config, bool apply) {
	struct wlr_output_configuration_head_v1 *head;
	bool any = false;
	wl_list_for_each(head, &config->heads, link) {
		struct wlr_output *wlr_output = head->state.output;
		struct tw_output *output = wlr_output->data;
		if (output == NULL) {
			return false; /* not one tw uses: a headset */
		}
		if (!head->state.enabled) {
			continue;
		}
		any = any || !output->lid_off;
		if ((head->state.mode == NULL && !wl_list_empty(&wlr_output->modes)) ||
				head->state.scale <= 0) {
			return false;
		}
		struct wlr_output_state state;
		wlr_output_state_init(&state);
		wlr_output_state_set_enabled(&state, true);
		if (head->state.mode != NULL) {
			wlr_output_state_set_mode(&state, head->state.mode);
		}
		wlr_output_state_set_scale(&state, head->state.scale);
		bool fits = wlr_output_test_state(wlr_output, &state);
		wlr_output_state_finish(&state);
		if (!fits) {
			return false;
		}
	}
	if (!any || !apply) {
		return any;
	}

	/* Those going off first, so that a workspace on one finds an heir. */
	wl_list_for_each(head, &config->heads, link) {
		struct tw_output *output = head->state.output->data;
		if (!head->state.enabled && !output->chosen.off) {
			wlr_log(WLR_DEBUG, "output %s: turned off", head->state.output->name);
			if (output_in_use(output)) {
				output_leave(output, &output->chosen.off);
			} else {
				output->chosen.off = true;
			}
		}
	}
	bool done = true;
	wl_list_for_each(head, &config->heads, link) {
		struct tw_output *output = head->state.output->data;
		struct wlr_output *wlr_output = head->state.output;
		if (!head->state.enabled) {
			continue;
		}
		bool was_off = output->chosen.off;
		output->chosen.off = false;
		output->chosen.mode = head->state.mode;
		output->chosen.scale = head->state.scale;
		output->chosen.placed = true;
		output->chosen.x = head->state.x;
		output->chosen.y = head->state.y;
		if (output->lid_off) {
			continue; /* the lid decides when it comes back */
		}
		if (was_off) {
			output_enter(output);
		} else {
			if (!output_set_power(output, !server->blanked)) {
				done = false;
			}
			wlr_output_layout_add(server->output_layout, wlr_output, head->state.x, head->state.y);
		}
		/* A screen that is off for idleness takes its mode when it comes on. */
		done = done && (server->blanked || ((head->state.mode == NULL ||
			wlr_output->current_mode == head->state.mode) &&
			wlr_output->scale == head->state.scale));
	}
	outputs_lid(server, server->lid.closed);
	return done;
}

static void output_config_answer(struct tw_server *server,
		struct wlr_output_configuration_v1 *config, bool apply) {
	if (outputs_configure(server, config, apply)) {
		wlr_output_configuration_v1_send_succeeded(config);
	} else {
		wlr_output_configuration_v1_send_failed(config);
	}
	wlr_output_configuration_v1_destroy(config);
	if (apply) {
		outputs_publish(server);
	}
}

static void handle_output_apply(struct wl_listener *listener, void *data) {
	struct tw_server *server = wl_container_of(listener, server, output_apply);
	output_config_answer(server, data, true);
}

static void handle_output_test(struct wl_listener *listener, void *data) {
	struct tw_server *server = wl_container_of(listener, server, output_test);
	output_config_answer(server, data, false);
}

static void monitors_restore(void *data) {
	struct tw_server *server = data;
	server->monitors_restore = NULL;
	spawn_installed(monitors_command);
}

void outputs_restore(struct tw_server *server) {
	if (server->started && server->monitors_restore == NULL && monitors_command[0] != '\0') {
		server->monitors_restore = wl_event_loop_add_idle(server->loop, monitors_restore, server);
	}
}

static void handle_layout_change(struct wl_listener *listener, void *data) {
	/* Layer surfaces first: they decide how much of the output is left. */
	struct tw_server *server = wl_container_of(listener, server, layout_change);
	layers_arrange(server);
	lock_arrange(server);
	strip_arrange(server);
	outputs_publish(server);
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

	server->output_manager = wlr_output_manager_v1_create(server->display);
	server->output_apply.notify = handle_output_apply;
	wl_signal_add(&server->output_manager->events.apply, &server->output_apply);
	server->output_test.notify = handle_output_test;
	wl_signal_add(&server->output_manager->events.test, &server->output_test);

	server->tearing_control = wlr_tearing_control_manager_v1_create(server->display, 1);
	server->content_type = wlr_content_type_manager_v1_create(server->display, 1);
}

void output_finish(struct tw_server *server) {
	wl_list_remove(&server->new_output.link);
	wl_list_remove(&server->layout_change.link);
	wl_list_remove(&server->renderer_lost.link);
	wl_list_remove(&server->output_apply.link);
	wl_list_remove(&server->output_test.link);
	if (server->monitors_restore != NULL) {
		wl_event_source_remove(server->monitors_restore);
		server->monitors_restore = NULL;
	}
	if (server->renderer_restart != NULL) {
		wl_event_source_remove(server->renderer_restart);
		server->renderer_restart = NULL;
	}
	/* The backend destroys the outputs only after the scene and the
	 * renderer are gone, so none of tw's handlers may run then: an output
	 * going would bring the laptop's own screen back, drawn with what was
	 * freed, and could lock the session on the way out. */
	struct tw_output *output, *tmp;
	wl_list_for_each_safe(output, tmp, &server->outputs, link) {
		output_free(output);
	}
}
