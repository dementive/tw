#include <math.h>
#include <stdlib.h>
#include <wlr/backend.h>
#include <wlr/backend/session.h>
#include <wlr/config.h>
#include <wlr/types/wlr_cursor.h>
#include <wlr/types/wlr_cursor_shape_v1.h>
#include <wlr/types/wlr_data_device.h>
#include <wlr/types/wlr_input_device.h>
#include <wlr/types/wlr_keyboard.h>
#include <wlr/types/wlr_pointer.h>
#include <wlr/types/wlr_pointer_constraints_v1.h>
#include <wlr/types/wlr_pointer_gestures_v1.h>
#include <wlr/types/wlr_primary_selection.h>
#include <wlr/types/wlr_primary_selection_v1.h>
#include <wlr/types/wlr_relative_pointer_v1.h>
#include <wlr/types/wlr_seat.h>
#include <wlr/types/wlr_switch.h>
#include <wlr/types/wlr_virtual_keyboard_v1.h>
#include <wlr/types/wlr_virtual_pointer_v1.h>
#include <wlr/types/wlr_xcursor_manager.h>
#include <wlr/util/log.h>
#include <wlr/util/region.h>

#if WLR_HAS_LIBINPUT_BACKEND
#include <libinput.h>
#include <wlr/backend/libinput.h>
#endif

#include "tw.h"
#include "config.h"

#define BINDING_MODS (WLR_MODIFIER_SHIFT | WLR_MODIFIER_CTRL | WLR_MODIFIER_ALT | WLR_MODIFIER_LOGO)

struct tw_keyboard {
	struct tw_server *server;
	struct wlr_keyboard *wlr_keyboard;
	/* Keycodes whose press ran a binding; their release is swallowed too,
	 * so clients never see half of a key press. 0 marks a free slot. */
	uint32_t consumed[8];
	struct wl_listener modifiers;
	struct wl_listener key;
	struct wl_listener destroy;
};

/* A pointer lock or confinement a client has asked for. */
struct tw_constraint {
	struct tw_server *server;
	struct wlr_pointer_constraint_v1 *constraint;
	struct wl_listener destroy;
};

/* A laptop lid. */
struct tw_switch {
	struct tw_server *server;
	struct wl_listener toggle;
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

	/* A locked session runs no bindings: every key goes to the lock screen. */
	if (server->locked) {
		return false;
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
		/* Only presses count as activity. Otherwise letting go of the keys
		 * that blanked the screen would wake it again at once. */
		idle_activity(keyboard->server);
		/* A locked session whose lock screen died gets a new one. */
		lock_key_pressed(keyboard->server);
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

}

static void handle_new_virtual_keyboard(struct wl_listener *listener, void *data) {
	struct tw_server *server = wl_container_of(listener, server, new_virtual_keyboard);
	struct wlr_virtual_keyboard_v1 *virtual_keyboard = data;
	new_keyboard(server, &virtual_keyboard->keyboard, true);
}

static void handle_new_virtual_pointer(struct wl_listener *listener, void *data) {
	/* Remote-control tools and the tests move the same cursor a mouse does. */
	struct tw_server *server = wl_container_of(listener, server, new_virtual_pointer);
	struct wlr_virtual_pointer_v1_new_pointer_event *event = data;
	wlr_cursor_attach_input_device(server->cursor, &event->new_pointer->pointer.base);
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

/* --- Lid switch ------------------------------------------------------------- */

static void handle_switch_toggle(struct wl_listener *listener, void *data) {
	struct tw_switch *lid = wl_container_of(listener, lid, toggle);
	struct wlr_switch_toggle_event *event = data;
	if (event->switch_type != WLR_SWITCH_TYPE_LID) {
		return;
	}
	if (event->switch_state == WLR_SWITCH_STATE_ON) {
		/* Closed. logind suspends the machine by itself; the session has to
		 * be locked before it does. */
		if (lock_on_lid_close) {
			lock_now(lid->server);
		}
	} else {
		/* Opened: turn the screen back on if it was off. */
		idle_activity(lid->server);
	}
}

static void handle_switch_destroy(struct wl_listener *listener, void *data) {
	struct tw_switch *lid = wl_container_of(listener, lid, destroy);
	wl_list_remove(&lid->toggle.link);
	wl_list_remove(&lid->destroy.link);
	free(lid);
}

static void new_switch(struct tw_server *server, struct wlr_switch *wlr_switch) {
	struct tw_switch *lid = calloc(1, sizeof(*lid));
	if (lid == NULL) {
		return;
	}
	lid->server = server;
	lid->toggle.notify = handle_switch_toggle;
	wl_signal_add(&wlr_switch->events.toggle, &lid->toggle);
	lid->destroy.notify = handle_switch_destroy;
	wl_signal_add(&wlr_switch->base.events.destroy, &lid->destroy);
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
	case WLR_INPUT_DEVICE_SWITCH:
		new_switch(server, wlr_switch_from_input_device(device));
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

void pointer_refocus(struct tw_server *server) {
	/* Not in the middle of a click or a drag: the button's release goes to
	 * where its press went. */
	if (server->cursor == NULL || server->seat->pointer_state.button_count > 0) {
		return;
	}
	double sx, sy;
	struct wlr_surface *surface = NULL;
	window_at(server, server->cursor->x, server->cursor->y, &surface, &sx, &sy);
	/* Over nothing, as before: no cursor to set. That keeps the cursor
	 * theme unloaded until the pointer first moves. */
	if (surface == NULL && server->seat->pointer_state.focused_surface == NULL) {
		return;
	}
	pointer_update(server, (uint32_t)now_ms());
}

/*
 * Pointer lock and confinement. A game or a 3D editor locks the pointer in
 * place and steers by raw mouse movement instead; other programs confine it
 * to a region. A constraint is in force only while its window has both the
 * pointer and the keyboard, so any focus binding gets the user out.
 */

/* A client that held the pointer locked may say where it should reappear. */
static void constraint_apply_hint(struct tw_server *server,
		struct wlr_pointer_constraint_v1 *constraint) {
	struct wlr_seat *seat = server->seat;
	if (constraint->type != WLR_POINTER_CONSTRAINT_V1_LOCKED ||
			!constraint->current.cursor_hint.enabled ||
			seat->pointer_state.focused_surface != constraint->surface) {
		return;
	}
	/* The locked pointer has not moved, so where it is on the surface tells
	 * us where the surface is on screen. */
	double sx = constraint->current.cursor_hint.x, sy = constraint->current.cursor_hint.y;
	wlr_cursor_warp(server->cursor, NULL,
		server->cursor->x - seat->pointer_state.sx + sx,
		server->cursor->y - seat->pointer_state.sy + sy);
	wlr_seat_pointer_warp(seat, sx, sy);
}

void pointer_constraint_update(struct tw_server *server) {
	struct wlr_seat *seat = server->seat;
	struct wlr_surface *pointed = seat->pointer_state.focused_surface;
	struct wlr_surface *typed = seat->keyboard_state.focused_surface;
	struct wlr_pointer_constraint_v1 *constraint = NULL;
	if (pointed != NULL && typed != NULL &&
			wlr_surface_get_root_surface(pointed) == wlr_surface_get_root_surface(typed)) {
		constraint = wlr_pointer_constraints_v1_constraint_for_surface(
			server->pointer_constraints, pointed, seat);
		/* One we could not attach a destroy listener to is never used. */
		if (constraint != NULL && constraint->data == NULL) {
			constraint = NULL;
		}
	}
	struct wlr_pointer_constraint_v1 *old = server->active_constraint;
	if (constraint == old) {
		return;
	}
	server->active_constraint = constraint;
	if (old != NULL) {
		constraint_apply_hint(server, old);
		/* Destroys a one-shot constraint; handle_constraint_destroy() copes. */
		wlr_pointer_constraint_v1_send_deactivated(old);
	}
	if (constraint != NULL) {
		wlr_pointer_constraint_v1_send_activated(constraint);
	}
}

static void handle_constraint_destroy(struct wl_listener *listener, void *data) {
	struct tw_constraint *constraint = wl_container_of(listener, constraint, destroy);
	struct tw_server *server = constraint->server;
	if (server->active_constraint == constraint->constraint) {
		/* How a client lets go of the pointer: by destroying its lock. */
		server->active_constraint = NULL;
		constraint_apply_hint(server, constraint->constraint);
	}
	constraint->constraint->data = NULL;
	wl_list_remove(&constraint->destroy.link);
	free(constraint);
}

static void handle_new_constraint(struct wl_listener *listener, void *data) {
	struct tw_server *server = wl_container_of(listener, server, new_constraint);
	struct wlr_pointer_constraint_v1 *wlr_constraint = data;
	struct tw_constraint *constraint = calloc(1, sizeof(*constraint));
	if (constraint == NULL) {
		return;
	}
	constraint->server = server;
	constraint->constraint = wlr_constraint;
	constraint->destroy.notify = handle_constraint_destroy;
	wl_signal_add(&wlr_constraint->events.destroy, &constraint->destroy);
	wlr_constraint->data = constraint;
	pointer_constraint_update(server);
}

static void handle_cursor_motion(struct wl_listener *listener, void *data) {
	struct tw_server *server = wl_container_of(listener, server, cursor_motion);
	struct wlr_pointer_motion_event *event = data;
	struct wlr_seat *seat = server->seat;
	double dx = event->delta_x, dy = event->delta_y;
	idle_activity(server);

	/* Raw movement, for clients that asked for it: the mouse as a device
	 * rather than as a cursor position. */
	wlr_relative_pointer_manager_v1_send_relative_motion(server->relative_pointer_manager,
		seat, (uint64_t)event->time_msec * 1000, dx, dy, event->unaccel_dx, event->unaccel_dy);

	struct wlr_pointer_constraint_v1 *constraint = server->active_constraint;
	if (constraint != NULL) {
		if (constraint->type == WLR_POINTER_CONSTRAINT_V1_LOCKED) {
			return; /* the cursor stays where it is */
		}
		double sx = seat->pointer_state.sx, sy = seat->pointer_state.sy, cx, cy;
		if (seat->pointer_state.focused_surface == constraint->surface &&
				wlr_region_confine(&constraint->region, sx, sy, sx + dx, sy + dy, &cx, &cy)) {
			dx = cx - sx;
			dy = cy - sy;
		}
	}
	wlr_cursor_move(server->cursor, &event->pointer->base, dx, dy);
	pointer_update(server, event->time_msec);
}

static void handle_cursor_motion_absolute(struct wl_listener *listener, void *data) {
	/* What the nested backend and virtual pointers send: a position in 0..1
	 * across the screen. */
	struct tw_server *server = wl_container_of(listener, server, cursor_motion_absolute);
	struct wlr_pointer_motion_absolute_event *event = data;
	idle_activity(server);
	if (server->active_constraint != NULL) {
		return; /* a position cannot be confined or turned into movement */
	}
	wlr_cursor_warp_absolute(server->cursor, &event->pointer->base, event->x, event->y);
	pointer_update(server, event->time_msec);
}

static void handle_cursor_button(struct wl_listener *listener, void *data) {
	struct tw_server *server = wl_container_of(listener, server, cursor_button);
	struct wlr_pointer_button_event *event = data;

	idle_activity(server);
	wlr_seat_pointer_notify_button(server->seat, event->time_msec, event->button, event->state);

	if (event->state == WL_POINTER_BUTTON_STATE_PRESSED) {
		double sx, sy;
		struct wlr_surface *surface = NULL;
		struct tw_window *window = window_at(server,
			server->cursor->x, server->cursor->y, &surface, &sx, &sy);
		if (window != NULL) {
			window_focus(server, window);
			strip_arrange(server);
		} else if (!server->locked) {
			/* A click on an empty part of another screen moves the focus
			 * there, so that the next window opens on it. */
			struct tw_output *output = output_at(server, server->cursor->x, server->cursor->y);
			if (output != NULL) {
				strip_focus_output(server, output);
			}
		}
	}
}

static void handle_cursor_axis(struct wl_listener *listener, void *data) {
	struct tw_server *server = wl_container_of(listener, server, cursor_axis);
	struct wlr_pointer_axis_event *event = data;
	idle_activity(server);
	wlr_seat_pointer_notify_axis(server->seat, event->time_msec, event->orientation,
		event->delta, event->delta_discrete, event->source, event->relative_direction);
}

static void handle_cursor_frame(struct wl_listener *listener, void *data) {
	struct tw_server *server = wl_container_of(listener, server, cursor_frame);
	wlr_seat_pointer_notify_frame(server->seat);
}

/* --- Touchpad gestures ------------------------------------------------------ */

/*
 * A swipe with `swipe_fingers` fingers is ours: sideways it drags the strip,
 * vertically it switches workspace. Which of the two is decided by the first
 * few pixels of movement. Every other gesture is passed to the application
 * under the pointer (pinch to zoom, swipe back in a browser).
 */
enum { SWIPE_UNDECIDED, SWIPE_SIDEWAYS, SWIPE_VERTICAL };

static void handle_swipe_begin(struct wl_listener *listener, void *data) {
	struct tw_server *server = wl_container_of(listener, server, swipe_begin);
	struct wlr_pointer_swipe_begin_event *event = data;
	idle_activity(server);
	if (event->fingers == swipe_fingers && !server->locked) {
		server->swipe_ours = true;
		server->swipe_axis = SWIPE_UNDECIDED;
		server->swipe_dx = server->swipe_dy = 0;
		return;
	}
	wlr_pointer_gestures_v1_send_swipe_begin(server->pointer_gestures, server->seat,
		event->time_msec, event->fingers);
}

static void handle_swipe_update(struct wl_listener *listener, void *data) {
	struct tw_server *server = wl_container_of(listener, server, swipe_update);
	struct wlr_pointer_swipe_update_event *event = data;
	idle_activity(server);
	if (!server->swipe_ours) {
		wlr_pointer_gestures_v1_send_swipe_update(server->pointer_gestures, server->seat,
			event->time_msec, event->dx, event->dy);
		return;
	}
	server->swipe_dx += event->dx;
	server->swipe_dy += event->dy;
	if (server->swipe_axis == SWIPE_UNDECIDED &&
			fabs(server->swipe_dx) + fabs(server->swipe_dy) > 8) {
		if (fabs(server->swipe_dx) > fabs(server->swipe_dy)) {
			server->swipe_axis = SWIPE_SIDEWAYS;
			strip_swipe_begin(server);
			/* Do not lose the movement that made up our mind. */
			strip_swipe_update(server, server->swipe_dx - event->dx, event->time_msec);
		} else {
			server->swipe_axis = SWIPE_VERTICAL;
		}
	}
	if (server->swipe_axis == SWIPE_SIDEWAYS) {
		strip_swipe_update(server, event->dx, event->time_msec);
	}
}

static void handle_swipe_end(struct wl_listener *listener, void *data) {
	struct tw_server *server = wl_container_of(listener, server, swipe_end);
	struct wlr_pointer_swipe_end_event *event = data;
	if (!server->swipe_ours) {
		wlr_pointer_gestures_v1_send_swipe_end(server->pointer_gestures, server->seat,
			event->time_msec, event->cancelled);
		return;
	}
	server->swipe_ours = false;
	if (server->swipe_axis == SWIPE_SIDEWAYS) {
		strip_swipe_end(server, event->cancelled);
	} else if (server->swipe_axis == SWIPE_VERTICAL && !event->cancelled &&
			fabs(server->swipe_dy) >= swipe_workspace_distance) {
		/* Workspaces are stacked: pushing the screen up shows the next one. */
		union tw_arg arg = {.i = server->swipe_dy < 0 ? +1 : -1};
		action_workspace_relative(server, &arg);
	}
}

static void handle_pinch_begin(struct wl_listener *listener, void *data) {
	struct tw_server *server = wl_container_of(listener, server, pinch_begin);
	struct wlr_pointer_pinch_begin_event *event = data;
	idle_activity(server);
	wlr_pointer_gestures_v1_send_pinch_begin(server->pointer_gestures, server->seat,
		event->time_msec, event->fingers);
}

static void handle_pinch_update(struct wl_listener *listener, void *data) {
	struct tw_server *server = wl_container_of(listener, server, pinch_update);
	struct wlr_pointer_pinch_update_event *event = data;
	idle_activity(server);
	wlr_pointer_gestures_v1_send_pinch_update(server->pointer_gestures, server->seat,
		event->time_msec, event->dx, event->dy, event->scale, event->rotation);
}

static void handle_pinch_end(struct wl_listener *listener, void *data) {
	struct tw_server *server = wl_container_of(listener, server, pinch_end);
	struct wlr_pointer_pinch_end_event *event = data;
	wlr_pointer_gestures_v1_send_pinch_end(server->pointer_gestures, server->seat,
		event->time_msec, event->cancelled);
}

static void handle_hold_begin(struct wl_listener *listener, void *data) {
	struct tw_server *server = wl_container_of(listener, server, hold_begin);
	struct wlr_pointer_hold_begin_event *event = data;
	idle_activity(server);
	wlr_pointer_gestures_v1_send_hold_begin(server->pointer_gestures, server->seat,
		event->time_msec, event->fingers);
}

static void handle_hold_end(struct wl_listener *listener, void *data) {
	struct tw_server *server = wl_container_of(listener, server, hold_end);
	struct wlr_pointer_hold_end_event *event = data;
	wlr_pointer_gestures_v1_send_hold_end(server->pointer_gestures, server->seat,
		event->time_msec, event->cancelled);
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
	pointer_constraint_update(server);
}

static void handle_request_set_selection(struct wl_listener *listener, void *data) {
	struct tw_server *server = wl_container_of(listener, server, request_set_selection);
	struct wlr_seat_request_set_selection_event *event = data;
	if (event->source == NULL) {
		clipboard_cleared(server);
	}
	wlr_seat_set_selection(server->seat, event->source, event->serial);
}

static void handle_request_set_primary_selection(struct wl_listener *listener, void *data) {
	/* Select-to-copy, middle-click-to-paste. */
	struct tw_server *server =
		wl_container_of(listener, server, request_set_primary_selection);
	struct wlr_seat_request_set_primary_selection_event *event = data;
	wlr_seat_set_primary_selection(server->seat, event->source, event->serial);
}

static void handle_request_set_shape(struct wl_listener *listener, void *data) {
	/* A client names a cursor ("text", "pointer") instead of sending an
	 * image, so it does not have to load a cursor theme of its own. */
	struct tw_server *server = wl_container_of(listener, server, request_set_shape);
	struct wlr_cursor_shape_manager_v1_request_set_shape_event *event = data;
	if (event->device_type == WLR_CURSOR_SHAPE_MANAGER_V1_DEVICE_TYPE_POINTER &&
			server->seat->pointer_state.focused_client == event->seat_client) {
		wlr_cursor_set_xcursor(server->cursor, server->cursor_manager,
			wlr_cursor_shape_v1_name(event->shape));
	}
}

void input_init(struct tw_server *server) {

	server->cursor = wlr_cursor_create();
	wlr_cursor_attach_output_layout(server->cursor, server->output_layout);
	/* The theme and size programs are told, so that the pointer looks the
	 * same over a window as over the empty desktop. */
	const char *size = getenv("XCURSOR_SIZE");
	int cursor_size = size != NULL ? atoi(size) : 0;
	server->cursor_manager = wlr_xcursor_manager_create(getenv("XCURSOR_THEME"),
		cursor_size > 0 ? (uint32_t)cursor_size : 24);

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

	server->pointer_gestures = wlr_pointer_gestures_v1_create(server->display);
	server->swipe_begin.notify = handle_swipe_begin;
	wl_signal_add(&server->cursor->events.swipe_begin, &server->swipe_begin);
	server->swipe_update.notify = handle_swipe_update;
	wl_signal_add(&server->cursor->events.swipe_update, &server->swipe_update);
	server->swipe_end.notify = handle_swipe_end;
	wl_signal_add(&server->cursor->events.swipe_end, &server->swipe_end);
	server->pinch_begin.notify = handle_pinch_begin;
	wl_signal_add(&server->cursor->events.pinch_begin, &server->pinch_begin);
	server->pinch_update.notify = handle_pinch_update;
	wl_signal_add(&server->cursor->events.pinch_update, &server->pinch_update);
	server->pinch_end.notify = handle_pinch_end;
	wl_signal_add(&server->cursor->events.pinch_end, &server->pinch_end);
	server->hold_begin.notify = handle_hold_begin;
	wl_signal_add(&server->cursor->events.hold_begin, &server->hold_begin);
	server->hold_end.notify = handle_hold_end;
	wl_signal_add(&server->cursor->events.hold_end, &server->hold_end);

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

	wlr_primary_selection_v1_device_manager_create(server->display);
	server->request_set_primary_selection.notify = handle_request_set_primary_selection;
	wl_signal_add(&server->seat->events.request_set_primary_selection,
		&server->request_set_primary_selection);

	server->cursor_shape_manager = wlr_cursor_shape_manager_v1_create(server->display, 1);
	server->request_set_shape.notify = handle_request_set_shape;
	wl_signal_add(&server->cursor_shape_manager->events.request_set_shape,
		&server->request_set_shape);

	server->new_input.notify = handle_new_input;
	wl_signal_add(&server->backend->events.new_input, &server->new_input);

	server->virtual_keyboard_manager = wlr_virtual_keyboard_manager_v1_create(server->display);
	server->new_virtual_keyboard.notify = handle_new_virtual_keyboard;
	wl_signal_add(&server->virtual_keyboard_manager->events.new_virtual_keyboard,
		&server->new_virtual_keyboard);
	server->virtual_pointer_manager = wlr_virtual_pointer_manager_v1_create(server->display);
	server->new_virtual_pointer.notify = handle_new_virtual_pointer;
	wl_signal_add(&server->virtual_pointer_manager->events.new_virtual_pointer,
		&server->new_virtual_pointer);

	/* Mouse-look in games and 3D editors (Godot, Blender), also for X11
	 * programs, whose pointer grabs Xwayland turns into these. */
	server->relative_pointer_manager = wlr_relative_pointer_manager_v1_create(server->display);
	server->pointer_constraints = wlr_pointer_constraints_v1_create(server->display);
	server->new_constraint.notify = handle_new_constraint;
	wl_signal_add(&server->pointer_constraints->events.new_constraint, &server->new_constraint);
}

void input_finish(struct tw_server *server) {
	wl_list_remove(&server->new_input.link);
	wl_list_remove(&server->new_virtual_keyboard.link);
	wl_list_remove(&server->new_virtual_pointer.link);
	wl_list_remove(&server->new_constraint.link);
	wl_list_remove(&server->cursor_motion.link);
	wl_list_remove(&server->cursor_motion_absolute.link);
	wl_list_remove(&server->cursor_button.link);
	wl_list_remove(&server->cursor_axis.link);
	wl_list_remove(&server->cursor_frame.link);
	wl_list_remove(&server->swipe_begin.link);
	wl_list_remove(&server->swipe_update.link);
	wl_list_remove(&server->swipe_end.link);
	wl_list_remove(&server->pinch_begin.link);
	wl_list_remove(&server->pinch_update.link);
	wl_list_remove(&server->pinch_end.link);
	wl_list_remove(&server->hold_begin.link);
	wl_list_remove(&server->hold_end.link);
	wl_list_remove(&server->request_set_cursor.link);
	wl_list_remove(&server->pointer_focus_change.link);
	wl_list_remove(&server->request_set_selection.link);
	wl_list_remove(&server->request_set_primary_selection.link);
	wl_list_remove(&server->request_set_shape.link);
	wlr_xcursor_manager_destroy(server->cursor_manager);
	wlr_cursor_destroy(server->cursor);
	/* Outputs going away later in the teardown still arrange the strip. */
	server->cursor_manager = NULL;
	server->cursor = NULL;
}
