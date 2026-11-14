#include <stdlib.h>
#include <wlr/backend.h>
#include <wlr/backend/session.h>
#include <wlr/config.h>
#include <wlr/types/wlr_cursor.h>
#include <wlr/types/wlr_data_device.h>
#include <wlr/types/wlr_input_device.h>
#include <wlr/types/wlr_keyboard.h>
#include <wlr/types/wlr_pointer.h>
#include <wlr/types/wlr_seat.h>
#include <wlr/types/wlr_virtual_keyboard_v1.h>
#include <wlr/types/wlr_xcursor_manager.h>
#include <wlr/util/log.h>

#if WLR_HAS_LIBINPUT_BACKEND
#include <libinput.h>
#include <wlr/backend/libinput.h>
#endif

#include "tw.h"
#include "config.h"

#define BINDING_MODS (WLR_MODIFIER_SHIFT | WLR_MODIFIER_CTRL | WLR_MODIFIER_ALT | WLR_MODIFIER_LOGO)

struct tw_keyboard {
	struct wl_list link; /* tw_server.keyboards */
	struct tw_server *server;
	struct wlr_keyboard *wlr_keyboard;
	/* Keycodes whose press ran a binding; their release is swallowed too,
	 * so clients never see half of a key press. 0 marks a free slot. */
	uint32_t consumed[8];
	struct wl_listener modifiers;
	struct wl_listener key;
	struct wl_listener destroy;
};

/* --- Keyboard --------------------------------------------------------------- */

static bool run_bindings(struct tw_keyboard *keyboard, xkb_keycode_t keycode) {
	struct tw_server *server = keyboard->server;
	struct wlr_keyboard *wlr_keyboard = keyboard->wlr_keyboard;
	const xkb_keysym_t *syms;

	/* A virtual keyboard has no keymap until its client uploads one. */
	if (wlr_keyboard->xkb_state == NULL) {
		return false;
	}

	/* Ctrl+Alt+Fn arrives as XF86Switch_VT_n: the way out of a stuck session. */
	int nsyms = xkb_state_key_get_syms(wlr_keyboard->xkb_state, keycode, &syms);
	for (int i = 0; i < nsyms; i++) {
		if (syms[i] >= XKB_KEY_XF86Switch_VT_1 && syms[i] <= XKB_KEY_XF86Switch_VT_12) {
			if (server->session != NULL) {
				wlr_session_change_vt(server->session, syms[i] - XKB_KEY_XF86Switch_VT_1 + 1);
			}
			return true;
		}
	}

	/* Match bindings on the unshifted keysym, so that Shift combinations do
	 * not depend on what Shift produces in the active layout. */
	uint32_t mods = wlr_keyboard_get_modifiers(wlr_keyboard) & BINDING_MODS;
	xkb_layout_index_t layout = xkb_state_key_get_layout(wlr_keyboard->xkb_state, keycode);
	nsyms = xkb_keymap_key_get_syms_by_level(wlr_keyboard->keymap, keycode, layout, 0, &syms);
	for (int i = 0; i < nsyms; i++) {
		for (size_t j = 0; j < LENGTH(bindings); j++) {
			const struct tw_binding *binding = &bindings[j];
			uint32_t want = binding->mods & ~TW_MOD;
			if (binding->mods & TW_MOD) {
				want |= server->mod;
			}
			if (want == mods && binding->sym == syms[i]) {
				binding->action(server, &binding->arg);
				return true;
			}
		}
	}
	return false;
}

static void consumed_add(struct tw_keyboard *keyboard, uint32_t keycode) {
	for (size_t i = 0; i < LENGTH(keyboard->consumed); i++) {
		if (keyboard->consumed[i] == 0) {
			keyboard->consumed[i] = keycode;
			return;
		}
	}
}

static bool consumed_remove(struct tw_keyboard *keyboard, uint32_t keycode) {
	for (size_t i = 0; i < LENGTH(keyboard->consumed); i++) {
		if (keyboard->consumed[i] == keycode) {
			keyboard->consumed[i] = 0;
			return true;
		}
	}
	return false;
}

static void handle_key(struct wl_listener *listener, void *data) {
	struct tw_keyboard *keyboard = wl_container_of(listener, keyboard, key);
	struct wlr_keyboard_key_event *event = data;
	struct wlr_seat *seat = keyboard->server->seat;

	bool handled;
	if (event->state == WL_KEYBOARD_KEY_STATE_PRESSED) {
		/* libinput keycode -> xkbcommon keycode */
		handled = run_bindings(keyboard, event->keycode + 8);
		if (handled) {
			consumed_add(keyboard, event->keycode);
		}
	} else {
		handled = consumed_remove(keyboard, event->keycode);
	}

	if (!handled) {
		wlr_seat_set_keyboard(seat, keyboard->wlr_keyboard);
		wlr_seat_keyboard_notify_key(seat, event->time_msec, event->keycode, event->state);
	}
}

static void handle_modifiers(struct wl_listener *listener, void *data) {
	struct tw_keyboard *keyboard = wl_container_of(listener, keyboard, modifiers);
	struct wlr_seat *seat = keyboard->server->seat;
	/* A seat exposes one keyboard; make it the one that was just used. */
	wlr_seat_set_keyboard(seat, keyboard->wlr_keyboard);
	wlr_seat_keyboard_notify_modifiers(seat, &keyboard->wlr_keyboard->modifiers);
}

static void handle_keyboard_destroy(struct wl_listener *listener, void *data) {
	struct tw_keyboard *keyboard = wl_container_of(listener, keyboard, destroy);
	wl_list_remove(&keyboard->modifiers.link);
	wl_list_remove(&keyboard->key.link);
	wl_list_remove(&keyboard->destroy.link);
	wl_list_remove(&keyboard->link);
	free(keyboard);
}

/*
 * `is_virtual` keyboards come from the virtual-keyboard protocol (wtype, the
 * tests): their client supplies the keymap, so ours must not replace it.
 */
static void new_keyboard(struct tw_server *server, struct wlr_keyboard *wlr_keyboard,
		bool is_virtual) {
	struct tw_keyboard *keyboard = calloc(1, sizeof(*keyboard));
	if (keyboard == NULL) {
		wlr_log(WLR_ERROR, "out of memory, ignoring keyboard %s", wlr_keyboard->base.name);
		return;
	}
	keyboard->server = server;
	keyboard->wlr_keyboard = wlr_keyboard;

	if (!is_virtual) {
		struct xkb_context *context = xkb_context_new(XKB_CONTEXT_NO_FLAGS);
		struct xkb_keymap *keymap = context == NULL ? NULL :
			xkb_keymap_new_from_names(context, NULL, XKB_KEYMAP_COMPILE_NO_FLAGS);
		if (keymap != NULL) {
			wlr_keyboard_set_keymap(wlr_keyboard, keymap);
		} else {
			wlr_log(WLR_ERROR, "failed to compile a keymap, check XKB_DEFAULT_*");
		}
		xkb_keymap_unref(keymap);
		xkb_context_unref(context);
		wlr_keyboard_set_repeat_info(wlr_keyboard, repeat_rate, repeat_delay);
		wlr_seat_set_keyboard(server->seat, wlr_keyboard);
	}

	keyboard->modifiers.notify = handle_modifiers;
	wl_signal_add(&wlr_keyboard->events.modifiers, &keyboard->modifiers);
	keyboard->key.notify = handle_key;
	wl_signal_add(&wlr_keyboard->events.key, &keyboard->key);
	keyboard->destroy.notify = handle_keyboard_destroy;
	wl_signal_add(&wlr_keyboard->base.events.destroy, &keyboard->destroy);

	wl_list_insert(&server->keyboards, &keyboard->link);
}

static void handle_new_virtual_keyboard(struct wl_listener *listener, void *data) {
	struct tw_server *server = wl_container_of(listener, server, new_virtual_keyboard);
	struct wlr_virtual_keyboard_v1 *virtual_keyboard = data;
	new_keyboard(server, &virtual_keyboard->keyboard, true);
}

/* --- Pointer ---------------------------------------------------------------- */

static void new_pointer(struct tw_server *server, struct wlr_input_device *device) {
#if WLR_HAS_LIBINPUT_BACKEND
	if (wlr_input_device_is_libinput(device)) {
		struct libinput_device *libinput = wlr_libinput_get_device_handle(device);
		/* Only touchpads can tap; mice keep libinput's defaults. */
		if (libinput_device_config_tap_get_finger_count(libinput) > 0) {
			libinput_device_config_tap_set_enabled(libinput, touchpad_tap ?
				LIBINPUT_CONFIG_TAP_ENABLED : LIBINPUT_CONFIG_TAP_DISABLED);
			if (libinput_device_config_scroll_has_natural_scroll(libinput)) {
				libinput_device_config_scroll_set_natural_scroll_enabled(libinput,
					touchpad_natural_scroll);
			}
			if (libinput_device_config_dwt_is_available(libinput)) {
				libinput_device_config_dwt_set_enabled(libinput,
					touchpad_disable_while_typing ?
					LIBINPUT_CONFIG_DWT_ENABLED : LIBINPUT_CONFIG_DWT_DISABLED);
			}
		}
	}
#endif
	wlr_cursor_attach_input_device(server->cursor, device);
}

static void handle_new_input(struct wl_listener *listener, void *data) {
	struct tw_server *server = wl_container_of(listener, server, new_input);
	struct wlr_input_device *device = data;

	switch (device->type) {
	case WLR_INPUT_DEVICE_KEYBOARD:
		new_keyboard(server, wlr_keyboard_from_input_device(device), false);
		break;
	case WLR_INPUT_DEVICE_POINTER:
		new_pointer(server, device);
		break;
	default:
		break;
	}
}

static void pointer_update(struct tw_server *server, uint32_t time_msec) {
	double sx, sy;
	struct wlr_surface *surface = NULL;
	window_at(server, server->cursor->x, server->cursor->y, &surface, &sx, &sy);

	if (surface == NULL) {
		wlr_cursor_set_xcursor(server->cursor, server->cursor_manager, "default");
		wlr_seat_pointer_clear_focus(server->seat);
		return;
	}
	/* wlroots drops duplicate enter/motion events for us. */
	wlr_seat_pointer_notify_enter(server->seat, surface, sx, sy);
	wlr_seat_pointer_notify_motion(server->seat, time_msec, sx, sy);
}

static void handle_cursor_motion(struct wl_listener *listener, void *data) {
	struct tw_server *server = wl_container_of(listener, server, cursor_motion);
	struct wlr_pointer_motion_event *event = data;
	wlr_cursor_move(server->cursor, &event->pointer->base, event->delta_x, event->delta_y);
	pointer_update(server, event->time_msec);
}

static void handle_cursor_motion_absolute(struct wl_listener *listener, void *data) {
	/* What the nested backend sends: a position in 0..1 inside its window. */
	struct tw_server *server = wl_container_of(listener, server, cursor_motion_absolute);
	struct wlr_pointer_motion_absolute_event *event = data;
	wlr_cursor_warp_absolute(server->cursor, &event->pointer->base, event->x, event->y);
	pointer_update(server, event->time_msec);
}

static void handle_cursor_button(struct wl_listener *listener, void *data) {
	struct tw_server *server = wl_container_of(listener, server, cursor_button);
	struct wlr_pointer_button_event *event = data;

	wlr_seat_pointer_notify_button(server->seat, event->time_msec, event->button, event->state);

	if (event->state == WL_POINTER_BUTTON_STATE_PRESSED) {
		double sx, sy;
		struct wlr_surface *surface = NULL;
		struct tw_window *window = window_at(server,
			server->cursor->x, server->cursor->y, &surface, &sx, &sy);
		if (window != NULL) {
			window_focus(server, window);
			strip_arrange(server);
		}
	}
}

static void handle_cursor_axis(struct wl_listener *listener, void *data) {
	struct tw_server *server = wl_container_of(listener, server, cursor_axis);
	struct wlr_pointer_axis_event *event = data;
	wlr_seat_pointer_notify_axis(server->seat, event->time_msec, event->orientation,
		event->delta, event->delta_discrete, event->source, event->relative_direction);
}

static void handle_cursor_frame(struct wl_listener *listener, void *data) {
	struct tw_server *server = wl_container_of(listener, server, cursor_frame);
	wlr_seat_pointer_notify_frame(server->seat);
}

/* --- Seat ------------------------------------------------------------------- */

static void handle_request_set_cursor(struct wl_listener *listener, void *data) {
	struct tw_server *server = wl_container_of(listener, server, request_set_cursor);
	struct wlr_seat_pointer_request_set_cursor_event *event = data;
	/* Any client may ask; only the one under the pointer gets its way. */
	if (server->seat->pointer_state.focused_client == event->seat_client) {
		wlr_cursor_set_surface(server->cursor, event->surface, event->hotspot_x, event->hotspot_y);
	}
}

static void handle_pointer_focus_change(struct wl_listener *listener, void *data) {
	struct tw_server *server = wl_container_of(listener, server, pointer_focus_change);
	struct wlr_seat_pointer_focus_change_event *event = data;
	if (event->new_surface == NULL) {
		wlr_cursor_set_xcursor(server->cursor, server->cursor_manager, "default");
	}
}

static void handle_request_set_selection(struct wl_listener *listener, void *data) {
	struct tw_server *server = wl_container_of(listener, server, request_set_selection);
	struct wlr_seat_request_set_selection_event *event = data;
	wlr_seat_set_selection(server->seat, event->source, event->serial);
}

void input_init(struct tw_server *server) {
	wl_list_init(&server->keyboards);

	server->cursor = wlr_cursor_create();
	wlr_cursor_attach_output_layout(server->cursor, server->output_layout);
	server->cursor_manager = wlr_xcursor_manager_create(NULL, 24);

	server->cursor_motion.notify = handle_cursor_motion;
	wl_signal_add(&server->cursor->events.motion, &server->cursor_motion);
	server->cursor_motion_absolute.notify = handle_cursor_motion_absolute;
	wl_signal_add(&server->cursor->events.motion_absolute, &server->cursor_motion_absolute);
	server->cursor_button.notify = handle_cursor_button;
	wl_signal_add(&server->cursor->events.button, &server->cursor_button);
	server->cursor_axis.notify = handle_cursor_axis;
	wl_signal_add(&server->cursor->events.axis, &server->cursor_axis);
	server->cursor_frame.notify = handle_cursor_frame;
	wl_signal_add(&server->cursor->events.frame, &server->cursor_frame);

	server->seat = wlr_seat_create(server->display, "seat0");
	/* Always advertise both: a laptop always has them, and virtual keyboards
	 * come and go without making every client rebind its keyboard. */
	wlr_seat_set_capabilities(server->seat,
		WL_SEAT_CAPABILITY_POINTER | WL_SEAT_CAPABILITY_KEYBOARD);
	server->request_set_cursor.notify = handle_request_set_cursor;
	wl_signal_add(&server->seat->events.request_set_cursor, &server->request_set_cursor);
	server->pointer_focus_change.notify = handle_pointer_focus_change;
	wl_signal_add(&server->seat->pointer_state.events.focus_change, &server->pointer_focus_change);
	server->request_set_selection.notify = handle_request_set_selection;
	wl_signal_add(&server->seat->events.request_set_selection, &server->request_set_selection);

	server->new_input.notify = handle_new_input;
	wl_signal_add(&server->backend->events.new_input, &server->new_input);

	server->virtual_keyboard_manager = wlr_virtual_keyboard_manager_v1_create(server->display);
	server->new_virtual_keyboard.notify = handle_new_virtual_keyboard;
	wl_signal_add(&server->virtual_keyboard_manager->events.new_virtual_keyboard,
		&server->new_virtual_keyboard);
}

void input_finish(struct tw_server *server) {
	wl_list_remove(&server->new_input.link);
	wl_list_remove(&server->new_virtual_keyboard.link);
	wl_list_remove(&server->cursor_motion.link);
	wl_list_remove(&server->cursor_motion_absolute.link);
	wl_list_remove(&server->cursor_button.link);
	wl_list_remove(&server->cursor_axis.link);
	wl_list_remove(&server->cursor_frame.link);
	wl_list_remove(&server->request_set_cursor.link);
	wl_list_remove(&server->pointer_focus_change.link);
	wl_list_remove(&server->request_set_selection.link);
	wlr_xcursor_manager_destroy(server->cursor_manager);
	wlr_cursor_destroy(server->cursor);
}
