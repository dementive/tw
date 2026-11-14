/*
 * Presses key combinations through the virtual-keyboard protocol, so tests
 * exercise the same binding code a real keyboard does.
 *
 *   tw-test-keys super+ctrl+l super+2 x
 *
 * Each argument is pressed and released in turn. Modifiers are super, alt,
 * ctrl and shift; the key is an XKB keysym name (h, Return, Page_Down, 1).
 */
#define _GNU_SOURCE
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>
#include <wayland-client.h>
#include <xkbcommon/xkbcommon.h>

#include "virtual-keyboard-unstable-v1-client-protocol.h"

static struct wl_seat *seat;
static struct zwp_virtual_keyboard_manager_v1 *manager;

static void registry_global(void *data, struct wl_registry *registry, uint32_t name,
		const char *interface, uint32_t version) {
	if (strcmp(interface, wl_seat_interface.name) == 0) {
		seat = wl_registry_bind(registry, name, &wl_seat_interface, 1);
	} else if (strcmp(interface, zwp_virtual_keyboard_manager_v1_interface.name) == 0) {
		manager = wl_registry_bind(registry, name,
			&zwp_virtual_keyboard_manager_v1_interface, 1);
	}
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
	wl_display_roundtrip(display);
	if (seat == NULL || manager == NULL) {
		fprintf(stderr, "compositor is missing wl_seat or the virtual keyboard protocol\n");
		return 1;
	}

	/* The protocol makes the client supply the keymap. */
	struct xkb_context *context = xkb_context_new(XKB_CONTEXT_NO_FLAGS);
	struct xkb_keymap *keymap = xkb_keymap_new_from_names(context, NULL,
		XKB_KEYMAP_COMPILE_NO_FLAGS);
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

	uint32_t time = 1;
	for (int i = 1; i < argc; i++) {
		uint32_t mods = 0;
		char *combo = strdup(argv[i]);
		char *key = combo;
		for (char *plus; (plus = strchr(key, '+')) != NULL && plus[1] != '\0'; key = plus + 1) {
			*plus = '\0';
			if (strcmp(key, "super") == 0) {
				mods |= mod_mask(keymap, XKB_MOD_NAME_LOGO);
			} else if (strcmp(key, "alt") == 0) {
				mods |= mod_mask(keymap, XKB_MOD_NAME_ALT);
			} else if (strcmp(key, "ctrl") == 0) {
				mods |= mod_mask(keymap, XKB_MOD_NAME_CTRL);
			} else if (strcmp(key, "shift") == 0) {
				mods |= mod_mask(keymap, XKB_MOD_NAME_SHIFT);
			} else {
				fprintf(stderr, "unknown modifier '%s' in '%s'\n", key, argv[i]);
				return 1;
			}
		}

		xkb_keysym_t sym = xkb_keysym_from_name(key, XKB_KEYSYM_NO_FLAGS);
		xkb_keycode_t code = sym != XKB_KEY_NoSymbol ? find_keycode(keymap, sym) : 0;
		if (code == 0) {
			fprintf(stderr, "no key produces '%s' in '%s'\n", key, argv[i]);
			return 1;
		}

		/* Wayland key codes are XKB key codes minus 8. */
		zwp_virtual_keyboard_v1_modifiers(keyboard, mods, 0, 0, 0);
		zwp_virtual_keyboard_v1_key(keyboard, time++, code - 8, WL_KEYBOARD_KEY_STATE_PRESSED);
		zwp_virtual_keyboard_v1_key(keyboard, time++, code - 8, WL_KEYBOARD_KEY_STATE_RELEASED);
		zwp_virtual_keyboard_v1_modifiers(keyboard, 0, 0, 0, 0);
		/* Wait until the compositor has handled this combination. */
		wl_display_roundtrip(display);
		free(combo);
	}

	zwp_virtual_keyboard_v1_destroy(keyboard);
	wl_display_roundtrip(display);
	xkb_keymap_unref(keymap);
	xkb_context_unref(context);
	wl_display_disconnect(display);
	return 0;
}
