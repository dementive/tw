#include <getopt.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>
#include <wlr/backend.h>
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

void spawn(const char *cmd) {
	pid_t pid = fork();
	if (pid < 0) {
		wlr_log_errno(WLR_ERROR, "fork failed");
		return;
	}
	if (pid == 0) {
		/* The event loop blocks SIGINT/SIGTERM for its signalfd, and a
		 * blocked signal mask survives exec. Children must start clean. */
		sigset_t set;
		sigemptyset(&set);
		sigprocmask(SIG_SETMASK, &set, NULL);
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
		return access(path, X_OK) == 0;
	}
	while (*dirs != '\0') {
		size_t dir_length = strcspn(dirs, ":");
		struct stat st;
		if (dir_length > 0 && dir_length + 1 + length < size) {
			snprintf(path, size, "%.*s/%.*s", (int)dir_length, dirs, (int)length, command);
			if (access(path, X_OK) == 0 && stat(path, &st) == 0 && S_ISREG(st.st_mode)) {
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
	printf("Usage: %s [-d] [-b seconds] [-z seconds] [-s startup-command]\n"
		"  -b  turn the screen off after this long without input (0: never;\n"
		"      default %d)\n"
		"  -z  suspend the machine after this long without input (0: never;\n"
		"      default %d)\n"
		"  -d  log debug output\n"
		"  -s  run a shell command once the compositor is up\n",
		name, blank_after_seconds, suspend_after_seconds);
}

int main(int argc, char *argv[]) {
	const char *startup_cmd = NULL;
	enum wlr_log_importance verbosity = WLR_ERROR;
	int blank_after = blank_after_seconds;
	int suspend_after = suspend_after_seconds;

	int c;
	while ((c = getopt(argc, argv, "b:ds:z:h")) != -1) {
		switch (c) {
		case 'b':
			blank_after = atoi(optarg);
			break;
		case 'z':
			suspend_after = atoi(optarg);
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
	}
	if (optind < argc) {
		usage(argv[0]);
		return 1;
	}
	wlr_log_init(verbosity, NULL);

	struct tw_server server = {0};
	/* Capped so the values fit a millisecond timer. */
	server.blank_after_ms = blank_after <= 0 ? 0 :
		(blank_after > 86400 ? 86400 : blank_after) * 1000;
	server.suspend_after_ms = suspend_after <= 0 ? 0 :
		(suspend_after > 86400 ? 86400 : suspend_after) * 1000;

	/* Inside another session the host compositor owns Super, so use Alt.
	 * Checked before we overwrite WAYLAND_DISPLAY with our own socket. */
	bool nested = getenv("WAYLAND_DISPLAY") != NULL || getenv("DISPLAY") != NULL;
	server.mod = nested ? WLR_MODIFIER_ALT : WLR_MODIFIER_LOGO;

	server.display = wl_display_create();
	server.loop = wl_display_get_event_loop(server.display);
	server.sigint = wl_event_loop_add_signal(server.loop, SIGINT, handle_signal, &server);
	server.sigterm = wl_event_loop_add_signal(server.loop, SIGTERM, handle_signal, &server);

	/* Picks DRM+libinput on a TTY, a window when nested, or whatever
	 * WLR_BACKENDS says (the tests use "headless"). */
	server.backend = wlr_backend_autocreate(server.loop, &server.session);
	if (server.backend == NULL) {
		wlr_log(WLR_ERROR, "failed to create the backend");
		return 1;
	}
	server.renderer = wlr_renderer_autocreate(server.backend);
	if (server.renderer == NULL) {
		wlr_log(WLR_ERROR, "failed to create the renderer");
		return 1;
	}
	/* Shared-memory buffers always; GPU buffers when the renderer has a GPU.
	 * This is wlr_renderer_init_wl_display() spelled out, to keep the
	 * dmabuf global for the scene (see output_init()). */
	wlr_renderer_init_wl_shm(server.renderer, server.display);
	if (wlr_renderer_get_texture_formats(server.renderer, WLR_BUFFER_CAP_DMABUF) != NULL &&
			wlr_renderer_get_drm_fd(server.renderer) >= 0) {
		server.linux_dmabuf =
			wlr_linux_dmabuf_v1_create_with_renderer(server.display, 4, server.renderer);
	}
	server.allocator = wlr_allocator_autocreate(server.backend, server.renderer);
	if (server.allocator == NULL) {
		wlr_log(WLR_ERROR, "failed to create the allocator");
		return 1;
	}

	/*
	 * Globals wlroots implements entirely on its own. Each one is a few
	 * hundred bytes and does nothing until a client uses it.
	 */
	wlr_compositor_create(server.display, 6, server.renderer);
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
	input_init(&server);
	idle_init(&server);
	lock_init(&server);

	/* From here on listeners are attached, so every exit goes through the
	 * full teardown below. */
	int status = 1;
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
		char path[4096];
		if (program_find(autostart[i], path, sizeof(path))) {
			spawn(autostart[i]);
		} else {
			wlr_log(WLR_INFO, "autostart: not installed: %s", autostart[i]);
		}
	}

	wlr_log(WLR_INFO, "running on WAYLAND_DISPLAY=%s", socket);
	wl_display_run(server.display);
	status = 0;

teardown:
	/* wlroots asserts that every listener was removed before the object it
	 * listens on is destroyed, so the order here matters. */
	wl_display_destroy_clients(server.display);
	x11_finish(&server);
	lock_finish(&server);
	idle_finish(&server);
	input_finish(&server);
	window_finish(&server);
	layer_finish(&server);
	output_finish(&server);
	wl_event_source_remove(server.sigint);
	wl_event_source_remove(server.sigterm);
	wlr_scene_node_destroy(&server.scene->tree.node);
	wlr_allocator_destroy(server.allocator);
	wlr_renderer_destroy(server.renderer);
	wlr_backend_destroy(server.backend);
	wl_display_destroy(server.display);
	return status;
}
