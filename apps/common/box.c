#define _GNU_SOURCE
#include <errno.h>
#include <linux/input-event-codes.h>
#include <poll.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/timerfd.h>
#include <unistd.h>
#include <wayland-client.h>

#include "box.h"
#include "cursor-shape-v1-client-protocol.h"
#include "fractional-scale-v1-client-protocol.h"
#include "viewporter-client-protocol.h"
#include "wlr-layer-shell-unstable-v1-client-protocol.h"
#include "xdg-activation-v1-client-protocol.h"

#define LENGTH(a) (sizeof(a) / sizeof((a)[0]))

static const struct box_app *app;
static bool verbose;

static void say(const char *format, ...) __attribute__((format(printf, 1, 2)));
static void say(const char *format, ...) {
	if (verbose) {
		va_list args;
		va_start(args, format);
		vfprintf(stderr, format, args);
		va_end(args);
		fputc('\n', stderr);
	}
}

static struct wl_display *display;
static struct wl_registry *registry;
static struct wl_compositor *compositor;
static struct wl_shm *shm;
static struct wl_seat *seat;
static struct wl_keyboard *keyboard;
static struct wl_pointer *pointer;
static struct zwlr_layer_shell_v1 *layer_shell;
static struct wp_viewporter *viewporter;
static struct wp_fractional_scale_manager_v1 *scale_manager;
static struct xdg_activation_v1 *activation;
static struct wp_cursor_shape_manager_v1 *cursor_shapes;
static struct wp_cursor_shape_device_v1 *cursor;

static struct wl_surface *surface;
static struct zwlr_layer_surface_v1 *layer_surface;
static struct wp_viewport *viewport;
static struct wp_fractional_scale_v1 *fractional;
static struct wl_buffer *buffer;
static bool configured, dirty, running;
static uint32_t scale = 120; /* of the screen the box is on, in 120ths */
static uint32_t last_serial; /* of the last key or click */
static int timer_fd = -1, repeat_fd = -1;

int box_px(int logical) {
	return (logical * (int)scale + 60) / 120;
}

void box_redraw(void) {
	dirty = true;
}

void box_quit(void) {
	running = false;
}

static void arm(int fd, int first_ms, int every_ms) {
	struct itimerspec spec = {
		.it_value = {first_ms / 1000, (long)(first_ms % 1000) * 1000000},
		.it_interval = {every_ms / 1000, (long)(every_ms % 1000) * 1000000},
	};
	if (fd >= 0) {
		timerfd_settime(fd, 0, &spec, NULL);
	}
}

void box_timer(int ms) {
	arm(timer_fd, ms, 0);
}

/* --- The picture ------------------------------------------------------------- */

static void paint(void) {
	dirty = false;
	if (!configured) {
		return;
	}
	font_open(app->font, verbose); /* once */
	struct picture picture;
	if (!picture_open(&picture, box_px(app->width), box_px(app->height))) {
		fprintf(stderr, "%s: no memory for the picture\n", program_invocation_short_name);
		running = false;
		return;
	}
	app->draw(&picture);
	struct wl_buffer *drawn = picture_close(&picture, shm);
	wl_surface_attach(surface, drawn, 0, 0);
	if (viewport != NULL) {
		wp_viewport_set_destination(viewport, app->width, app->height);
	}
	wl_surface_damage_buffer(surface, 0, 0, INT32_MAX, INT32_MAX);
	wl_surface_commit(surface);
	if (buffer != NULL) {
		wl_buffer_destroy(buffer);
	}
	buffer = drawn;
	say("draw %dx%d", box_px(app->width), box_px(app->height));
}

static void layer_configure(void *data, struct zwlr_layer_surface_v1 *layer, uint32_t serial,
		uint32_t width, uint32_t height) {
	zwlr_layer_surface_v1_ack_configure(layer, serial);
	if (!configured) {
		configured = true;
		dirty = true;
	}
}

static void box_close(void) {
	struct wl_proxy *proxies[] = {
		(struct wl_proxy *)buffer, (struct wl_proxy *)fractional, (struct wl_proxy *)viewport,
		(struct wl_proxy *)layer_surface, (struct wl_proxy *)surface,
	};
	for (size_t i = 0; i < LENGTH(proxies); i++) {
		if (proxies[i] != NULL) {
			wl_proxy_destroy(proxies[i]);
		}
	}
	buffer = NULL;
	fractional = NULL;
	viewport = NULL;
	layer_surface = NULL;
	surface = NULL;
	configured = false;
}

static void layer_closed(void *data, struct zwlr_layer_surface_v1 *layer) {
	/* The screen it was on went away. */
	if (app->closed != NULL) {
		app->closed();
	} else {
		running = false;
	}
}

static const struct zwlr_layer_surface_v1_listener layer_listener = {
	.configure = layer_configure,
	.closed = layer_closed,
};

static void scale_preferred(void *data, struct wp_fractional_scale_v1 *object, uint32_t value) {
	if (value != scale && value != 0) {
		scale = value;
		dirty = true;
	}
}

static const struct wp_fractional_scale_v1_listener scale_listener = {
	.preferred_scale = scale_preferred,
};

void box_open(void) {
	if (surface != NULL) {
		box_close();
	}
	surface = wl_compositor_create_surface(compositor);
	/* On the overlay layer, above a fullscreen window too, taking every key
	 * while it is open. No anchor: the compositor centres it, and no output
	 * named: on the screen in use. */
	layer_surface = zwlr_layer_shell_v1_get_layer_surface(layer_shell, surface, NULL,
		ZWLR_LAYER_SHELL_V1_LAYER_OVERLAY, app->name);
	zwlr_layer_surface_v1_add_listener(layer_surface, &layer_listener, NULL);
	zwlr_layer_surface_v1_set_size(layer_surface, (uint32_t)app->width, (uint32_t)app->height);
	zwlr_layer_surface_v1_set_keyboard_interactivity(layer_surface, 1);
	if (viewporter != NULL && scale_manager != NULL) {
		viewport = wp_viewporter_get_viewport(viewporter, surface);
		fractional = wp_fractional_scale_manager_v1_get_fractional_scale(scale_manager, surface);
		wp_fractional_scale_v1_add_listener(fractional, &scale_listener, NULL);
	}
	wl_surface_commit(surface);
}

/* --- Keys -------------------------------------------------------------------- */

static struct xkb_context *xkb;
static struct xkb_keymap *keymap;
static struct xkb_state *xkb_state;
static int repeat_rate = 25, repeat_delay = 600;
static uint32_t repeat_key;

static void key_handle(uint32_t code) {
	char text[16];
	int length = xkb_state_key_get_utf8(xkb_state, code, text, sizeof(text));
	if (length <= 0 || length >= (int)sizeof(text) || (unsigned char)text[0] < 0x20 ||
			text[0] == 0x7f) {
		text[0] = '\0';
	}
	bool ctrl = xkb_state_mod_name_is_active(xkb_state, XKB_MOD_NAME_CTRL,
		XKB_STATE_MODS_EFFECTIVE) > 0;
	bool alt = xkb_state_mod_name_is_active(xkb_state, XKB_MOD_NAME_ALT,
		XKB_STATE_MODS_EFFECTIVE) > 0;
	app->key(xkb_state_key_get_one_sym(xkb_state, code), text, ctrl, alt);
}

static void keyboard_keymap(void *data, struct wl_keyboard *wl_keyboard, uint32_t format,
		int32_t fd, uint32_t size) {
	char *text = format == WL_KEYBOARD_KEYMAP_FORMAT_XKB_V1 ?
		mmap(NULL, size, PROT_READ, MAP_PRIVATE, fd, 0) : MAP_FAILED;
	close(fd);
	if (text == MAP_FAILED) {
		return;
	}
	struct xkb_keymap *made = xkb_keymap_new_from_string(xkb, text,
		XKB_KEYMAP_FORMAT_TEXT_V1, XKB_KEYMAP_COMPILE_NO_FLAGS);
	munmap(text, size);
	if (made == NULL) {
		return;
	}
	xkb_state_unref(xkb_state);
	xkb_keymap_unref(keymap);
	keymap = made;
	xkb_state = xkb_state_new(keymap);
}

static void keyboard_enter(void *data, struct wl_keyboard *wl_keyboard, uint32_t serial,
		struct wl_surface *entered, struct wl_array *keys) {
	last_serial = serial;
	say("keyboard");
}

static void keyboard_leave(void *data, struct wl_keyboard *wl_keyboard, uint32_t serial,
		struct wl_surface *left) {
	repeat_key = 0;
	arm(repeat_fd, 0, 0);
}

static void keyboard_key(void *data, struct wl_keyboard *wl_keyboard, uint32_t serial,
		uint32_t time, uint32_t key, uint32_t state) {
	if (xkb_state == NULL) {
		return;
	}
	uint32_t code = key + 8; /* evdev to XKB */
	if (state == WL_KEYBOARD_KEY_STATE_PRESSED) {
		last_serial = serial;
		key_handle(code);
		/* Held, it repeats by the compositor's settings, until let go. */
		bool repeats = repeat_rate > 0 && xkb_keymap_key_repeats(keymap, code);
		repeat_key = repeats ? code : 0;
		arm(repeat_fd, repeats ? (repeat_delay > 0 ? repeat_delay : 1) : 0,
			repeats ? 1000 / repeat_rate : 0);
	} else if (code == repeat_key) {
		repeat_key = 0;
		arm(repeat_fd, 0, 0);
	}
}

static void keyboard_modifiers(void *data, struct wl_keyboard *wl_keyboard, uint32_t serial,
		uint32_t depressed, uint32_t latched, uint32_t locked, uint32_t group) {
	if (xkb_state != NULL) {
		xkb_state_update_mask(xkb_state, depressed, latched, locked, 0, 0, group);
	}
}

static void keyboard_repeat_info(void *data, struct wl_keyboard *wl_keyboard,
		int32_t rate, int32_t delay) {
	repeat_rate = rate;
	repeat_delay = delay;
}

static const struct wl_keyboard_listener keyboard_listener = {
	.keymap = keyboard_keymap,
	.enter = keyboard_enter,
	.leave = keyboard_leave,
	.key = keyboard_key,
	.modifiers = keyboard_modifiers,
	.repeat_info = keyboard_repeat_info,
};

/* --- The mouse --------------------------------------------------------------- */

static wl_fixed_t pointer_x, pointer_y;

static void pointer_enter(void *data, struct wl_pointer *wl_pointer, uint32_t serial,
		struct wl_surface *entered, wl_fixed_t x, wl_fixed_t y) {
	pointer_x = x;
	pointer_y = y;
	/* The cursor is ours to set on entering, or it stays as the window
	 * below left it. A named one needs no cursor theme. */
	if (cursor == NULL && cursor_shapes != NULL) {
		cursor = wp_cursor_shape_manager_v1_get_pointer(cursor_shapes, wl_pointer);
	}
	if (cursor != NULL) {
		wp_cursor_shape_device_v1_set_shape(cursor, serial, WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_DEFAULT);
	}
}

static void pointer_leave(void *data, struct wl_pointer *wl_pointer, uint32_t serial,
		struct wl_surface *left) {
}

static void pointer_motion(void *data, struct wl_pointer *wl_pointer, uint32_t time,
		wl_fixed_t x, wl_fixed_t y) {
	pointer_x = x;
	pointer_y = y;
}

static void pointer_button(void *data, struct wl_pointer *wl_pointer, uint32_t serial,
		uint32_t time, uint32_t button, uint32_t state) {
	if (button == BTN_LEFT && state == WL_POINTER_BUTTON_STATE_PRESSED && app->click != NULL) {
		last_serial = serial;
		app->click(wl_fixed_to_int(pointer_x), wl_fixed_to_int(pointer_y));
	}
}

static void pointer_axis(void *data, struct wl_pointer *wl_pointer, uint32_t time,
		uint32_t axis, wl_fixed_t value) {
	if (axis == WL_POINTER_AXIS_VERTICAL_SCROLL && value != 0 && app->scroll != NULL) {
		app->scroll(wl_fixed_to_int(pointer_x), wl_fixed_to_int(pointer_y), value > 0 ? 1 : -1);
	}
}

static const struct wl_pointer_listener pointer_listener = {
	.enter = pointer_enter,
	.leave = pointer_leave,
	.motion = pointer_motion,
	.button = pointer_button,
	.axis = pointer_axis,
};

/* --- Activation tokens ------------------------------------------------------- */

static void token_done(void *data, struct xdg_activation_token_v1 *made, const char *token) {
	void (*done)(const char *) = data;
	xdg_activation_token_v1_destroy(made);
	done(token);
}

static const struct xdg_activation_token_v1_listener token_listener = {
	.done = token_done,
};

void box_token(void (*done)(const char *token)) {
	if (activation == NULL || surface == NULL) {
		done(NULL);
		return;
	}
	struct xdg_activation_token_v1 *token = xdg_activation_v1_get_activation_token(activation);
	xdg_activation_token_v1_add_listener(token, &token_listener, (void *)done);
	xdg_activation_token_v1_set_serial(token, last_serial, seat);
	xdg_activation_token_v1_set_surface(token, surface);
	xdg_activation_token_v1_commit(token);
}

/* --- The connection ---------------------------------------------------------- */

static void seat_capabilities(void *data, struct wl_seat *wl_seat, uint32_t capabilities) {
	if ((capabilities & WL_SEAT_CAPABILITY_KEYBOARD) != 0 && keyboard == NULL) {
		keyboard = wl_seat_get_keyboard(wl_seat);
		wl_keyboard_add_listener(keyboard, &keyboard_listener, NULL);
	}
	if ((capabilities & WL_SEAT_CAPABILITY_POINTER) != 0 && pointer == NULL) {
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

static void registry_global(void *data, struct wl_registry *from, uint32_t name,
		const char *interface, uint32_t version) {
	if (strcmp(interface, wl_compositor_interface.name) == 0 && version >= 4) {
		compositor = wl_registry_bind(from, name, &wl_compositor_interface, 4);
	} else if (strcmp(interface, wl_shm_interface.name) == 0) {
		shm = wl_registry_bind(from, name, &wl_shm_interface, 1);
	} else if (strcmp(interface, wl_seat_interface.name) == 0 && seat == NULL && version >= 4) {
		/* 4: the keyboard says how keys repeat. */
		seat = wl_registry_bind(from, name, &wl_seat_interface, 4);
		wl_seat_add_listener(seat, &seat_listener, NULL);
	} else if (strcmp(interface, zwlr_layer_shell_v1_interface.name) == 0) {
		layer_shell = wl_registry_bind(from, name, &zwlr_layer_shell_v1_interface, 1);
	} else if (strcmp(interface, wp_viewporter_interface.name) == 0) {
		viewporter = wl_registry_bind(from, name, &wp_viewporter_interface, 1);
	} else if (strcmp(interface, wp_fractional_scale_manager_v1_interface.name) == 0) {
		scale_manager = wl_registry_bind(from, name,
			&wp_fractional_scale_manager_v1_interface, 1);
	} else if (strcmp(interface, xdg_activation_v1_interface.name) == 0) {
		activation = wl_registry_bind(from, name, &xdg_activation_v1_interface, 1);
	} else if (strcmp(interface, wp_cursor_shape_manager_v1_interface.name) == 0) {
		cursor_shapes = wl_registry_bind(from, name,
			&wp_cursor_shape_manager_v1_interface, 1);
	} else if (app->global != NULL) {
		app->global(from, name, interface, version);
	}
}

static void registry_global_remove(void *data, struct wl_registry *from, uint32_t name) {
}

static const struct wl_registry_listener registry_listener = {
	.global = registry_global,
	.global_remove = registry_global_remove,
};

bool box_connect(const struct box_app *with, bool say_more) {
	app = with;
	verbose = say_more;
	xkb = xkb_context_new(XKB_CONTEXT_NO_FLAGS);
	timer_fd = timerfd_create(CLOCK_MONOTONIC, TFD_CLOEXEC | TFD_NONBLOCK);
	repeat_fd = timerfd_create(CLOCK_MONOTONIC, TFD_CLOEXEC | TFD_NONBLOCK);
	display = wl_display_connect(NULL);
	if (display == NULL) {
		fprintf(stderr, "%s: no Wayland compositor\n", program_invocation_short_name);
		return false;
	}
	registry = wl_display_get_registry(display);
	wl_registry_add_listener(registry, &registry_listener, NULL);
	if (wl_display_roundtrip(display) < 0 || compositor == NULL || shm == NULL ||
			seat == NULL || layer_shell == NULL || xkb == NULL) {
		fprintf(stderr, "%s: this compositor has no layer shell\n", program_invocation_short_name);
		return false;
	}
	return true;
}

bool box_roundtrip(void) {
	return wl_display_roundtrip(display) >= 0;
}

void box_run(void) {
	running = true;
	while (running) {
		if (dirty) {
			paint();
		}
		if (wl_display_flush(display) < 0 && errno != EAGAIN) {
			break;
		}
		struct pollfd fds[3] = {
			{wl_display_get_fd(display), POLLIN, 0},
			{repeat_fd, POLLIN, 0},
			{timer_fd, POLLIN, 0},
		};
		if (poll(fds, LENGTH(fds), -1) < 0) {
			if (errno == EINTR) {
				continue;
			}
			break;
		}
		if (fds[0].revents != 0 && wl_display_dispatch(display) < 0) {
			break;
		}
		uint64_t fired;
		if ((fds[1].revents & POLLIN) != 0 &&
				read(repeat_fd, &fired, sizeof(fired)) == sizeof(fired)) {
			while (fired-- > 0 && running && repeat_key != 0) {
				key_handle(repeat_key);
			}
		}
		if ((fds[2].revents & POLLIN) != 0 &&
				read(timer_fd, &fired, sizeof(fired)) == sizeof(fired) && app->timer != NULL) {
			app->timer();
		}
	}
}

void box_disconnect(void) {
	if (display != NULL) {
		box_close();
		struct wl_proxy *proxies[] = {
			(struct wl_proxy *)cursor, (struct wl_proxy *)pointer, (struct wl_proxy *)keyboard,
			(struct wl_proxy *)seat, (struct wl_proxy *)compositor, (struct wl_proxy *)shm,
			(struct wl_proxy *)layer_shell, (struct wl_proxy *)viewporter,
			(struct wl_proxy *)scale_manager, (struct wl_proxy *)activation,
			(struct wl_proxy *)cursor_shapes, (struct wl_proxy *)registry,
		};
		/* Only our side of each: the compositor drops its own with the
		 * connection. */
		for (size_t i = 0; i < LENGTH(proxies); i++) {
			if (proxies[i] != NULL) {
				wl_proxy_destroy(proxies[i]);
			}
		}
		wl_display_flush(display);
		wl_display_disconnect(display);
		display = NULL;
	}
	xkb_state_unref(xkb_state);
	xkb_keymap_unref(keymap);
	xkb_context_unref(xkb);
	xkb_state = NULL;
	keymap = NULL;
	xkb = NULL;
	if (timer_fd >= 0) {
		close(timer_fd);
	}
	if (repeat_fd >= 0) {
		close(repeat_fd);
	}
	timer_fd = repeat_fd = -1;
	font_close();
}
