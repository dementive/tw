#include <getopt.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/wait.h>
#include <unistd.h>
#include <wlr/backend.h>
#include <wlr/render/allocator.h>
#include <wlr/render/wlr_renderer.h>
#include <wlr/types/wlr_compositor.h>
#include <wlr/types/wlr_data_device.h>
#include <wlr/types/wlr_fractional_scale_v1.h>
#include <wlr/types/wlr_keyboard.h>
#include <wlr/types/wlr_scene.h>
#include <wlr/types/wlr_subcompositor.h>
#include <wlr/types/wlr_viewporter.h>
#include <wlr/util/log.h>

#include "tw.h"

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
	printf("Usage: %s [-d] [-s startup-command]\n"
		"  -d  log debug output\n"
		"  -s  run a shell command once the compositor is up\n", name);
}

int main(int argc, char *argv[]) {
	const char *startup_cmd = NULL;
	enum wlr_log_importance verbosity = WLR_ERROR;

	int c;
	while ((c = getopt(argc, argv, "ds:h")) != -1) {
		switch (c) {
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
	wlr_renderer_init_wl_display(server.renderer, server.display);
	server.allocator = wlr_allocator_autocreate(server.backend, server.renderer);
	if (server.allocator == NULL) {
		wlr_log(WLR_ERROR, "failed to create the allocator");
		return 1;
	}

	/* Globals wlroots implements entirely on its own. */
	wlr_compositor_create(server.display, 6, server.renderer);
	wlr_subcompositor_create(server.display);
	wlr_data_device_manager_create(server.display);
	wlr_viewporter_create(server.display);
	wlr_fractional_scale_manager_v1_create(server.display, 1);

	output_init(&server);
	layer_init(&server);
	window_init(&server);
	input_init(&server);

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
	if (startup_cmd != NULL) {
		spawn(startup_cmd);
	}

	wlr_log(WLR_INFO, "running on WAYLAND_DISPLAY=%s", socket);
	wl_display_run(server.display);
	status = 0;

teardown:
	/* wlroots asserts that every listener was removed before the object it
	 * listens on is destroyed, so the order here matters. */
	wl_display_destroy_clients(server.display);
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
