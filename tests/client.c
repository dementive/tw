/*
 * Minimal Wayland client for the headless tests. It reports what the
 * compositor tells it, one line per event:
 *
 *   configure <w>x<h> activated=<0|1> suspended=<0|1> fullscreen=<0|1>
 *   layer configure <w>x<h>                             with --layer
 *   lock configure <w>x<h>, locked, lock denied         with --lock
 *   keyboard enter | keyboard leave                     keyboard focus
 *   key <code> <pressed|released>                       keys it received
 *   pointer enter <x> <y> | pointer leave               pointer focus
 *   output enter <name> | output leave <name>           which monitor shows it
 *   pointer motion <x> <y>, button <code> <pressed|released>
 *   closed                                              asked to close; it exits
 *   token, activate, activate forged                    see the focus modes below
 *   x11 connected, x11 served, x11 closed               with --x11
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
 * Asking for the focus (xdg-activation):
 *   --token <file>     a window that, when it gets the keyboard, turns that
 *                      event into an activation token and writes it to <file>
 *   --activate <file>  a window that, the first time it loses the focus, asks
 *                      for it back with the token in <file>
 *   --activate-forged  the same, but with a token it made up without any
 *                      input event, which must not work
 *
 * Holding the pointer, as games and 3D editors do:
 *   --lock-pointer     a window that locks the pointer when it enters. It
 *                      then reports raw movement ("relative <dx> <dy>") and
 *                      "pointer locked" / "pointer unlocked". A key press
 *                      makes it let go, asking for the cursor to reappear at
 *                      20,30 in the window ("pointer released")
 *   --confine-pointer  a window that confines the pointer to its top left
 *                      100x100 pixels ("pointer confined" / "unconfined")
 *
 * Without a Wayland connection, for the X11 socket tests:
 *   --x11 <path|abstract> <:N>   connects to X11 display N the way an X11
 *                      client would, by socket file or by abstract name, and
 *                      reports when the server answers and when it hangs up
 *   --x11-serve <arguments>      stands in for xwayland-satellite: prints its
 *                      pid and arguments, then answers on the sockets it was
 *                      handed with -listenfd
 *
 * It draws once per configure and never asks for frame callbacks, so an idle
 * compositor with these clients mapped should not wake up at all.
 */
#define _GNU_SOURCE
#include <poll.h>
#include <signal.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/prctl.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <time.h>
#include <unistd.h>
#include <wayland-client.h>

#include "ext-session-lock-v1-client-protocol.h"
#include "idle-inhibit-unstable-v1-client-protocol.h"
#include "pointer-constraints-unstable-v1-client-protocol.h"
#include "relative-pointer-unstable-v1-client-protocol.h"
#include "wlr-layer-shell-unstable-v1-client-protocol.h"
#include "xdg-activation-v1-client-protocol.h"
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
static struct xdg_activation_v1 *activation;
static struct zwp_pointer_constraints_v1 *constraints;
static struct zwp_relative_pointer_manager_v1 *relative_manager;
static struct wl_pointer *pointer;
static struct zwp_locked_pointer_v1 *locked_pointer;
static struct ext_session_lock_v1 *session_lock;
static struct wl_surface *surface;
static struct wl_buffer *buffer;

/* Every monitor, to report by name which ones show the surface. */
static struct {
	struct wl_output *output;
	char name[32];
} outputs[4];
static size_t output_count;

static int32_t pending_width, pending_height;
static bool pending_activated, pending_suspended, pending_fullscreen;
static bool fixed_size;
static bool running = true;

/* Focus modes. */
static const char *token_path;
static bool issue_token, use_token, forge_token;
static bool token_requested, was_activated, activation_sent;

/* Pointer modes. */
static bool lock_pointer, confine_pointer, constraint_requested;

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

/* --- Asking for the focus --------------------------------------------------- */

static void token_done(void *data, struct xdg_activation_token_v1 *token, const char *string) {
	xdg_activation_token_v1_destroy(token);
	if (forge_token) {
		xdg_activation_v1_activate(activation, string, surface);
		say("activate forged");
		return;
	}
	/* Under another name first, so that a reader never finds half a token. */
	char partial[512];
	snprintf(partial, sizeof(partial), "%s.partial", token_path);
	FILE *file = fopen(partial, "w");
	if (file == NULL || fputs(string, file) < 0 || fclose(file) != 0 ||
			rename(partial, token_path) != 0) {
		perror("token file");
		exit(1);
	}
	say("token");
}

static const struct xdg_activation_token_v1_listener token_listener = {
	.done = token_done,
};

/* `serial` is the input event the token stands for; 0 asks for one without. */
static void request_token(uint32_t serial) {
	struct xdg_activation_token_v1 *token = xdg_activation_v1_get_activation_token(activation);
	xdg_activation_token_v1_add_listener(token, &token_listener, NULL);
	if (serial != 0) {
		xdg_activation_token_v1_set_serial(token, serial, seat);
		xdg_activation_token_v1_set_surface(token, surface);
	}
	xdg_activation_token_v1_commit(token);
}

static void ask_for_focus(void) {
	if (forge_token) {
		request_token(0);
		return;
	}
	char token[256] = "";
	FILE *file = fopen(token_path, "r");
	if (file == NULL || fgets(token, sizeof(token), file) == NULL) {
		perror("token file");
		exit(1);
	}
	fclose(file);
	xdg_activation_v1_activate(activation, token, surface);
	say("activate");
}

/* --- Window ----------------------------------------------------------------- */

static void xdg_surface_configure(void *data, struct xdg_surface *xdg_surface, uint32_t serial) {
	xdg_surface_ack_configure(xdg_surface, serial);
	printf("configure %dx%d activated=%d suspended=%d fullscreen=%d\n", pending_width,
		pending_height, pending_activated, pending_suspended, pending_fullscreen);
	fflush(stdout);
	if (pending_activated) {
		was_activated = true;
	} else if (was_activated && !activation_sent && (use_token || forge_token)) {
		/* Just lost the focus: ask for it back, once. */
		activation_sent = true;
		ask_for_focus();
	}
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
	if (issue_token && !token_requested) {
		token_requested = true;
		request_token(serial);
	}
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
	if (locked_pointer != NULL && pressed) {
		/* Let go of the pointer the way a game leaving mouse-look does. */
		zwp_locked_pointer_v1_set_cursor_position_hint(locked_pointer,
			wl_fixed_from_int(20), wl_fixed_from_int(30));
		wl_surface_commit(surface);
		zwp_locked_pointer_v1_destroy(locked_pointer);
		locked_pointer = NULL;
		say("pointer released");
	}
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

/* --- Pointer ---------------------------------------------------------------- */

static void locked_locked(void *data, struct zwp_locked_pointer_v1 *locked) {
	say("pointer locked");
}

static void locked_unlocked(void *data, struct zwp_locked_pointer_v1 *locked) {
	say("pointer unlocked");
}

static const struct zwp_locked_pointer_v1_listener locked_listener = {
	.locked = locked_locked,
	.unlocked = locked_unlocked,
};

static void confined_confined(void *data, struct zwp_confined_pointer_v1 *confined) {
	say("pointer confined");
}

static void confined_unconfined(void *data, struct zwp_confined_pointer_v1 *confined) {
	say("pointer unconfined");
}

static const struct zwp_confined_pointer_v1_listener confined_listener = {
	.confined = confined_confined,
	.unconfined = confined_unconfined,
};

static void relative_motion(void *data, struct zwp_relative_pointer_v1 *relative,
		uint32_t utime_hi, uint32_t utime_lo, wl_fixed_t dx, wl_fixed_t dy,
		wl_fixed_t dx_unaccel, wl_fixed_t dy_unaccel) {
	printf("relative %d %d\n", wl_fixed_to_int(dx), wl_fixed_to_int(dy));
	fflush(stdout);
}

static const struct zwp_relative_pointer_v1_listener relative_listener = {
	.relative_motion = relative_motion,
};

static void pointer_enter(void *data, struct wl_pointer *wl_pointer, uint32_t serial,
		struct wl_surface *wl_surface, wl_fixed_t x, wl_fixed_t y) {
	printf("pointer enter %d %d\n", wl_fixed_to_int(x), wl_fixed_to_int(y));
	fflush(stdout);
	if (constraint_requested || !(lock_pointer || confine_pointer)) {
		return;
	}
	constraint_requested = true;
	if (lock_pointer) {
		locked_pointer = zwp_pointer_constraints_v1_lock_pointer(constraints, surface,
			wl_pointer, NULL, ZWP_POINTER_CONSTRAINTS_V1_LIFETIME_PERSISTENT);
		zwp_locked_pointer_v1_add_listener(locked_pointer, &locked_listener, NULL);
		zwp_relative_pointer_v1_add_listener(
			zwp_relative_pointer_manager_v1_get_relative_pointer(relative_manager, wl_pointer),
			&relative_listener, NULL);
	} else {
		struct wl_region *region = wl_compositor_create_region(compositor);
		wl_region_add(region, 0, 0, 100, 100);
		zwp_confined_pointer_v1_add_listener(
			zwp_pointer_constraints_v1_confine_pointer(constraints, surface, wl_pointer,
				region, ZWP_POINTER_CONSTRAINTS_V1_LIFETIME_PERSISTENT),
			&confined_listener, NULL);
		wl_region_destroy(region);
	}
}

static void pointer_leave(void *data, struct wl_pointer *wl_pointer, uint32_t serial,
		struct wl_surface *wl_surface) {
	say("pointer leave");
}

static void pointer_motion(void *data, struct wl_pointer *wl_pointer, uint32_t time,
		wl_fixed_t x, wl_fixed_t y) {
	printf("pointer motion %d %d\n", wl_fixed_to_int(x), wl_fixed_to_int(y));
	fflush(stdout);
}

static void pointer_button(void *data, struct wl_pointer *wl_pointer, uint32_t serial,
		uint32_t time, uint32_t button, uint32_t state) {
	printf("button %u %s\n", button,
		state == WL_POINTER_BUTTON_STATE_PRESSED ? "pressed" : "released");
	fflush(stdout);
}

static void pointer_axis(void *data, struct wl_pointer *wl_pointer, uint32_t time,
		uint32_t axis, wl_fixed_t value) {
}

static const struct wl_pointer_listener pointer_listener = {
	.enter = pointer_enter,
	.leave = pointer_leave,
	.motion = pointer_motion,
	.button = pointer_button,
	.axis = pointer_axis,
};

static void seat_capabilities(void *data, struct wl_seat *wl_seat, uint32_t capabilities) {
	if ((capabilities & WL_SEAT_CAPABILITY_KEYBOARD) && keyboard == NULL) {
		keyboard = wl_seat_get_keyboard(wl_seat);
		wl_keyboard_add_listener(keyboard, &keyboard_listener, NULL);
	}
	if ((capabilities & WL_SEAT_CAPABILITY_POINTER) && pointer == NULL) {
		pointer = wl_seat_get_pointer(wl_seat);
		wl_pointer_add_listener(pointer, &pointer_listener, NULL);
	}
}

static void seat_name(void *data, struct wl_seat *wl_seat, const char *name) {
}

static const struct wl_seat_listener seat_listener = {
	.capabilities = seat_capabilities,
	.name = seat_name,
};

/* --- Monitors --------------------------------------------------------------- */

static void output_geometry(void *data, struct wl_output *wl_output, int32_t x, int32_t y,
		int32_t physical_width, int32_t physical_height, int32_t subpixel,
		const char *make, const char *model, int32_t transform) {
}

static void output_mode(void *data, struct wl_output *wl_output, uint32_t flags,
		int32_t width, int32_t height, int32_t refresh) {
}

static void output_done(void *data, struct wl_output *wl_output) {
}

static void output_scale(void *data, struct wl_output *wl_output, int32_t factor) {
}

static void output_name(void *data, struct wl_output *wl_output, const char *name) {
	char *slot = data;
	snprintf(slot, sizeof(outputs[0].name), "%s", name);
}

static void output_description(void *data, struct wl_output *wl_output, const char *text) {
}

static const struct wl_output_listener output_listener = {
	.geometry = output_geometry,
	.mode = output_mode,
	.done = output_done,
	.scale = output_scale,
	.name = output_name,
	.description = output_description,
};

static void report_output(const char *what, struct wl_output *wl_output) {
	for (size_t i = 0; i < output_count; i++) {
		if (outputs[i].output == wl_output) {
			printf("output %s %s\n", what, outputs[i].name);
			fflush(stdout);
		}
	}
}

static void surface_enter(void *data, struct wl_surface *wl_surface, struct wl_output *wl_output) {
	report_output("enter", wl_output);
}

static void surface_leave(void *data, struct wl_surface *wl_surface, struct wl_output *wl_output) {
	report_output("leave", wl_output);
}

static const struct wl_surface_listener surface_listener = {
	.enter = surface_enter,
	.leave = surface_leave,
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
	} else if (strcmp(interface, wl_output_interface.name) == 0 &&
			output_count < sizeof(outputs) / sizeof(outputs[0])) {
		/* Version 4 is the first that names the output. */
		struct wl_output *wl_output = wl_registry_bind(registry, name, &wl_output_interface,
			at_most(version, 4));
		wl_output_add_listener(wl_output, &output_listener, outputs[output_count].name);
		outputs[output_count++].output = wl_output;
		if (output == NULL) {
			output = wl_output;
		}
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
	} else if (strcmp(interface, xdg_activation_v1_interface.name) == 0) {
		activation = wl_registry_bind(registry, name, &xdg_activation_v1_interface, 1);
	} else if (strcmp(interface, zwp_pointer_constraints_v1_interface.name) == 0) {
		constraints = wl_registry_bind(registry, name, &zwp_pointer_constraints_v1_interface, 1);
	} else if (strcmp(interface, zwp_relative_pointer_manager_v1_interface.name) == 0) {
		relative_manager = wl_registry_bind(registry, name,
			&zwp_relative_pointer_manager_v1_interface, 1);
	}
}

static void registry_global_remove(void *data, struct wl_registry *registry, uint32_t name) {
}

static const struct wl_registry_listener registry_listener = {
	.global = registry_global,
	.global_remove = registry_global_remove,
};

/* --- X11 sockets ------------------------------------------------------------ */

static int x11_client(const char *kind, const char *display_name) {
	int number = atoi(display_name + 1);
	struct sockaddr_un addr = {.sun_family = AF_UNIX};
	socklen_t length = offsetof(struct sockaddr_un, sun_path);
	if (strcmp(kind, "abstract") == 0) {
		length += 1 + (socklen_t)snprintf(addr.sun_path + 1, sizeof(addr.sun_path) - 1,
			"/tmp/.X11-unix/X%d", number);
	} else {
		length += 1 + (socklen_t)snprintf(addr.sun_path, sizeof(addr.sun_path),
			"/tmp/.X11-unix/X%d", number);
	}
	int fd = socket(AF_UNIX, SOCK_STREAM, 0);
	if (fd < 0 || connect(fd, (struct sockaddr *)&addr, length) < 0) {
		say("x11 refused");
		return 0;
	}
	say("x11 connected");
	char byte;
	if (read(fd, &byte, 1) == 1) {
		say("x11 served");
		/* Stay connected, like an application, until the server goes away. */
		while (read(fd, &byte, 1) > 0) {
		}
	}
	say("x11 closed");
	return 0;
}

static int x11_serve(int argc, char *argv[]) {
	/* Never outlive the compositor that started us. */
	prctl(PR_SET_PDEATHSIG, SIGTERM);

	struct pollfd sockets[2];
	nfds_t count = 0;
	printf("serve pid=%d", (int)getpid());
	for (int i = 0; i < argc; i++) {
		printf(" %s", argv[i]);
		if (strcmp(argv[i], "-listenfd") == 0 && i + 1 < argc && count < 2) {
			sockets[count++] = (struct pollfd){.fd = atoi(argv[i + 1]), .events = POLLIN};
		}
	}
	say("");

	/* Xwayland takes a while to come up. A compositor that kept watching the
	 * sockets meanwhile would start a second helper for the same client. */
	nanosleep(&(struct timespec){.tv_nsec = 300000000}, NULL);

	while (poll(sockets, count, -1) > 0) {
		for (nfds_t i = 0; i < count; i++) {
			if (!(sockets[i].revents & POLLIN)) {
				continue;
			}
			/* Kept open: the client stays connected until we exit. */
			int client = accept(sockets[i].fd, NULL, NULL);
			if (client < 0 || write(client, "x", 1) != 1) {
				say("serve error");
				return 1;
			}
			say("accepted");
		}
	}
	return 1;
}

int main(int argc, char *argv[]) {
	const char *mode = argc > 1 ? argv[1] : "";

	if (strcmp(mode, "--x11") == 0 && argc == 4) {
		return x11_client(argv[2], argv[3]);
	}
	if (strcmp(mode, "--x11-serve") == 0) {
		return x11_serve(argc - 2, argv + 2);
	}
	if (argc > 2) {
		token_path = argv[2];
	}
	issue_token = strcmp(mode, "--token") == 0;
	use_token = strcmp(mode, "--activate") == 0;
	forge_token = strcmp(mode, "--activate-forged") == 0;
	lock_pointer = strcmp(mode, "--lock-pointer") == 0;
	confine_pointer = strcmp(mode, "--confine-pointer") == 0;

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
			lock_manager == NULL || inhibit_manager == NULL || activation == NULL ||
			constraints == NULL || relative_manager == NULL || output == NULL) {
		fprintf(stderr, "compositor is missing a required global\n");
		return 1;
	}

	surface = wl_compositor_create_surface(compositor);
	wl_surface_add_listener(surface, &surface_listener, NULL);
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
