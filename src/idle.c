/*
 * Idle handling: power the outputs off after a period without input, and let
 * clients (a video player) hold that off while they are on screen.
 *
 * Blanking is built in rather than left to a helper such as swayidle: no
 * extra process, and it can do what a helper cannot, which is tell every
 * client to stop drawing while nobody can see it.
 *
 * Cost: one timer. It is not re-armed on every input event; input only
 * records a timestamp, and when the timer fires it either blanks or sleeps
 * for the time that is left. That is at most one wakeup per timeout while the
 * machine is in use, and none once the outputs are off or while inhibited.
 */
#include <stdlib.h>
#include <wlr/types/wlr_idle_inhibit_v1.h>
#include <wlr/types/wlr_idle_notify_v1.h>
#include <wlr/types/wlr_output.h>
#include <wlr/util/log.h>

#include "tw.h"
#include "config.h"

struct tw_idle_inhibitor {
	struct tw_server *server;
	struct wlr_idle_inhibitor_v1 *inhibitor;
	struct wl_listener destroy;
};

static void blank_timer_arm(struct tw_server *server, int ms) {
	if (server->blank_after_ms > 0) {
		wl_event_source_timer_update(server->blank_timer, ms);
	}
}

static void set_blanked(struct tw_server *server, bool blanked) {
	if (server->blanked == blanked) {
		return;
	}
	server->blanked = blanked;

	struct tw_output *output;
	wl_list_for_each(output, &server->outputs, link) {
		struct wlr_output_state state;
		wlr_output_state_init(&state);
		wlr_output_state_set_enabled(&state, !blanked);
		if (!wlr_output_commit_state(output->wlr_output, &state)) {
			wlr_log(WLR_ERROR, "failed to power %s output %s",
				blanked ? "off" : "on", output->wlr_output->name);
		}
		wlr_output_state_finish(&state);
		if (!blanked) {
			wlr_output_schedule_frame(output->wlr_output);
		}
	}

	/* Suspends every window while the screen is off, resumes them after. */
	strip_arrange(server);
	if (!blanked) {
		blank_timer_arm(server, server->blank_after_ms);
	}
}

static int handle_blank_timer(void *data) {
	struct tw_server *server = data;
	/* Both of these re-arm the timer when they end. */
	if (server->blanked || server->idle_inhibited) {
		return 0;
	}
	int64_t idle_ms = now_ms() - server->last_activity_ms;
	if (idle_ms >= server->blank_after_ms) {
		/* Nobody is here: lock before the screen goes dark, so that whoever
		 * wakes it finds the lock screen. */
		if (lock_on_idle) {
			lock_now(server);
		}
		set_blanked(server, true);
	} else {
		blank_timer_arm(server, (int)(server->blank_after_ms - idle_ms));
	}
	return 0;
}

void idle_activity(struct tw_server *server) {
	server->last_activity_ms = now_ms();
	wlr_idle_notifier_v1_notify_activity(server->idle_notifier, server->seat);
	set_blanked(server, false);
}

void action_blank(struct tw_server *server, const union tw_arg *arg) {
	set_blanked(server, true);
}

/* `dying` is an inhibitor that is being destroyed but is still in the list. */
static void update_inhibited(struct tw_server *server, struct wlr_idle_inhibitor_v1 *dying) {
	bool inhibited = false;
	struct wlr_idle_inhibitor_v1 *inhibitor;
	wl_list_for_each(inhibitor, &server->idle_inhibit_manager->inhibitors, link) {
		/* Only what is on screen counts: a paused player on another
		 * workspace must not keep the display lit. */
		if (inhibitor != dying && surface_visible(inhibitor->surface)) {
			inhibited = true;
			break;
		}
	}
	if (inhibited == server->idle_inhibited) {
		return;
	}
	server->idle_inhibited = inhibited;
	wlr_idle_notifier_v1_set_inhibited(server->idle_notifier, inhibited);
	if (!inhibited && !server->blanked) {
		/* The idle clock starts over when the inhibitor goes away. */
		server->last_activity_ms = now_ms();
		blank_timer_arm(server, server->blank_after_ms);
	}
}

void idle_update_inhibited(struct tw_server *server) {
	update_inhibited(server, NULL);
}

static void handle_inhibitor_destroy(struct wl_listener *listener, void *data) {
	struct tw_idle_inhibitor *inhibitor = wl_container_of(listener, inhibitor, destroy);
	wl_list_remove(&inhibitor->destroy.link);
	update_inhibited(inhibitor->server, inhibitor->inhibitor);
	free(inhibitor);
}

static void handle_new_inhibitor(struct wl_listener *listener, void *data) {
	struct tw_server *server = wl_container_of(listener, server, new_idle_inhibitor);
	struct wlr_idle_inhibitor_v1 *wlr_inhibitor = data;

	/* Without the destroy listener we would not learn that the screen may
	 * blank again, so an inhibitor we cannot track is simply not honoured. */
	struct tw_idle_inhibitor *inhibitor = calloc(1, sizeof(*inhibitor));
	if (inhibitor == NULL) {
		return;
	}
	inhibitor->server = server;
	inhibitor->inhibitor = wlr_inhibitor;
	inhibitor->destroy.notify = handle_inhibitor_destroy;
	wl_signal_add(&wlr_inhibitor->events.destroy, &inhibitor->destroy);
	update_inhibited(server, NULL);
}

void idle_init(struct tw_server *server) {
	server->last_activity_ms = now_ms();
	/* For clients that track idleness themselves (swayidle, chat status). */
	server->idle_notifier = wlr_idle_notifier_v1_create(server->display);
	server->idle_inhibit_manager = wlr_idle_inhibit_v1_create(server->display);
	server->new_idle_inhibitor.notify = handle_new_inhibitor;
	wl_signal_add(&server->idle_inhibit_manager->events.new_inhibitor,
		&server->new_idle_inhibitor);

	server->blank_timer = wl_event_loop_add_timer(server->loop, handle_blank_timer, server);
	blank_timer_arm(server, server->blank_after_ms);
}

void idle_finish(struct tw_server *server) {
	wl_list_remove(&server->new_idle_inhibitor.link);
	wl_event_source_remove(server->blank_timer);
}
