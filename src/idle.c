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
 * while the machine is in use, and once the outputs are off none unless a
 * suspend is still to come. While inhibited the timer fires at most once
 * more and is not set again until the inhibitor lets go, and while the user
 * is on another VT it is not set at all.
 */
#include <errno.h>
#include <stdlib.h>
#include <sys/timerfd.h>
#include <unistd.h>
#include <wlr/backend/session.h>
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

/* The user is on another VT: tw gets no input and has no outputs, so it
 * cannot tell whether anybody is there. */
static bool away(const struct tw_server *server) {
	return server->session != NULL && !server->session->active;
}

/* Sets the timer for whatever is due next: blanking, or a suspend attempt.
 * Call when one of those has just happened, the screen came back on, an
 * inhibitor let go or the user came back to tw's VT; never per input event
 * (see the top of this file). */
static void idle_timer_start(struct tw_server *server) {
	int64_t next = 0; /* in milliseconds of idleness; 0 if nothing is due */
	if (away(server)) {
		/* Nothing is due: coming back is activity. */
		wl_event_source_timer_update(server->idle_timer, 0);
		return;
	}
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

/* The idle clock starts over: the user did something, or something that
 * held it off let go. */
static void idle_clock_restart(struct tw_server *server) {
	server->last_activity_ms = now_ms();
	server->suspend_tries = 0;
}

/*
 * Turns every output in use off, or on those that are off. An output that
 * refuses to come on (a GPU still waking, a monitor still training its link)
 * is tried again on later input; see idle_activity().
 */
static void outputs_power(struct tw_server *server, bool on) {
	struct tw_output *output;
	wl_list_for_each(output, &server->outputs, link) {
		/* The laptop's own screen behind a closed lid stays off. */
		if (output->lid_off || (on && output->wlr_output->enabled)) {
			continue;
		}
		if (!output_set_power(output, on)) {
			wlr_log(WLR_ERROR, "failed to power %s output %s",
				on ? "on" : "off", output->wlr_output->name);
		} else if (on) {
			wlr_output_schedule_frame(output->wlr_output);
		}
	}
}

static void set_blanked(struct tw_server *server, bool blanked) {
	if (server->blanked == blanked) {
		return;
	}
	server->blanked = blanked;
	outputs_power(server, !blanked);

	/* Suspends every window while the screen is off, resumes them after. */
	strip_arrange(server);
	if (!blanked) {
		idle_timer_start(server);
	}
}

static int handle_idle_timer(void *data) {
	struct tw_server *server = data;
	/* The timer is started again when the inhibitor goes away, or when the
	 * user comes back to this VT. */
	if (server->idle_inhibited || away(server)) {
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
		wlr_log(WLR_INFO, "no input for %d seconds, suspending", (int)(idle_ms / 1000));
		suspend_now(server);
	}
	idle_timer_start(server);
	return 0;
}

void suspend_now(struct tw_server *server) {
	if (!program_installed(suspend_command)) {
		wlr_log(WLR_INFO, "not installed: %s", suspend_command);
		return;
	}
	/* Locked first, so that whoever wakes the machine finds the lock screen. */
	if (lock_on_idle) {
		lock_now(server);
	}
	spawn(suspend_command);
}

void idle_activity(struct tw_server *server) {
	idle_clock_restart(server);
	wlr_idle_notifier_v1_notify_activity(server->idle_notifier, server->seat);
	if (server->blanked) {
		set_blanked(server, false);
	} else if (server->last_activity_ms - server->lit_check_ms >= 1000) {
		/* An output that should be on and is not (it refused to come on, or
		 * to be set up) is tried again, at most once a second: one that
		 * goes on refusing must not fill the log at the pointer's rate. */
		server->lit_check_ms = server->last_activity_ms;
		outputs_power(server, true);
	}
}

/* The user switched to another VT, or back. wlroots has taken the outputs
 * away on leaving and brings them back on return. */
static void handle_session_active(struct wl_listener *listener, void *data) {
	struct tw_server *server = wl_container_of(listener, server, session_active);
	if (!away(server)) {
		/* Coming back is activity: the outputs come on, if they were
		 * off, and the idle clock starts over. */
		idle_activity(server);
	}
	idle_timer_start(server); /* stops it while away */
}

/*
 * Waking up. Nothing tells a Wayland compositor that the machine was
 * suspended, and the key or lid that woke it does not always arrive as input.
 * What does happen is that the wall clock jumps, and the kernel cancels any
 * timer that asked to be told about that. The timer itself never expires, so
 * this costs nothing until it happens.
 *
 * The wall clock also jumps when it is set, as the network time service does
 * when the network comes back. That is told apart by the time spent asleep,
 * which grows only when the machine has slept.
 */
static void resume_watch_arm(int fd) {
	struct itimerspec never = {.it_value.tv_sec = (time_t)1 << 40};
	timerfd_settime(fd, TFD_TIMER_ABSTIME | TFD_TIMER_CANCEL_ON_SET, &never, NULL);
}

static int handle_clock_jump(int fd, uint32_t mask, void *data) {
	struct tw_server *server = data;
	uint64_t expirations;
	if (read(fd, &expirations, sizeof(expirations)) >= 0 || errno != ECANCELED) {
		return 0;
	}
	resume_watch_arm(fd);
	int64_t asleep = asleep_ms();
	bool woke = asleep - server->asleep_ms > 500;
	server->asleep_ms = asleep;
	if (woke) {
		/* Back from suspend: turn the screen on and start the idle clock
		 * over. */
		wlr_log(WLR_DEBUG, "woke up");
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
		if (inhibitor != dying && surface_visible(server, inhibitor->surface)) {
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
			idle_clock_restart(server);
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

	if (server->suspend_after_ms > 0 && !program_installed(suspend_command)) {
		wlr_log(WLR_INFO, "not suspending when idle: not installed: %s", suspend_command);
		server->suspend_after_ms = 0;
	}
	server->idle_timer = wl_event_loop_add_timer(server->loop, handle_idle_timer, server);
	idle_timer_start(server);
	if (server->session != NULL) {
		server->session_active.notify = handle_session_active;
		wl_signal_add(&server->session->events.active, &server->session_active);
	}

	server->asleep_ms = asleep_ms();
	server->resume_fd = timerfd_create(CLOCK_REALTIME, TFD_CLOEXEC | TFD_NONBLOCK);
	if (server->resume_fd >= 0) {
		resume_watch_arm(server->resume_fd);
		server->resume_watch = wl_event_loop_add_fd(server->loop, server->resume_fd,
			WL_EVENT_READABLE, handle_clock_jump, server);
	}
}

void idle_finish(struct tw_server *server) {
	wl_list_remove(&server->new_idle_inhibitor.link);
	if (server->session != NULL) {
		wl_list_remove(&server->session_active.link);
	}
	wl_event_source_remove(server->idle_timer);
	if (server->resume_watch != NULL) {
		wl_event_source_remove(server->resume_watch);
	}
	if (server->resume_fd >= 0) {
		close(server->resume_fd);
	}
}
