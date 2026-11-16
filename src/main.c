#include <errno.h>
#include <getopt.h>
#include <limits.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>
#include <wlr/backend.h>
#include <wlr/backend/multi.h>
#include <wlr/backend/wayland.h>
#include <wlr/config.h>
#include <wlr/render/allocator.h>
#include <wlr/render/wlr_renderer.h>
#include <wlr/types/wlr_buffer.h>
#include <wlr/types/wlr_compositor.h>
#include <wlr/types/wlr_data_control_v1.h>
#include <wlr/types/wlr_data_device.h>
#include <wlr/types/wlr_ext_data_control_v1.h>
#include <wlr/types/wlr_ext_image_capture_source_v1.h>
#include <wlr/types/wlr_ext_image_copy_capture_v1.h>
#include <wlr/types/wlr_fractional_scale_v1.h>
#include <wlr/types/wlr_keyboard.h>
#include <wlr/types/wlr_linux_dmabuf_v1.h>
#include <wlr/types/wlr_presentation_time.h>
#include <wlr/types/wlr_scene.h>
#include <wlr/types/wlr_screencopy_v1.h>
#include <wlr/types/wlr_single_pixel_buffer_v1.h>
#include <wlr/types/wlr_subcompositor.h>
#include <wlr/types/wlr_viewporter.h>
#include <wlr/types/wlr_xdg_output_v1.h>
#include <wlr/util/log.h>

#include "tw.h"
#include "config.h"

int64_t now_ms(void) {
	struct timespec now;
	clock_gettime(CLOCK_MONOTONIC, &now);
	return (int64_t)now.tv_sec * 1000 + now.tv_nsec / 1000000;
}

int64_t asleep_ms(void) {
	/* CLOCK_BOOTTIME counts the time asleep, CLOCK_MONOTONIC does not. */
	struct timespec all, awake;
	clock_gettime(CLOCK_BOOTTIME, &all);
	clock_gettime(CLOCK_MONOTONIC, &awake);
	return (int64_t)(all.tv_sec - awake.tv_sec) * 1000 +
		(all.tv_nsec - awake.tv_nsec) / 1000000;
}

void child_reset_signals(void) {
	/* The event loop blocks SIGINT/SIGTERM for its signalfd, and a blocked
	 * signal mask survives exec. */
	sigset_t set;
	sigemptyset(&set);
	sigprocmask(SIG_SETMASK, &set, NULL);
	/* So does a signal that is ignored, see main(). */
	signal(SIGPIPE, SIG_DFL);
}

void spawn(const char *cmd) {
	pid_t pid = fork();
	if (pid < 0) {
		wlr_log_errno(WLR_ERROR, "fork failed");
		return;
	}
	if (pid == 0) {
		child_reset_signals();
		setsid();
		/* Fork twice so the command is reparented to init: no zombies to
		 * reap, so no SIGCHLD handler and no wakeups when children exit. */
		if (fork() == 0) {
			execl("/bin/sh", "sh", "-c", cmd, (char *)NULL);
			_exit(127);
		}
		_exit(0);
	}
	waitpid(pid, NULL, 0);
}

/* A file that can be run: a directory can be "executed" too. */
static bool executable(const char *path) {
	struct stat st;
	return access(path, X_OK) == 0 && stat(path, &st) == 0 && S_ISREG(st.st_mode);
}

bool program_find(const char *command, char *path, size_t size) {
	size_t length = strcspn(command, " \t");
	const char *dirs = getenv("PATH");
	if (length == 0 || length >= size) {
		return false;
	}
	if (memchr(command, '/', length) != NULL || dirs == NULL) {
		/* A path is used as it is. */
		memcpy(path, command, length);
		path[length] = '\0';
		return executable(path);
	}
	while (*dirs != '\0') {
		size_t dir_length = strcspn(dirs, ":");
		if (dir_length > 0 && dir_length + 1 + length < size) {
			snprintf(path, size, "%.*s/%.*s", (int)dir_length, dirs, (int)length, command);
			if (executable(path)) {
				return true;
			}
		}
		dirs += dir_length;
		if (*dirs == ':') {
			dirs++;
		}
	}
	return false;
}

bool program_installed(const char *command) {
	char path[PATH_MAX];
	return program_find(command, path, sizeof(path));
}

bool spawn_installed(const char *cmd) {
	if (cmd[0] == '\0') {
		return false;
	}
	if (!program_installed(cmd)) {
		wlr_log(WLR_INFO, "not installed: %s", cmd);
		return false;
	}
	spawn(cmd);
	return true;
}

void action_spawn(struct tw_server *server, const union tw_arg *arg) {
	spawn(arg->s);
}

void action_quit(struct tw_server *server, const union tw_arg *arg) {
	wl_display_terminate(server->display);
}

static int handle_signal(int signo, void *data) {
	struct tw_server *server = data;
	wl_display_terminate(server->display);
	return 0;
}

static void usage(const char *name) {
	printf("Usage: %s [-d] [-b seconds] [-z seconds] [-x seconds] [-s startup-command]\n"
		"  -b  turn the screen off after this long without input (0: never;\n"
		"      default %d)\n"
		"  -z  suspend the machine after this long without input (0: never;\n"
		"      default %d)\n"
		"  -x  stop the X11 server when no X11 program has been connected for\n"
		"      this long (0: never; default %d)\n"
		"  -d  log debug output\n"
		"  -s  run a shell command once the compositor is up\n",
		name, blank_after_seconds, suspend_after_seconds, x11_idle_seconds);
}

/* A number of seconds given on the command line: digits and nothing else. */
static bool parse_seconds(const char *text, int *seconds) {
	char *end;
	errno = 0;
	long value = strtol(text, &end, 10);
	if (end == text || *end != '\0' || errno != 0 || value < 0 || value > INT_MAX) {
		return false;
	}
	*seconds = (int)value;
	return true;
}

/* A timeout in seconds as the milliseconds a timer takes. 0 stays 0, which
 * is never; a day is the most, so that the result fits. */
static int seconds_to_ms(int seconds) {
	return seconds <= 0 ? 0 : (seconds > 86400 ? 86400 : seconds) * 1000;
}

static void backend_nested(struct wlr_backend *backend, void *data) {
	if (wlr_backend_is_wl(backend)) {
		*(bool *)data = true;
	}
}

int main(int argc, char *argv[]) {
	const char *startup_cmd = NULL;
	enum wlr_log_importance verbosity = WLR_ERROR;
	int blank_after = blank_after_seconds;
	int suspend_after = suspend_after_seconds;
	int x11_idle = x11_idle_seconds;

	int c;
	while ((c = getopt(argc, argv, "b:ds:x:z:h")) != -1) {
		int *seconds = NULL;
		switch (c) {
		case 'b':
			seconds = &blank_after;
			break;
		case 'x':
			seconds = &x11_idle;
			break;
		case 'z':
			seconds = &suspend_after;
			break;
		case 'd':
			verbosity = WLR_DEBUG;
			break;
		case 's':
			startup_cmd = optarg;
			break;
		default:
			usage(argv[0]);
			return c == 'h' ? 0 : 1;
		}
		if (seconds != NULL && !parse_seconds(optarg, seconds)) {
			fprintf(stderr, "%s: -%c takes a number of seconds, not '%s'\n", argv[0], c, optarg);
			return 1;
		}
	}
	if (optind < argc) {
		usage(argv[0]);
		return 1;
	}
	wlr_log_init(verbosity, NULL);

	struct tw_server server = {0};
	server.blank_after_ms = seconds_to_ms(blank_after);
	server.suspend_after_ms = seconds_to_ms(suspend_after);
	server.x11_idle_ms = seconds_to_ms(x11_idle);

	/* A program that pastes may close its end before tw has written all
	 * of a copy it keeps (clipboard.c), and writing to a closed pipe kills
	 * the writer unless this signal is ignored. */
	signal(SIGPIPE, SIG_IGN);

	int status = 1;
	server.display = wl_display_create();
	if (server.display == NULL) {
		wlr_log(WLR_ERROR, "failed to create the Wayland display");
		return 1;
	}
	server.loop = wl_display_get_event_loop(server.display);
	server.sigint = wl_event_loop_add_signal(server.loop, SIGINT, handle_signal, &server);
	server.sigterm = wl_event_loop_add_signal(server.loop, SIGTERM, handle_signal, &server);

	/* Picks DRM+libinput on a TTY, a window when nested, or whatever
	 * WLR_BACKENDS says (the tests use "headless"). */
	server.backend = wlr_backend_autocreate(server.loop, &server.session);
	if (server.backend == NULL) {
		wlr_log(WLR_ERROR, "failed to create the backend");
		goto teardown_base;
	}
	/* In a window inside another session the host compositor owns Super,
	 * so Mod is Alt there. Told from the backend, not from the environment,
	 * which a console login may have inherited a DISPLAY into. */
	bool nested = false;
	wlr_multi_for_each_backend(server.backend, backend_nested, &nested);
	server.mod = nested ? WLR_MODIFIER_ALT : WLR_MODIFIER_LOGO;
	server.renderer = wlr_renderer_autocreate(server.backend);
	if (server.renderer == NULL) {
		wlr_log(WLR_ERROR, "failed to create the renderer");
		goto teardown_base;
	}
	/* Shared-memory buffers always; GPU buffers when the renderer has a GPU.
	 * This is wlr_renderer_init_wl_display() spelled out, to keep the
	 * dmabuf global for the scene (see output_init()). */
	if (!wlr_renderer_init_wl_shm(server.renderer, server.display)) {
		wlr_log(WLR_ERROR, "failed to offer shared-memory buffers");
		goto teardown_base;
	}
	if (wlr_renderer_get_texture_formats(server.renderer, WLR_BUFFER_CAP_DMABUF) != NULL &&
			wlr_renderer_get_drm_fd(server.renderer) >= 0) {
		server.linux_dmabuf =
			wlr_linux_dmabuf_v1_create_with_renderer(server.display, 4, server.renderer);
	}
	server.allocator = wlr_allocator_autocreate(server.backend, server.renderer);
	if (server.allocator == NULL) {
		wlr_log(WLR_ERROR, "failed to create the allocator");
		goto teardown_base;
	}

	/*
	 * Globals wlroots implements entirely on its own. Each one is a few
	 * hundred bytes and does nothing until a client uses it.
	 */
	server.compositor = wlr_compositor_create(server.display, 6, server.renderer);
	wlr_subcompositor_create(server.display);
	wlr_data_device_manager_create(server.display);
	wlr_viewporter_create(server.display);
	wlr_fractional_scale_manager_v1_create(server.display, 1);
	wlr_single_pixel_buffer_manager_v1_create(server.display);
	wlr_presentation_create(server.display, server.backend, 2);
	/* Screenshots and screen recording (grim, wf-recorder, portals). */
	wlr_screencopy_manager_v1_create(server.display);
	wlr_ext_image_copy_capture_manager_v1_create(server.display, 1);
	wlr_ext_output_image_capture_source_manager_v1_create(server.display, 1);
	/* Clipboard tools and managers (wl-copy, cliphist). */
	wlr_data_control_manager_v1_create(server.display);
	wlr_ext_data_control_manager_v1_create(server.display, 1);

	output_init(&server);
	/* Logical output geometry, which screenshot tools and some toolkits use. */
	wlr_xdg_output_manager_v1_create(server.display, server.output_layout);
	layer_init(&server);
	window_init(&server);
	strip_init(&server);
	input_init(&server);
	clipboard_init(&server);
	idle_init(&server);
	lock_init(&server);
	power_init(&server);

	/* From here on listeners are attached, so every exit goes through the
	 * full teardown below. */
	const char *socket = wl_display_add_socket_auto(server.display);
	if (socket == NULL) {
		wlr_log(WLR_ERROR, "failed to open a Wayland socket (is XDG_RUNTIME_DIR set?)");
		goto teardown;
	}
	if (!wlr_backend_start(server.backend)) {
		wlr_log(WLR_ERROR, "failed to start the backend");
		goto teardown;
	}

	setenv("WAYLAND_DISPLAY", socket, true);
	x11_init(&server);
	if (startup_cmd != NULL) {
		spawn(startup_cmd);
	}
	for (size_t i = 0; autostart[i] != NULL; i++) {
		spawn_installed(autostart[i]);
	}

	wlr_log(WLR_INFO, "running on WAYLAND_DISPLAY=%s", socket);
	wl_display_run(server.display);
	status = 0;

teardown:
	/* wlroots asserts that every listener was removed before the object it
	 * listens on is destroyed, so the order here matters. */
	wl_display_destroy_clients(server.display);
	x11_finish(&server);
	power_finish(&server);
	lock_finish(&server);
	idle_finish(&server);
	clipboard_finish(&server);
	input_finish(&server);
	window_finish(&server);
	layer_finish(&server);
	output_finish(&server);
	wlr_scene_node_destroy(&server.scene->tree.node);

teardown_base:
	/* What exists before any listener does. A start that failed early has
	 * only some of it; wlroots takes NULL for all but the event sources. */
	if (server.sigint != NULL) {
		wl_event_source_remove(server.sigint);
	}
	if (server.sigterm != NULL) {
		wl_event_source_remove(server.sigterm);
	}
	wlr_allocator_destroy(server.allocator);
	wlr_renderer_destroy(server.renderer);
	wlr_backend_destroy(server.backend);
	wl_display_destroy(server.display);
	return status;
}
