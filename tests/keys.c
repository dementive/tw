/*
 * Presses key combinations through the virtual-keyboard protocol, and moves
 * and clicks the mouse through the virtual-pointer protocol, so tests
 * exercise the same input code real devices do.
 *
 *   tw-test-keys super+ctrl+l super+2 x
 *   tw-test-keys move:100,80:1024,576 click rel:30,-5
 *
 * Each argument is carried out in turn. A key combination is pressed and
 * released: modifiers are super, alt, ctrl and shift; the key is an XKB
 * keysym name (h, Return, Page_Down, 1). The mouse actions are:
 *
 *   move:X,Y:W,H   put the cursor at X,Y on a screen area W wide and H high
 *   rel:DX,DY      move the mouse by DX,DY
 *   click          press and release the left button
 *   down, up       press the left button, let it go: a drag in between
 *   click:B, down:B, up:B  the same with button B, an evdev code (274 is
 *                  the middle button, 275 and 276 the thumb buttons)
 *   wheel:N        turn the wheel N notches, down (N < 0: up), in one event
 *   tilt:N         tilt the wheel N notches right (N < 0: left)
 *   pause:MS       wait, holding what is held, as a hand does; the button
 *                  is let go before this program exits
 *   hold:MODS      hold modifiers down (hold:super, hold:super+ctrl) through
 *                  the mouse actions that follow, until the next key
 *                  combination; hold: with none lets them go
 *
 * Each action has been handled by tw by the time the next one starts, and
 * all of them by the time this program exits; it fails if tw goes away.
 * Events carry the time they were sent, in milliseconds, as a device's do.
 */
#define _GNU_SOURCE
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <time.h>
#include <unistd.h>
#include <wayland-client.h>
#include <xkbcommon/xkbcommon.h>

#include "virtual-keyboard-unstable-v1-client-protocol.h"
#include "wlr-virtual-pointer-unstable-v1-client-protocol.h"

#define BTN_LEFT 0x110 /* evdev code of the left mouse button */

static struct wl_seat *seat;
static struct zwp_virtual_keyboard_manager_v1 *manager;
static struct zwlr_virtual_pointer_manager_v1 *pointer_manager;
static struct zwlr_virtual_pointer_v1 *pointer; /* made with the first mouse action */

/* The time of an event: the monotonic clock in milliseconds, as libinput's,
 * and later than the last one. */
static uint32_t event_time(void) {
	static uint32_t last;
	struct timespec now;
	clock_gettime(CLOCK_MONOTONIC, &now);
	uint32_t ms = (uint32_t)(now.tv_sec * 1000 + now.tv_nsec / 1000000);
	last = ms > last ? ms : last + 1;
	return last;
}

/* Waits until tw has handled everything sent so far. */
static void sync_display(struct wl_display *display) {
	if (wl_display_roundtrip(display) < 0) {
		fprintf(stderr, "lost the connection to the compositor\n");
		exit(1);
	}
}

static void registry_global(void *data, struct wl_registry *registry, uint32_t name,
		const char *interface, uint32_t version) {
	if (strcmp(interface, wl_seat_interface.name) == 0) {
		seat = wl_registry_bind(registry, name, &wl_seat_interface, 1);
	} else if (strcmp(interface, zwp_virtual_keyboard_manager_v1_interface.name) == 0) {
		manager = wl_registry_bind(registry, name,
			&zwp_virtual_keyboard_manager_v1_interface, 1);
	} else if (strcmp(interface, zwlr_virtual_pointer_manager_v1_interface.name) == 0) {
		pointer_manager = wl_registry_bind(registry, name,
			&zwlr_virtual_pointer_manager_v1_interface, 1);
	}
}

/* Carries out a mouse action; false if `action` is not one. */
static bool mouse_action(struct wl_display *display, const char *action) {
	int x, y, width, height, notches;
	unsigned button = BTN_LEFT;
	char name[8] = "";
	bool move = sscanf(action, "move:%d,%d:%d,%d", &x, &y, &width, &height) == 4;
	bool rel = !move && sscanf(action, "rel:%d,%d", &x, &y) == 2;
	bool wheel = sscanf(action, "wheel:%d", &notches) == 1;
	bool tilt = sscanf(action, "tilt:%d", &notches) == 1;
	if (sscanf(action, "%7[a-z]:%u", name, &button) != 2) {
		snprintf(name, sizeof(name), "%s", action);
	}
	bool click = strcmp(name, "click") == 0;
	bool down = strcmp(name, "down") == 0, up = strcmp(name, "up") == 0;
	if (!move && !rel && !wheel && !tilt && !click && !down && !up) {
		return false;
	}
	if (pointer_manager == NULL) {
		fprintf(stderr, "compositor is missing the virtual pointer protocol\n");
		exit(1);
	}
	if (pointer == NULL) {
		pointer = zwlr_virtual_pointer_manager_v1_create_virtual_pointer(pointer_manager, seat);
	}
	if (move) {
		zwlr_virtual_pointer_v1_motion_absolute(pointer, event_time(), (uint32_t)x, (uint32_t)y,
			(uint32_t)width, (uint32_t)height);
	} else if (rel) {
		zwlr_virtual_pointer_v1_motion(pointer, event_time(),
			wl_fixed_from_int(x), wl_fixed_from_int(y));
	} else if (wheel || tilt) {
		/* What libinput makes of a wheel: 15 pixels a notch. */
		zwlr_virtual_pointer_v1_axis_source(pointer, WL_POINTER_AXIS_SOURCE_WHEEL);
		zwlr_virtual_pointer_v1_axis_discrete(pointer, event_time(),
			tilt ? WL_POINTER_AXIS_HORIZONTAL_SCROLL : WL_POINTER_AXIS_VERTICAL_SCROLL,
			wl_fixed_from_int(15 * notches), notches);
	} else {
		if (click || down) {
			zwlr_virtual_pointer_v1_button(pointer, event_time(), button,
				WL_POINTER_BUTTON_STATE_PRESSED);
		}
		if (click) {
			zwlr_virtual_pointer_v1_frame(pointer);
		}
		if (click || up) {
			zwlr_virtual_pointer_v1_button(pointer, event_time(), button,
				WL_POINTER_BUTTON_STATE_RELEASED);
		}
	}
	zwlr_virtual_pointer_v1_frame(pointer);
	sync_display(display);
	return true;
}

static void registry_global_remove(void *data, struct wl_registry *registry, uint32_t name) {
}

static const struct wl_registry_listener registry_listener = {
	.global = registry_global,
	.global_remove = registry_global_remove,
};

static uint32_t mod_mask(struct xkb_keymap *keymap, const char *name) {
	xkb_mod_index_t index = xkb_keymap_mod_get_index(keymap, name);
	return index == XKB_MOD_INVALID ? 0 : 1u << index;
}

/* The modifier named `name` (super, alt, ctrl, shift) as a mask, or 0. */
static uint32_t mod_named(struct xkb_keymap *keymap, const char *name) {
	if (strcmp(name, "super") == 0) {
		return mod_mask(keymap, XKB_MOD_NAME_LOGO);
	} else if (strcmp(name, "alt") == 0) {
		return mod_mask(keymap, XKB_MOD_NAME_ALT);
	} else if (strcmp(name, "ctrl") == 0) {
		return mod_mask(keymap, XKB_MOD_NAME_CTRL);
	} else if (strcmp(name, "shift") == 0) {
		return mod_mask(keymap, XKB_MOD_NAME_SHIFT);
	}
	return 0;
}

/* Keycode whose unshifted keysym is `sym`, or 0. */
static xkb_keycode_t find_keycode(struct xkb_keymap *keymap, xkb_keysym_t sym) {
	xkb_keycode_t max = xkb_keymap_max_keycode(keymap);
	for (xkb_keycode_t code = xkb_keymap_min_keycode(keymap); code <= max; code++) {
		const xkb_keysym_t *syms;
		if (xkb_keymap_key_get_syms_by_level(keymap, code, 0, 0, &syms) == 1 && syms[0] == sym) {
			return code;
		}
	}
	return 0;
}

int main(int argc, char *argv[]) {
	struct wl_display *display = wl_display_connect(NULL);
	if (display == NULL) {
		fprintf(stderr, "cannot connect to a Wayland display\n");
		return 1;
	}
	struct wl_registry *registry = wl_display_get_registry(display);
	wl_registry_add_listener(registry, &registry_listener, NULL);
	sync_display(display);
	if (seat == NULL || manager == NULL) {
		fprintf(stderr, "compositor is missing wl_seat or the virtual keyboard protocol\n");
		return 1;
	}

	/* The protocol makes the client supply the keymap. */
	struct xkb_context *context = xkb_context_new(XKB_CONTEXT_NO_FLAGS);
	struct xkb_keymap *keymap = context == NULL ? NULL :
		xkb_keymap_new_from_names(context, NULL, XKB_KEYMAP_COMPILE_NO_FLAGS);
	if (keymap == NULL) {
		fprintf(stderr, "cannot compile the default keymap\n");
		return 1;
	}
	char *keymap_string = xkb_keymap_get_as_string(keymap, XKB_KEYMAP_FORMAT_TEXT_V1);
	size_t keymap_size = strlen(keymap_string) + 1;
	int fd = memfd_create("tw-test-keymap", MFD_CLOEXEC);
	if (fd < 0 || write(fd, keymap_string, keymap_size) != (ssize_t)keymap_size) {
		perror("keymap");
		return 1;
	}

	struct zwp_virtual_keyboard_v1 *keyboard =
		zwp_virtual_keyboard_manager_v1_create_virtual_keyboard(manager, seat);
	zwp_virtual_keyboard_v1_keymap(keyboard, WL_KEYBOARD_KEYMAP_FORMAT_XKB_V1, fd,
		(uint32_t)keymap_size);
	close(fd);
	free(keymap_string);
	sync_display(display);

	for (int i = 1; i < argc; i++) {
		if (mouse_action(display, argv[i])) {
			continue;
		}
		int ms;
		if (sscanf(argv[i], "pause:%d", &ms) == 1) {
			usleep((useconds_t)ms * 1000);
			continue;
		}
		uint32_t mods = 0;
		char *combo = strdup(argv[i]);
		if (combo == NULL) {
			perror("strdup");
			return 1;
		}
		if (strncmp(combo, "hold:", 5) == 0) {
			for (char *name = strtok(combo + 5, "+"); name != NULL; name = strtok(NULL, "+")) {
				uint32_t mod = mod_named(keymap, name);
				if (mod == 0) {
					fprintf(stderr, "unknown modifier '%s' in '%s'\n", name, argv[i]);
					return 1;
				}
				mods |= mod;
			}
			zwp_virtual_keyboard_v1_modifiers(keyboard, mods, 0, 0, 0);
			sync_display(display);
			free(combo);
			continue;
		}
		char *key = combo;
		for (char *plus; (plus = strchr(key, '+')) != NULL && plus[1] != '\0'; key = plus + 1) {
			*plus = '\0';
			uint32_t mod = mod_named(keymap, key);
			if (mod == 0) {
				fprintf(stderr, "unknown modifier '%s' in '%s'\n", key, argv[i]);
				return 1;
			}
			mods |= mod;
		}

		xkb_keysym_t sym = xkb_keysym_from_name(key, XKB_KEYSYM_NO_FLAGS);
		xkb_keycode_t code = sym != XKB_KEY_NoSymbol ? find_keycode(keymap, sym) : 0;
		if (code == 0) {
			fprintf(stderr, "no key produces '%s' in '%s'\n", key, argv[i]);
			return 1;
		}

		/* Wayland key codes are XKB key codes minus 8. */
		zwp_virtual_keyboard_v1_modifiers(keyboard, mods, 0, 0, 0);
		zwp_virtual_keyboard_v1_key(keyboard, event_time(), code - 8, WL_KEYBOARD_KEY_STATE_PRESSED);
		zwp_virtual_keyboard_v1_key(keyboard, event_time(), code - 8, WL_KEYBOARD_KEY_STATE_RELEASED);
		zwp_virtual_keyboard_v1_modifiers(keyboard, 0, 0, 0, 0);
		sync_display(display);
		free(combo);
	}

	/* Destroying the pointer lets go of a button still held. */
	if (pointer != NULL) {
		zwlr_virtual_pointer_v1_destroy(pointer);
	}
	zwp_virtual_keyboard_v1_destroy(keyboard);
	sync_display(display);
	xkb_keymap_unref(keymap);
	xkb_context_unref(context);
	wl_registry_destroy(registry);
	wl_display_disconnect(display);
	return 0;
}
