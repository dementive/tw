/*
 * Minimal Wayland client for the headless tests. It reports what the
 * compositor tells it, one line per event:
 *
 *   configure <w>x<h> activated=<0|1> suspended=<0|1> fullscreen=<0|1>
 *   layer configure <w>x<h>                             with --layer
 *   lock configure <w>x<h>, locked, lock denied         with --lock
 *   keyboard enter | keyboard leave                     keyboard focus
 *   key <code> <pressed|released>                       keys it received
 *   closed                                              asked to close; it exits
 *
 * Modes:
 *   (none)        one ordinary window
 *   --fullscreen  a window that asks to start fullscreen
 *   --fixed       a 300x200 window that cannot be resized, like a dialog
 *   --inhibit     a window that asks to keep the screen on, like a video player
 *   --layer       a 400x200 layer surface on the overlay layer wanting
 *                 exclusive keyboard focus, like a launcher
 *   --lock        a screen locker; pressing Return unlocks ("unlocked")
 *
 * It draws once per configure and never asks for frame callbacks, so an idle
 * compositor with these clients mapped should not wake up at all.
 */
#define _GNU_SOURCE
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>
#include <wayland-client.h>

#include "ext-session-lock-v1-client-protocol.h"
#include "idle-inhibit-unstable-v1-client-protocol.h"
#include "wlr-layer-shell-unstable-v1-client-protocol.h"
#include "xdg-shell-client-protocol.h"

#define KEY_RETURN 28 /* evdev code of the Return key */

static struct wl_display *display;
static struct wl_compositor *compositor;
static struct wl_shm *shm;
static struct wl_seat *seat;
static struct wl_output *output;
static struct wl_keyboard *keyboard;
static struct xdg_wm_base *wm_base;
static struct zwlr_layer_shell_v1 *layer_shell;
static struct ext_session_lock_manager_v1 *lock_manager;
static struct zwp_idle_inhibit_manager_v1 *inhibit_manager;
static struct ext_session_lock_v1 *session_lock;
static struct wl_surface *surface;
static struct wl_buffer *buffer;

static int32_t pending_width, pending_height;
static bool pending_activated, pending_suspended, pending_fullscreen;
static bool fixed_size;
static bool running = true;

static void say(const char *line) {
	puts(line);
	fflush(stdout);
}

static void draw(int32_t width, int32_t height) {
	int32_t stride = width * 4;
	size_t size = (size_t)stride * (size_t)height;

	int fd = memfd_create("tw-test-client", MFD_CLOEXEC);
	if (fd < 0 || ftruncate(fd, (off_t)size) < 0) {
		perror("shm buffer");
		exit(1);
	}
	uint32_t *pixels = mmap(NULL, size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
	if (pixels == MAP_FAILED) {
		perror("mmap");
		exit(1);
	}
	for (size_t i = 0; i < size / 4; i++) {
		pixels[i] = 0xff336699;
	}
	munmap(pixels, size);

	struct wl_shm_pool *pool = wl_shm_create_pool(shm, fd, (int32_t)size);
	struct wl_buffer *old = buffer;
	buffer = wl_shm_pool_create_buffer(pool, 0, width, height, stride, WL_SHM_FORMAT_XRGB8888);
	wl_shm_pool_destroy(pool);
	close(fd);

	wl_surface_attach(surface, buffer, 0, 0);
	wl_surface_damage_buffer(surface, 0, 0, width, height);
	wl_surface_commit(surface);
	if (old != NULL) {
		wl_buffer_destroy(old);
	}
}

/* --- Window ----------------------------------------------------------------- */

static void xdg_surface_configure(void *data, struct xdg_surface *xdg_surface, uint32_t serial) {
	xdg_surface_ack_configure(xdg_surface, serial);
	printf("configure %dx%d activated=%d suspended=%d fullscreen=%d\n", pending_width,
		pending_height, pending_activated, pending_suspended, pending_fullscreen);
	fflush(stdout);
	/* 0x0 means "pick your own size". */
	int32_t own_width = fixed_size ? 300 : 640, own_height = fixed_size ? 200 : 480;
	draw(pending_width > 0 ? pending_width : own_width,
		pending_height > 0 ? pending_height : own_height);
}

static const struct xdg_surface_listener xdg_surface_listener = {
	.configure = xdg_surface_configure,
};

static void toplevel_configure(void *data, struct xdg_toplevel *toplevel,
		int32_t width, int32_t height, struct wl_array *states) {
	pending_width = width;
	pending_height = height;
	pending_activated = pending_suspended = pending_fullscreen = false;
	uint32_t *state;
	wl_array_for_each(state, states) {
		if (*state == XDG_TOPLEVEL_STATE_ACTIVATED) {
			pending_activated = true;
		} else if (*state == XDG_TOPLEVEL_STATE_SUSPENDED) {
			pending_suspended = true;
		} else if (*state == XDG_TOPLEVEL_STATE_FULLSCREEN) {
			pending_fullscreen = true;
		}
	}
}

static void toplevel_close(void *data, struct xdg_toplevel *toplevel) {
	say("closed");
	running = false;
}

static void toplevel_configure_bounds(void *data, struct xdg_toplevel *toplevel,
		int32_t width, int32_t height) {
}

static void toplevel_wm_capabilities(void *data, struct xdg_toplevel *toplevel,
		struct wl_array *capabilities) {
}

static const struct xdg_toplevel_listener toplevel_listener = {
	.configure = toplevel_configure,
	.close = toplevel_close,
	.configure_bounds = toplevel_configure_bounds,
	.wm_capabilities = toplevel_wm_capabilities,
};

static void wm_base_ping(void *data, struct xdg_wm_base *base, uint32_t serial) {
	xdg_wm_base_pong(base, serial);
}

static const struct xdg_wm_base_listener wm_base_listener = {
	.ping = wm_base_ping,
};

/* --- Layer surface ---------------------------------------------------------- */

static void layer_configure(void *data, struct zwlr_layer_surface_v1 *layer_surface,
		uint32_t serial, uint32_t width, uint32_t height) {
	zwlr_layer_surface_v1_ack_configure(layer_surface, serial);
	printf("layer configure %ux%u\n", width, height);
	fflush(stdout);
	draw((int32_t)width, (int32_t)height);
}

static void layer_closed(void *data, struct zwlr_layer_surface_v1 *layer_surface) {
	running = false;
}

static const struct zwlr_layer_surface_v1_listener layer_listener = {
	.configure = layer_configure,
	.closed = layer_closed,
};

/* --- Screen lock ------------------------------------------------------------ */

static void lock_locked(void *data, struct ext_session_lock_v1 *lock) {
	say("locked");
}

static void lock_finished(void *data, struct ext_session_lock_v1 *lock) {
	/* The compositor refused the lock, or gave up on it. */
	say("lock denied");
	running = false;
}

static const struct ext_session_lock_v1_listener lock_listener = {
	.locked = lock_locked,
	.finished = lock_finished,
};

static void lock_surface_configure(void *data, struct ext_session_lock_surface_v1 *lock_surface,
		uint32_t serial, uint32_t width, uint32_t height) {
	ext_session_lock_surface_v1_ack_configure(lock_surface, serial);
	printf("lock configure %ux%u\n", width, height);
	fflush(stdout);
	draw((int32_t)width, (int32_t)height);
}

static const struct ext_session_lock_surface_v1_listener lock_surface_listener = {
	.configure = lock_surface_configure,
};

/* --- Keyboard --------------------------------------------------------------- */

static void keyboard_keymap(void *data, struct wl_keyboard *wl_keyboard, uint32_t format,
		int32_t fd, uint32_t size) {
	close(fd);
}

static void keyboard_enter(void *data, struct wl_keyboard *wl_keyboard, uint32_t serial,
		struct wl_surface *wl_surface, struct wl_array *keys) {
	say("keyboard enter");
}

static void keyboard_leave(void *data, struct wl_keyboard *wl_keyboard, uint32_t serial,
		struct wl_surface *wl_surface) {
	say("keyboard leave");
}

static void keyboard_key(void *data, struct wl_keyboard *wl_keyboard, uint32_t serial,
		uint32_t time, uint32_t key, uint32_t state) {
	bool pressed = state == WL_KEYBOARD_KEY_STATE_PRESSED;
	printf("key %u %s\n", key, pressed ? "pressed" : "released");
	fflush(stdout);
	if (session_lock != NULL && key == KEY_RETURN && pressed) {
		/* "Password accepted". */
		ext_session_lock_v1_unlock_and_destroy(session_lock);
		session_lock = NULL;
		wl_display_roundtrip(display);
		say("unlocked");
		running = false;
	}
}

static void keyboard_modifiers(void *data, struct wl_keyboard *wl_keyboard, uint32_t serial,
		uint32_t depressed, uint32_t latched, uint32_t locked, uint32_t group) {
}

static void keyboard_repeat_info(void *data, struct wl_keyboard *wl_keyboard,
		int32_t rate, int32_t delay) {
}

static const struct wl_keyboard_listener keyboard_listener = {
	.keymap = keyboard_keymap,
	.enter = keyboard_enter,
	.leave = keyboard_leave,
	.key = keyboard_key,
	.modifiers = keyboard_modifiers,
	.repeat_info = keyboard_repeat_info,
};

static void seat_capabilities(void *data, struct wl_seat *wl_seat, uint32_t capabilities) {
	if ((capabilities & WL_SEAT_CAPABILITY_KEYBOARD) && keyboard == NULL) {
		keyboard = wl_seat_get_keyboard(wl_seat);
		wl_keyboard_add_listener(keyboard, &keyboard_listener, NULL);
	}
}

static void seat_name(void *data, struct wl_seat *wl_seat, const char *name) {
}

static const struct wl_seat_listener seat_listener = {
	.capabilities = seat_capabilities,
	.name = seat_name,
};

/* --- Setup ------------------------------------------------------------------ */

static uint32_t at_most(uint32_t version, uint32_t max) {
	return version < max ? version : max;
}

static void registry_global(void *data, struct wl_registry *registry, uint32_t name,
		const char *interface, uint32_t version) {
	if (strcmp(interface, wl_compositor_interface.name) == 0) {
		compositor = wl_registry_bind(registry, name, &wl_compositor_interface, 4);
	} else if (strcmp(interface, wl_shm_interface.name) == 0) {
		shm = wl_registry_bind(registry, name, &wl_shm_interface, 1);
	} else if (strcmp(interface, wl_seat_interface.name) == 0) {
		seat = wl_registry_bind(registry, name, &wl_seat_interface, at_most(version, 4));
		wl_seat_add_listener(seat, &seat_listener, NULL);
	} else if (strcmp(interface, wl_output_interface.name) == 0 && output == NULL) {
		output = wl_registry_bind(registry, name, &wl_output_interface, 1);
	} else if (strcmp(interface, xdg_wm_base_interface.name) == 0) {
		/* Version 6 is the first with the `suspended` state. */
		wm_base = wl_registry_bind(registry, name, &xdg_wm_base_interface, at_most(version, 6));
		xdg_wm_base_add_listener(wm_base, &wm_base_listener, NULL);
	} else if (strcmp(interface, zwlr_layer_shell_v1_interface.name) == 0) {
		/* Version 4 is the first with named keyboard interactivity modes. */
		layer_shell = wl_registry_bind(registry, name, &zwlr_layer_shell_v1_interface,
			at_most(version, 4));
	} else if (strcmp(interface, ext_session_lock_manager_v1_interface.name) == 0) {
		lock_manager = wl_registry_bind(registry, name,
			&ext_session_lock_manager_v1_interface, 1);
	} else if (strcmp(interface, zwp_idle_inhibit_manager_v1_interface.name) == 0) {
		inhibit_manager = wl_registry_bind(registry, name,
			&zwp_idle_inhibit_manager_v1_interface, 1);
	}
}

static void registry_global_remove(void *data, struct wl_registry *registry, uint32_t name) {
}

static const struct wl_registry_listener registry_listener = {
	.global = registry_global,
	.global_remove = registry_global_remove,
};

int main(int argc, char *argv[]) {
	const char *mode = argc > 1 ? argv[1] : "";

	display = wl_display_connect(NULL);
	if (display == NULL) {
		fprintf(stderr, "cannot connect to a Wayland display\n");
		return 1;
	}
	struct wl_registry *registry = wl_display_get_registry(display);
	wl_registry_add_listener(registry, &registry_listener, NULL);
	/* Twice: the second round delivers the seat's capabilities, so the
	 * keyboard is bound before the surface can be focused. */
	wl_display_roundtrip(display);
	wl_display_roundtrip(display);
	if (compositor == NULL || shm == NULL || wm_base == NULL || layer_shell == NULL ||
			lock_manager == NULL || inhibit_manager == NULL || output == NULL) {
		fprintf(stderr, "compositor is missing a required global\n");
		return 1;
	}

	surface = wl_compositor_create_surface(compositor);
	if (strcmp(mode, "--layer") == 0) {
		struct zwlr_layer_surface_v1 *layer_surface = zwlr_layer_shell_v1_get_layer_surface(
			layer_shell, surface, NULL, ZWLR_LAYER_SHELL_V1_LAYER_OVERLAY, "tw-test");
		zwlr_layer_surface_v1_add_listener(layer_surface, &layer_listener, NULL);
		zwlr_layer_surface_v1_set_size(layer_surface, 400, 200);
		zwlr_layer_surface_v1_set_keyboard_interactivity(layer_surface,
			ZWLR_LAYER_SURFACE_V1_KEYBOARD_INTERACTIVITY_EXCLUSIVE);
		wl_surface_commit(surface);
	} else if (strcmp(mode, "--lock") == 0) {
		/* A lock surface gets its role without an initial commit. */
		session_lock = ext_session_lock_manager_v1_lock(lock_manager);
		ext_session_lock_v1_add_listener(session_lock, &lock_listener, NULL);
		struct ext_session_lock_surface_v1 *lock_surface =
			ext_session_lock_v1_get_lock_surface(session_lock, surface, output);
		ext_session_lock_surface_v1_add_listener(lock_surface, &lock_surface_listener, NULL);
	} else {
		struct xdg_surface *xdg_surface = xdg_wm_base_get_xdg_surface(wm_base, surface);
		xdg_surface_add_listener(xdg_surface, &xdg_surface_listener, NULL);
		struct xdg_toplevel *toplevel = xdg_surface_get_toplevel(xdg_surface);
		xdg_toplevel_add_listener(toplevel, &toplevel_listener, NULL);
		xdg_toplevel_set_title(toplevel, "tw-test-client");
		if (strcmp(mode, "--fixed") == 0) {
			fixed_size = true;
			xdg_toplevel_set_min_size(toplevel, 300, 200);
			xdg_toplevel_set_max_size(toplevel, 300, 200);
		} else if (strcmp(mode, "--fullscreen") == 0) {
			xdg_toplevel_set_fullscreen(toplevel, NULL);
		} else if (strcmp(mode, "--inhibit") == 0) {
			zwp_idle_inhibit_manager_v1_create_inhibitor(inhibit_manager, surface);
		}
		wl_surface_commit(surface);
	}

	while (running && wl_display_dispatch(display) != -1) {
	}

	wl_display_disconnect(display);
	return 0;
}
