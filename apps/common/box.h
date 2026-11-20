/*
 * The box tw's apps show: in the middle of the screen in use, on the overlay
 * layer, taking every key while it is open, of a size that never changes.
 * It draws through draw.h at the screen's own scale, and hands the app its
 * keys (repeated while held), clicks and the wheel. One box per program.
 */
#ifndef TW_BOX_H
#define TW_BOX_H

#include <stdbool.h>
#include <stdint.h>
#include <sys/types.h>
#include <xkbcommon/xkbcommon.h>

#include "draw.h"

struct wl_display;
struct wl_registry;

struct box_app {
	const char *name;  /* the layer surface's namespace ("launcher") */
	int width, height; /* in logical pixels */
	const char *font;  /* as fontconfig names one; opened with the first picture */
	/* Paints the whole box; box_px() gives the picture's pixels. */
	void (*draw)(struct picture *picture);
	/* A key was pressed, or repeats: its keysym, and the text it types
	 * ("" for none). */
	void (*key)(xkb_keysym_t sym, const char *text, bool ctrl, bool alt);
	/* The left button was pressed at x,y, in logical pixels. Optional. */
	void (*click)(int x, int y);
	/* The wheel turned over x,y: steps > 0 down, < 0 up. Optional. */
	void (*scroll)(int x, int y, int steps);
	/* A global the box does not use itself, for the app to bind. Optional. */
	void (*global)(struct wl_registry *registry, uint32_t name, const char *interface,
		uint32_t version);
	/* The compositor closed the box: the screen it was on went. Optional;
	 * without it the program leaves. */
	void (*closed)(void);
	/* The time box_timer() set has come. Optional. */
	void (*timer)(void);
};

/* Connects to the compositor and binds what the box needs, without showing
 * it; false without a compositor or one without the layer shell. With
 * `verbose` box and font say what they do on stderr. */
bool box_connect(const struct box_app *app, bool verbose);
/* Shows the box. */
void box_open(void);
/* The picture is to be painted anew. */
void box_redraw(void);
/* Logical pixels to the picture's, at the scale of the screen the box is on. */
int box_px(int logical);
/* Calls app->timer once after `ms`; 0 stops it. */
void box_timer(int ms);
/* Waits for the compositor's answers to everything sent so far. */
bool box_roundtrip(void);
/* Handles keys, clicks and the compositor until box_quit(). */
void box_run(void);
void box_quit(void);
/* Has the compositor make an activation token from the last key or click in
 * the box, and calls `done` with it (NULL if it cannot). */
void box_token(void (*done)(const char *token));
/* Lets go of the box and the connection. */
void box_disconnect(void);
/* Calls `ready` whenever `fd` can be read, while the box runs; NULL stops
 * watching it. A few at most. */
void box_watch(int fd, void (*ready)(void));
/*
 * One box of this name at a time: the key that opened it closes it again.
 * True if this program is the one; false if another was open, which is
 * ended. The first holds a lock on $XDG_RUNTIME_DIR/<name>.pid until it exits.
 */
bool box_only_one(const char *name, bool verbose);
/* The compositor's process, or 0. */
pid_t box_compositor_pid(void);

#endif
