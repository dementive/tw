/*
 * Minimal Wayland client for the headless tests. It reports what the
 * compositor tells it, one line per event:
 *
 *   configure <w>x<h> activated=<0|1> suspended=<0|1> fullscreen=<0|1> tiled=<0|1>
 *   capabilities <name...>                              what the window may ask
 *                      for besides moving and resizing: fullscreen, maximize,
 *                      minimize, menu
 *   layer configure <w>x<h>                             with --layer, --bar
 *   scale <n>                                           with --layer: the
 *                      screen's scale in 120ths, as soon as it is told
 *   lock configure <w>x<h>, locked, lock denied         with --lock
 *   keyboard enter | keyboard leave                     keyboard focus; "with
 *                      <n> keys held" if it is told keys are down as it enters
 *   key <code> <pressed|released>                       keys it received
 *   pointer enter <x> <y> | pointer leave               pointer focus; "pointer
 *                      enter menu <x> <y>" when it enters the menu (--menu)
 *   output enter <name> | output leave <name>           which monitor shows it
 *   pointer motion <x> <y>, button <code> <pressed|released>    (motion when its
 *                      wl_pointer.frame arrives)
 *   closed                                              asked to close; it exits
 *   token, activate, activate forged                    see the focus modes below
 *   x11 connected, x11 served, x11 closed               with --x11
 *
 * Modes:
 *   (none)        one ordinary window
 *   --fullscreen  a window that asks to start fullscreen
 *   --fixed [<w>x<h>]  a window that cannot be resized, like a dialog;
 *                 300x200 unless given
 *   --shadow      the 300x200 dialog of --fixed drawing a 20-pixel shadow
 *                 around its window geometry while it is not told it is
 *                 tiled, as GTK does
 *   --game        a 300x200 window that cannot be resized, like a game, and
 *                 that asks for fullscreen or to leave it when F is pressed
 *   --fickle      the same game asking to start fullscreen, and changing its
 *                 mind before it draws its first frame
 *   --remap       an ordinary window that, the first time it is suspended,
 *                 takes its window down ("unmapped") and puts it back up as
 *                 one that cannot be resized, like a dialog
 *   --remap-on-leave [<w>x<h>]  a dialog (as --fixed) that, the first time
 *                 it is told it lost the focus, takes its window down and
 *                 puts it back up at once ("unmapped"), as a program does
 *                 that hides a dialog and shows it again
 *   --game-late   the game, asking for fullscreen the first time it loses
 *                 the keyboard ("ask fullscreen"), as one does that goes
 *                 fullscreen when its loading is done
 *   --inhibit     a window that asks to keep the screen on, like a video player
 *   --layer       a 400x200 layer surface on the overlay layer wanting
 *                 exclusive keyboard focus, like a launcher
 *   --layer-inhibit  the same, asking to keep the screen on
 *   --bar         a layer surface 30 pixels high across the top of the
 *                 screen that keeps those 30 pixels to itself, like a bar
 *   --bar-menu    the same bar on the bottom layer, as waybar's default is,
 *                 that opens a 100x50 menu at 100,30 hanging into the room
 *                 below it once it is configured: "menu shown"
 *   --lock        a screen locker; pressing Return unlocks ("unlocked")
 *   --drag        a window that starts dragging text each time a button is
 *                 pressed in it: "drag started", then "drag dropped" and
 *                 "drag finished", or "drag cancelled"
 *   --drop        a window that takes what is dragged onto it: "drop enter",
 *                 "drop leave", "drop"
 *   --menu        a window that opens a menu holding the keyboard and the
 *                 pointer (a popup grab) when it first gets the keyboard:
 *                 "menu shown", and "menu closed" when the compositor ends it.
 *                 Once shown, the menu asks to be moved far off to the right,
 *                 as a menu that follows what it belongs to does; "menu at
 *                 <x> <y>" for each place it is given, relative to the window
 *   --copy-late   a window that, the first time it loses the keyboard, copies
 *                 "copied late" with the serial of the keyboard focus it had,
 *                 as a program does that copies when an action started
 *                 earlier is done: "copied", and "copy cancelled" when
 *                 another copy takes its place
 *
 * Asking for the focus (xdg-activation):
 *   --token <file>     a window that, when it gets the keyboard, turns that
 *                      event into an activation token and writes it to <file>
 *   --activate <file>  a window that, the first time it loses the focus, asks
 *                      for it back with the token in <file>
 *   --activate-forged  the same, but with a token it made up without any
 *                      input event, which must not work
 *   --token-no-surface <file>  as --token, but the token does not say which
 *                      surface had the event, which must not work either
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
 * Animating, after any window mode:
 *   --animate     the window draws a new frame each time it is told the last
 *                 one was shown, as a game or a spinner does: "frame" for
 *                 each, so the frames a second can be counted
 *
 * Without --animate it draws once per configure and never asks for frame
 * callbacks, so an idle compositor with these clients mapped should not wake
 * up at all.
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
#include "fractional-scale-v1-client-protocol.h"
#include "idle-inhibit-unstable-v1-client-protocol.h"
#include "pointer-constraints-unstable-v1-client-protocol.h"
#include "relative-pointer-unstable-v1-client-protocol.h"
#include "wlr-layer-shell-unstable-v1-client-protocol.h"
#include "xdg-activation-v1-client-protocol.h"
#include "xdg-shell-client-protocol.h"

#define KEY_RETURN 28 /* evdev codes of the Return and F keys */
#define KEY_F 33

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
static struct wp_fractional_scale_manager_v1 *scale_manager;
static struct wl_pointer *pointer;
static struct zwp_locked_pointer_v1 *locked_pointer;
static struct ext_session_lock_v1 *session_lock;
static struct wl_surface *surface;
static struct wl_buffer *buffer;
static struct xdg_surface *window_xdg_surface;
static bool open_menu, menu_opened, bar_menu;
static struct wl_surface *menu_surface;
static struct wl_buffer *menu_buffer;

/* Every monitor, to report by name which ones show the surface. */
static struct {
	struct wl_output *output;
	char name[32];
} outputs[4];
static size_t output_count;

static int32_t pending_width, pending_height;
static bool pending_activated, pending_suspended, pending_fullscreen, pending_tiled;
static bool fixed_size, game, fickle, remap, remap_on_leave, game_late, shadow;
static int32_t fixed_width = 300, fixed_height = 200;
static struct xdg_toplevel *window_toplevel;
static bool running = true;

/* Focus modes. */
static const char *token_path;
static bool issue_token, use_token, forge_token, token_without_surface;
static bool token_requested, was_activated, activation_sent;

/* Pointer modes. */
static bool lock_pointer, confine_pointer, constraint_requested;

/* Drag and drop. */
static struct wl_data_device_manager *data_manager;
static struct wl_data_device *data_device;
static struct wl_data_offer *drop_offer;
static bool drag_source, drop_target;

/* --copy-late: the serial of the latest keyboard enter. */
static bool copy_late, copied;
static uint32_t enter_serial;

static void say(const char *line) {
	puts(line);
	fflush(stdout);
}

static void draw_on(struct wl_surface *target, struct wl_buffer **current,
		int32_t width, int32_t height) {
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
	struct wl_buffer *old = *current;
	*current = wl_shm_pool_create_buffer(pool, 0, width, height, stride, WL_SHM_FORMAT_XRGB8888);
	wl_shm_pool_destroy(pool);
	close(fd);

	wl_surface_attach(target, *current, 0, 0);
	wl_surface_damage_buffer(target, 0, 0, width, height);
	wl_surface_commit(target);
	if (old != NULL) {
		wl_buffer_destroy(old);
	}
}

/* --animate: draws again at every frame callback, at the last size. */
static bool animate;
static struct wl_callback *frame_callback;
static int32_t drawn_width, drawn_height;

static void draw(int32_t width, int32_t height);

static void frame_done(void *data, struct wl_callback *callback, uint32_t time) {
	wl_callback_destroy(callback);
	frame_callback = NULL;
	say("frame");
	draw(drawn_width, drawn_height);
}

static const struct wl_callback_listener frame_listener = {.done = frame_done};

static void draw(int32_t width, int32_t height) {
	drawn_width = width;
	drawn_height = height;
	if (animate && frame_callback == NULL) {
		frame_callback = wl_surface_frame(surface);
		wl_callback_add_listener(frame_callback, &frame_listener, NULL);
	}
	draw_on(surface, &buffer, width, height);
}

/* --- A menu ----------------------------------------------------------------- */

static struct xdg_positioner *menu_positioner(int32_t x) {
	struct xdg_positioner *positioner = xdg_wm_base_create_positioner(wm_base);
	xdg_positioner_set_size(positioner, 100, 50);
	xdg_positioner_set_anchor_rect(positioner, x, 10, 1, 1);
	xdg_positioner_set_constraint_adjustment(positioner,
		XDG_POSITIONER_CONSTRAINT_ADJUSTMENT_SLIDE_X |
		XDG_POSITIONER_CONSTRAINT_ADJUSTMENT_SLIDE_Y);
	return positioner;
}

static void menu_configure(void *data, struct xdg_surface *xdg_surface, uint32_t serial) {
	static bool moved;
	xdg_surface_ack_configure(xdg_surface, serial);
	draw_on(menu_surface, &menu_buffer, 100, 50);
	say("menu shown");
	if (open_menu && !moved) {
		moved = true;
		struct xdg_positioner *positioner = menu_positioner(5000);
		xdg_popup_reposition(data, positioner, 1);
		xdg_positioner_destroy(positioner);
	}
}

static const struct xdg_surface_listener menu_surface_listener = {
	.configure = menu_configure,
};

static void popup_configure(void *data, struct xdg_popup *popup, int32_t x, int32_t y,
		int32_t width, int32_t height) {
	printf("menu at %d %d\n", x, y);
	fflush(stdout);
}

static void popup_done(void *data, struct xdg_popup *popup) {
	say("menu closed");
}

static void popup_repositioned(void *data, struct xdg_popup *popup, uint32_t token) {
}

static const struct xdg_popup_listener popup_listener = {
	.configure = popup_configure,
	.popup_done = popup_done,
	.repositioned = popup_repositioned,
};

/* A menu as a right click opens one: it takes the keyboard and the pointer
 * until it is closed. `serial` is that of the input event behind it. */
static void menu_open(uint32_t serial) {
	struct xdg_positioner *positioner = menu_positioner(10);
	menu_surface = wl_compositor_create_surface(compositor);
	struct xdg_surface *xdg_surface = xdg_wm_base_get_xdg_surface(wm_base, menu_surface);
	struct xdg_popup *popup = xdg_surface_get_popup(xdg_surface, window_xdg_surface, positioner);
	xdg_surface_add_listener(xdg_surface, &menu_surface_listener, popup);
	xdg_popup_add_listener(popup, &popup_listener, NULL);
	xdg_popup_grab(popup, seat, serial);
	xdg_positioner_destroy(positioner);
	wl_surface_commit(menu_surface);
}

/* A bar's menu, without a grab, as one that opens on hovering does. */
static void bar_menu_open(struct zwlr_layer_surface_v1 *layer_surface) {
	struct xdg_positioner *positioner = xdg_wm_base_create_positioner(wm_base);
	xdg_positioner_set_size(positioner, 100, 50);
	xdg_positioner_set_anchor_rect(positioner, 100, 0, 1, 30);
	xdg_positioner_set_anchor(positioner, XDG_POSITIONER_ANCHOR_BOTTOM_LEFT);
	xdg_positioner_set_gravity(positioner, XDG_POSITIONER_GRAVITY_BOTTOM_RIGHT);
	menu_surface = wl_compositor_create_surface(compositor);
	struct xdg_surface *xdg_surface = xdg_wm_base_get_xdg_surface(wm_base, menu_surface);
	struct xdg_popup *popup = xdg_surface_get_popup(xdg_surface, NULL, positioner);
	xdg_surface_add_listener(xdg_surface, &menu_surface_listener, popup);
	xdg_popup_add_listener(popup, &popup_listener, NULL);
	zwlr_layer_surface_v1_get_popup(layer_surface, popup);
	xdg_positioner_destroy(positioner);
	wl_surface_commit(menu_surface);
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
		if (!token_without_surface) {
			xdg_activation_token_v1_set_surface(token, surface);
		}
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
	if (fickle) {
		fickle = false;
		xdg_toplevel_unset_fullscreen(window_toplevel);
		say("leave fullscreen");
	}
	printf("configure %dx%d activated=%d suspended=%d fullscreen=%d tiled=%d\n",
		pending_width, pending_height, pending_activated, pending_suspended,
		pending_fullscreen, pending_tiled);
	fflush(stdout);
	/* Unmapped here, once this configure is acked: one acked after the window
	 * started over would be refused. */
	if ((remap && pending_suspended) ||
			(remap_on_leave && was_activated && !pending_activated)) {
		/* A null buffer unmaps it; the next commit starts it over. */
		remap = remap_on_leave = false;
		wl_surface_attach(surface, NULL, 0, 0);
		wl_surface_commit(surface);
		fixed_size = true;
		xdg_toplevel_set_min_size(window_toplevel, fixed_width, fixed_height);
		xdg_toplevel_set_max_size(window_toplevel, fixed_width, fixed_height);
		wl_surface_commit(surface);
		say("unmapped");
		return;
	}
	if (pending_activated) {
		was_activated = true;
	} else if (was_activated && !activation_sent && (use_token || forge_token)) {
		/* Just lost the focus: ask for it back, once. */
		activation_sent = true;
		ask_for_focus();
	}
	/* 0x0 means "pick your own size". */
	int32_t own_width = fixed_size ? fixed_width : 640;
	int32_t own_height = fixed_size ? fixed_height : 480;
	int32_t width = pending_width > 0 ? pending_width : own_width;
	int32_t height = pending_height > 0 ? pending_height : own_height;
	if (shadow) {
		/* The window is the geometry; the shadow is drawn around it. */
		int32_t margin = pending_tiled ? 0 : 20;
		xdg_surface_set_window_geometry(window_xdg_surface, margin, margin, width, height);
		width += 2 * margin;
		height += 2 * margin;
	}
	draw(width, height);
}

static const struct xdg_surface_listener xdg_surface_listener = {
	.configure = xdg_surface_configure,
};

static void toplevel_configure(void *data, struct xdg_toplevel *toplevel,
		int32_t width, int32_t height, struct wl_array *states) {
	pending_width = width;
	pending_height = height;
	pending_activated = pending_suspended = pending_fullscreen = pending_tiled = false;
	uint32_t *state;
	wl_array_for_each(state, states) {
		if (*state == XDG_TOPLEVEL_STATE_ACTIVATED) {
			pending_activated = true;
		} else if (*state == XDG_TOPLEVEL_STATE_SUSPENDED) {
			pending_suspended = true;
		} else if (*state == XDG_TOPLEVEL_STATE_FULLSCREEN) {
			pending_fullscreen = true;
		} else if (*state == XDG_TOPLEVEL_STATE_TILED_LEFT ||
				*state == XDG_TOPLEVEL_STATE_TILED_RIGHT ||
				*state == XDG_TOPLEVEL_STATE_TILED_TOP ||
				*state == XDG_TOPLEVEL_STATE_TILED_BOTTOM) {
			pending_tiled = true;
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
	static const char *const names[] = {
		[XDG_TOPLEVEL_WM_CAPABILITIES_WINDOW_MENU] = "menu",
		[XDG_TOPLEVEL_WM_CAPABILITIES_MAXIMIZE] = "maximize",
		[XDG_TOPLEVEL_WM_CAPABILITIES_FULLSCREEN] = "fullscreen",
		[XDG_TOPLEVEL_WM_CAPABILITIES_MINIMIZE] = "minimize",
	};
	uint32_t *capability;
	printf("capabilities");
	wl_array_for_each(capability, capabilities) {
		printf(" %s", *capability < sizeof(names) / sizeof(names[0]) &&
			names[*capability] != NULL ? names[*capability] : "unknown");
	}
	printf("\n");
	fflush(stdout);
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
	if (bar_menu && !menu_opened) {
		menu_opened = true;
		bar_menu_open(layer_surface);
	}
}

static void layer_closed(void *data, struct zwlr_layer_surface_v1 *layer_surface) {
	running = false;
}

static void scale_preferred(void *data, struct wp_fractional_scale_v1 *fractional,
		uint32_t scale) {
	printf("scale %u\n", scale);
	fflush(stdout);
}

static const struct wp_fractional_scale_v1_listener scale_listener = {
	.preferred_scale = scale_preferred,
};

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
	if (keys->size > 0) {
		printf("keyboard enter with %zu keys held\n", keys->size / sizeof(uint32_t));
		fflush(stdout);
	} else {
		say("keyboard enter");
	}
	if (open_menu && !menu_opened) {
		menu_opened = true;
		menu_open(serial);
	}
	if (issue_token && !token_requested) {
		token_requested = true;
		request_token(serial);
	}
	enter_serial = serial;
}

static void copy_start(void);

static void keyboard_leave(void *data, struct wl_keyboard *wl_keyboard, uint32_t serial,
		struct wl_surface *wl_surface) {
	say("keyboard leave");
	if (game_late) {
		game_late = false;
		xdg_toplevel_set_fullscreen(window_toplevel, NULL);
		say("ask fullscreen");
	}
	if (copy_late && !copied) {
		copied = true;
		copy_start();
	}
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
	if (game && key == KEY_F && pressed) {
		/* A game's own fullscreen key, as F11 is in many. */
		if (pending_fullscreen) {
			xdg_toplevel_unset_fullscreen(window_toplevel);
			say("leave fullscreen");
		} else {
			xdg_toplevel_set_fullscreen(window_toplevel, NULL);
			say("ask fullscreen");
		}
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
	printf("pointer enter %s%d %d\n", wl_surface != NULL && wl_surface == menu_surface ?
		"menu " : "", wl_fixed_to_int(x), wl_fixed_to_int(y));
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

/* Motion is said at the frame that closes it, as toolkits act on it: one
 * that never gets its frame never shows up. */
static bool motion_pending;
static wl_fixed_t motion_x, motion_y;

static void pointer_motion(void *data, struct wl_pointer *wl_pointer, uint32_t time,
		wl_fixed_t x, wl_fixed_t y) {
	motion_pending = true;
	motion_x = x;
	motion_y = y;
}

static void pointer_frame(void *data, struct wl_pointer *wl_pointer) {
	if (motion_pending) {
		motion_pending = false;
		printf("pointer motion %d %d\n", wl_fixed_to_int(motion_x), wl_fixed_to_int(motion_y));
		fflush(stdout);
	}
}

static void pointer_axis_source(void *data, struct wl_pointer *wl_pointer, uint32_t source) {
}

static void pointer_axis_stop(void *data, struct wl_pointer *wl_pointer, uint32_t time,
		uint32_t axis) {
}

static void pointer_axis_discrete(void *data, struct wl_pointer *wl_pointer, uint32_t axis,
		int32_t discrete) {
}

/* --- Drag and drop ---------------------------------------------------------- */

static void source_target(void *data, struct wl_data_source *source, const char *mime_type) {
}

static void source_send(void *data, struct wl_data_source *source, const char *mime_type,
		int32_t fd) {
	if (write(fd, "dragged", 7) < 0) {
		perror("drag send");
	}
	close(fd);
}

static void source_cancelled(void *data, struct wl_data_source *source) {
	say("drag cancelled");
	wl_data_source_destroy(source);
}

static void source_drop_performed(void *data, struct wl_data_source *source) {
	say("drag dropped");
}

static void source_finished(void *data, struct wl_data_source *source) {
	say("drag finished");
	wl_data_source_destroy(source);
}

static void source_action(void *data, struct wl_data_source *source, uint32_t action) {
}

static const struct wl_data_source_listener source_listener = {
	.target = source_target,
	.send = source_send,
	.cancelled = source_cancelled,
	.dnd_drop_performed = source_drop_performed,
	.dnd_finished = source_finished,
	.action = source_action,
};

/* --- Copying ---------------------------------------------------------------- */

static void copy_send(void *data, struct wl_data_source *source, const char *mime_type,
		int32_t fd) {
	if (write(fd, "copied late", 11) < 0) {
		perror("copy send");
	}
	close(fd);
}

static void copy_cancelled(void *data, struct wl_data_source *source) {
	say("copy cancelled");
	wl_data_source_destroy(source);
}

static const struct wl_data_source_listener copy_listener = {
	.target = source_target,
	.send = copy_send,
	.cancelled = copy_cancelled,
	.dnd_drop_performed = source_drop_performed,
	.dnd_finished = source_finished,
	.action = source_action,
};

/* Copies with the serial of the keyboard focus it had, not of anything
 * newer it has been sent since. */
static void copy_start(void) {
	struct wl_data_source *source = wl_data_device_manager_create_data_source(data_manager);
	wl_data_source_add_listener(source, &copy_listener, NULL);
	wl_data_source_offer(source, "text/plain");
	wl_data_device_set_selection(data_device, source, enter_serial);
	say("copied");
}

/* Starts dragging from a button press, as a file manager does. */
static void drag_start(uint32_t serial) {
	struct wl_data_source *source = wl_data_device_manager_create_data_source(data_manager);
	wl_data_source_add_listener(source, &source_listener, NULL);
	wl_data_source_offer(source, "text/plain");
	wl_data_source_set_actions(source, WL_DATA_DEVICE_MANAGER_DND_ACTION_COPY);
	wl_data_device_start_drag(data_device, source, surface, NULL, serial);
	say("drag started");
}

static void device_data_offer(void *data, struct wl_data_device *device,
		struct wl_data_offer *offer) {
}

static void device_enter(void *data, struct wl_data_device *device, uint32_t serial,
		struct wl_surface *target, wl_fixed_t x, wl_fixed_t y, struct wl_data_offer *offer) {
	say("drop enter");
	drop_offer = offer;
	if (offer != NULL && drop_target) {
		wl_data_offer_accept(offer, serial, "text/plain");
		wl_data_offer_set_actions(offer, WL_DATA_DEVICE_MANAGER_DND_ACTION_COPY,
			WL_DATA_DEVICE_MANAGER_DND_ACTION_COPY);
	}
}

static void device_leave(void *data, struct wl_data_device *device) {
	say("drop leave");
	if (drop_offer != NULL) {
		wl_data_offer_destroy(drop_offer);
		drop_offer = NULL;
	}
}

static void device_motion(void *data, struct wl_data_device *device, uint32_t time,
		wl_fixed_t x, wl_fixed_t y) {
}

static void device_drop(void *data, struct wl_data_device *device) {
	say("drop");
	if (drop_offer != NULL) {
		wl_data_offer_finish(drop_offer);
		wl_data_offer_destroy(drop_offer);
		drop_offer = NULL;
	}
}

static void device_selection(void *data, struct wl_data_device *device,
		struct wl_data_offer *offer) {
	if (offer != NULL) {
		wl_data_offer_destroy(offer);
	}
}

static const struct wl_data_device_listener device_listener = {
	.data_offer = device_data_offer,
	.enter = device_enter,
	.leave = device_leave,
	.motion = device_motion,
	.drop = device_drop,
	.selection = device_selection,
};

static void pointer_button(void *data, struct wl_pointer *wl_pointer, uint32_t serial,
		uint32_t time, uint32_t button, uint32_t state) {
	printf("button %u %s\n", button,
		state == WL_POINTER_BUTTON_STATE_PRESSED ? "pressed" : "released");
	fflush(stdout);
	if (drag_source && state == WL_POINTER_BUTTON_STATE_PRESSED) {
		drag_start(serial);
	}
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
	.frame = pointer_frame,
	.axis_source = pointer_axis_source,
	.axis_stop = pointer_axis_stop,
	.axis_discrete = pointer_axis_discrete,
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
		/* 5: pointer frames, as toolkits use them (pointer_frame()). */
		seat = wl_registry_bind(registry, name, &wl_seat_interface, at_most(version, 5));
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
	} else if (strcmp(interface, wp_fractional_scale_manager_v1_interface.name) == 0) {
		scale_manager = wl_registry_bind(registry, name,
			&wp_fractional_scale_manager_v1_interface, 1);
	} else if (strcmp(interface, wl_data_device_manager_interface.name) == 0 && version >= 3) {
		/* Version 3 is the first with drag-and-drop actions. */
		data_manager = wl_registry_bind(registry, name, &wl_data_device_manager_interface, 3);
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

	/* The listening sockets first, then the clients that are connected. */
	struct pollfd sockets[2 + 16];
	nfds_t listening = 0;
	bool stranger = false;
	/* tw ignores SIGPIPE; the programs it starts must not inherit that. */
	struct sigaction on_pipe;
	if (sigaction(SIGPIPE, NULL, &on_pipe) == 0 && on_pipe.sa_handler == SIG_IGN) {
		printf("ignores SIGPIPE\n");
	}
	printf("serve pid=%d", (int)getpid());
	for (int i = 0; i < argc; i++) {
		printf(" %s", argv[i]);
		if (strcmp(argv[i], "-listenfd") == 0 && i + 1 < argc && listening < 2) {
			sockets[listening++] = (struct pollfd){.fd = atoi(argv[i + 1]), .events = POLLIN};
		}
		stranger = stranger || strcmp(argv[i], "-stranger") == 0;
	}
	say("");

	/* The real helper holds a connection with a name that is no X11 client:
	 * it runs a Wayland socket of its own for Xwayland. So do we. */
	struct sockaddr_un own = {.sun_family = AF_UNIX};
	snprintf(own.sun_path, sizeof(own.sun_path), "%s/x11-serve-%d",
		getenv("XDG_RUNTIME_DIR"), (int)getpid());
	int own_server = socket(AF_UNIX, SOCK_STREAM, 0);
	int own_client = socket(AF_UNIX, SOCK_STREAM, 0);
	if (bind(own_server, (struct sockaddr *)&own, sizeof(own)) < 0 ||
			listen(own_server, 1) < 0 ||
			connect(own_client, (struct sockaddr *)&own, sizeof(own)) < 0 ||
			accept(own_server, NULL, NULL) < 0) {
		say("serve error");
		return 1;
	}
	unlink(own.sun_path);
	/* -stranger: also a socket that is in no list of Unix sockets, as a
	 * connection from a sandbox with a network of its own would be. */
	if (stranger && socket(AF_INET, SOCK_DGRAM, 0) < 0) {
		say("serve error");
		return 1;
	}

	/* Xwayland takes a while to come up. A compositor that kept watching the
	 * sockets meanwhile would start a second helper for the same client. */
	nanosleep(&(struct timespec){.tv_nsec = 300000000}, NULL);

	nfds_t count = listening;
	while (poll(sockets, count, -1) > 0) {
		/* Like an X server, let go of a client that has hung up. */
		for (nfds_t i = count; i-- > listening;) {
			char byte;
			if (sockets[i].revents != 0 && read(sockets[i].fd, &byte, 1) <= 0) {
				close(sockets[i].fd);
				sockets[i] = sockets[--count];
				say("hung up");
			}
		}
		for (nfds_t i = 0; i < listening; i++) {
			if (!(sockets[i].revents & POLLIN)) {
				continue;
			}
			/* Kept open: the client stays connected until it or we exit. */
			int client = accept(sockets[i].fd, NULL, NULL);
			if (client < 0 || count == sizeof(sockets) / sizeof(sockets[0]) ||
					write(client, "x", 1) != 1) {
				say("serve error");
				return 1;
			}
			sockets[count++] = (struct pollfd){.fd = client, .events = POLLIN};
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
	animate = argc > 1 && strcmp(argv[argc - 1], "--animate") == 0;
	token_without_surface = strcmp(mode, "--token-no-surface") == 0;
	issue_token = strcmp(mode, "--token") == 0 || token_without_surface;
	use_token = strcmp(mode, "--activate") == 0;
	forge_token = strcmp(mode, "--activate-forged") == 0;
	lock_pointer = strcmp(mode, "--lock-pointer") == 0;
	confine_pointer = strcmp(mode, "--confine-pointer") == 0;
	open_menu = strcmp(mode, "--menu") == 0;
	drag_source = strcmp(mode, "--drag") == 0;
	drop_target = strcmp(mode, "--drop") == 0;
	copy_late = strcmp(mode, "--copy-late") == 0;

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
	if (drag_source || drop_target || copy_late) {
		if (data_manager == NULL) {
			fprintf(stderr, "compositor has no data device manager\n");
			return 1;
		}
		data_device = wl_data_device_manager_get_data_device(data_manager, seat);
		wl_data_device_add_listener(data_device, &device_listener, NULL);
	}

	surface = wl_compositor_create_surface(compositor);
	wl_surface_add_listener(surface, &surface_listener, NULL);
	if (strcmp(mode, "--layer") == 0 || strcmp(mode, "--layer-inhibit") == 0) {
		if (strcmp(mode, "--layer-inhibit") == 0) {
			zwp_idle_inhibit_manager_v1_create_inhibitor(inhibit_manager, surface);
		}
		struct zwlr_layer_surface_v1 *layer_surface = zwlr_layer_shell_v1_get_layer_surface(
			layer_shell, surface, NULL, ZWLR_LAYER_SHELL_V1_LAYER_OVERLAY, "tw-test");
		zwlr_layer_surface_v1_add_listener(layer_surface, &layer_listener, NULL);
		zwlr_layer_surface_v1_set_size(layer_surface, 400, 200);
		zwlr_layer_surface_v1_set_keyboard_interactivity(layer_surface,
			ZWLR_LAYER_SURFACE_V1_KEYBOARD_INTERACTIVITY_EXCLUSIVE);
		if (scale_manager != NULL) {
			wp_fractional_scale_v1_add_listener(
				wp_fractional_scale_manager_v1_get_fractional_scale(scale_manager, surface),
				&scale_listener, NULL);
		}
		wl_surface_commit(surface);
	} else if (strcmp(mode, "--bar") == 0 || strcmp(mode, "--bar-menu") == 0) {
		bar_menu = strcmp(mode, "--bar-menu") == 0;
		struct zwlr_layer_surface_v1 *layer_surface = zwlr_layer_shell_v1_get_layer_surface(
			layer_shell, surface, NULL, bar_menu ? ZWLR_LAYER_SHELL_V1_LAYER_BOTTOM :
				ZWLR_LAYER_SHELL_V1_LAYER_TOP, "tw-test-bar");
		zwlr_layer_surface_v1_add_listener(layer_surface, &layer_listener, NULL);
		zwlr_layer_surface_v1_set_anchor(layer_surface, ZWLR_LAYER_SURFACE_V1_ANCHOR_TOP |
			ZWLR_LAYER_SURFACE_V1_ANCHOR_LEFT | ZWLR_LAYER_SURFACE_V1_ANCHOR_RIGHT);
		zwlr_layer_surface_v1_set_size(layer_surface, 0, 30);
		zwlr_layer_surface_v1_set_exclusive_zone(layer_surface, 30);
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
		window_xdg_surface = xdg_surface;
		xdg_surface_add_listener(xdg_surface, &xdg_surface_listener, NULL);
		struct xdg_toplevel *toplevel = xdg_surface_get_toplevel(xdg_surface);
		window_toplevel = toplevel;
		xdg_toplevel_add_listener(toplevel, &toplevel_listener, NULL);
		xdg_toplevel_set_title(toplevel, "tw-test-client");
		fickle = strcmp(mode, "--fickle") == 0;
		game_late = strcmp(mode, "--game-late") == 0;
		game = strcmp(mode, "--game") == 0 || fickle || game_late;
		remap = strcmp(mode, "--remap") == 0;
		remap_on_leave = strcmp(mode, "--remap-on-leave") == 0;
		shadow = strcmp(mode, "--shadow") == 0;
		if (fickle) {
			xdg_toplevel_set_fullscreen(toplevel, NULL);
		}
		if (strcmp(mode, "--fixed") == 0 || game || remap_on_leave || shadow) {
			fixed_size = true;
			if (argc > 2 && sscanf(argv[2], "%dx%d", &fixed_width, &fixed_height) != 2) {
				fprintf(stderr, "--fixed takes <width>x<height>\n");
				return 1;
			}
			xdg_toplevel_set_min_size(toplevel, fixed_width, fixed_height);
			xdg_toplevel_set_max_size(toplevel, fixed_width, fixed_height);
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
