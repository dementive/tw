/*
 * Idle handling: power the outputs off after a period without input, suspend
 * the machine after a longer one, and let clients (a video player) hold both
 * off while they are on screen.
 *
 * This is built in rather than left to a helper such as swayidle: no extra
 * process, and it can do what a helper cannot, which is tell every client to
 * stop drawing while nobody can see it.
 *
 * Cost: one timer. It is not re-armed on every input event; input only
 * records a timestamp, and when the timer fires it does what is due and
 * sleeps until the next thing is. That is at most one wakeup per timeout
 * while the machine is in use, and none while inhibited or, unless a suspend
 * is still to come, once the outputs are off.
 */
#include <errno.h>
#include <stdlib.h>
#include <sys/timerfd.h>
#include <unistd.h>
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

/* Sets the timer for whatever is due next: blanking, or a suspend attempt.
 * Call when the idle clock restarts or one of those has just happened. */
static void idle_timer_start(struct tw_server *server) {
	int64_t next = 0; /* in milliseconds of idleness; 0 if nothing is due */
	if (!server->blanked && server->blank_after_ms > 0) {
		next = server->blank_after_ms;
	}
	if (server->suspend_after_ms > 0) {
		int64_t due = (int64_t)server->suspend_after_ms * (server->suspend_tries + 1);
		if (next == 0 || due < next) {
			next = due;
		}
	}
	int64_t wait = next - (now_ms() - server->last_activity_ms);
	wl_event_source_timer_update(server->idle_timer,
		next == 0 ? 0 : (wait > 0 ? (int)wait : 1));
}

static void set_blanked(struct tw_server *server, bool blanked) {
	if (server->blanked == blanked) {
		return;
	}
	server->blanked = blanked;

	struct tw_output *output;
	wl_list_for_each(output, &server->outputs, link) {
		if (!output_set_power(output, !blanked)) {
			wlr_log(WLR_ERROR, "failed to power %s output %s",
				blanked ? "off" : "on", output->wlr_output->name);
		}
		if (!blanked) {
			wlr_output_schedule_frame(output->wlr_output);
		}
	}

	/* Suspends every window while the screen is off, resumes them after. */
	strip_arrange(server);
	if (!blanked) {
		idle_timer_start(server);
	}
}

static int handle_idle_timer(void *data) {
	struct tw_server *server = data;
	/* The timer is started again when the inhibitor goes away. */
	if (server->idle_inhibited) {
		return 0;
	}
	int64_t idle_ms = now_ms() - server->last_activity_ms;
	if (!server->blanked && server->blank_after_ms > 0 && idle_ms >= server->blank_after_ms) {
		/* Nobody is here: lock before the screen goes dark, so that whoever
		 * wakes it finds the lock screen. */
		if (lock_on_idle) {
			lock_now(server);
		}
		set_blanked(server, true);
	}
	if (server->suspend_after_ms > 0 &&
			idle_ms >= (int64_t)server->suspend_after_ms * (server->suspend_tries + 1)) {
		/* If the machine is still awake after this (something holds a sleep
		 * inhibitor), the next attempt is one more period away. */
		server->suspend_tries++;
		if (lock_on_idle) {
			lock_now(server);
		}
		wlr_log(WLR_INFO, "no input for %d seconds, suspending", (int)(idle_ms / 1000));
		spawn(suspend_command);
	}
	idle_timer_start(server);
	return 0;
}

void idle_activity(struct tw_server *server) {
	server->last_activity_ms = now_ms();
	server->suspend_tries = 0;
	wlr_idle_notifier_v1_notify_activity(server->idle_notifier, server->seat);
	set_blanked(server, false);
}

/*
 * Waking up. Nothing tells a Wayland compositor that the machine was
 * suspended, and the key or lid that woke it does not always arrive as input.
 * What does happen is that the wall clock jumps, and the kernel cancels any
 * timer that asked to be told about that. The timer itself never expires, so
 * this costs nothing until it happens.
 */
static void resume_watch_arm(int fd) {
	struct itimerspec never = {.it_value.tv_sec = (time_t)1 << 40};
	timerfd_settime(fd, TFD_TIMER_ABSTIME | TFD_TIMER_CANCEL_ON_SET, &never, NULL);
}

static int handle_clock_jump(int fd, uint32_t mask, void *data) {
	struct tw_server *server = data;
	uint64_t expirations;
	if (read(fd, &expirations, sizeof(expirations)) < 0 && errno == ECANCELED) {
		resume_watch_arm(fd);
		/* Back from suspend (or the clock was set, which is rare and
		 * harmless here): turn the screen on and start the idle clock over. */
		wlr_log(WLR_DEBUG, "the clock jumped, treating it as a wake-up");
		idle_activity(server);
	}
	return 0;
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
	if (!inhibited) {
		/* The idle clock starts over when the inhibitor goes away, unless
		 * that was because the screen went off. */
		if (!server->blanked) {
			server->last_activity_ms = now_ms();
		}
		idle_timer_start(server);
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

	char path[4096];
	if (server->suspend_after_ms > 0 && !program_find(suspend_command, path, sizeof(path))) {
		wlr_log(WLR_INFO, "not suspending when idle: cannot find %s", suspend_command);
		server->suspend_after_ms = 0;
	}
	server->idle_timer = wl_event_loop_add_timer(server->loop, handle_idle_timer, server);
	idle_timer_start(server);

	server->resume_fd = timerfd_create(CLOCK_REALTIME, TFD_CLOEXEC | TFD_NONBLOCK);
	if (server->resume_fd >= 0) {
		resume_watch_arm(server->resume_fd);
		server->resume_watch = wl_event_loop_add_fd(server->loop, server->resume_fd,
			WL_EVENT_READABLE, handle_clock_jump, server);
	}
}

void idle_finish(struct tw_server *server) {
	wl_list_remove(&server->new_idle_inhibitor.link);
	wl_event_source_remove(server->idle_timer);
	if (server->resume_watch != NULL) {
		wl_event_source_remove(server->resume_watch);
	}
	if (server->resume_fd >= 0) {
		close(server->resume_fd);
	}
}
