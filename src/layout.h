/*
 * Scrolling-strip maths, and the decisions about refresh rate, battery and
 * lid that follow from a few numbers.
 *
 * Pure functions: no wlroots, no allocation, no global state, so they can be
 * unit tested without a compositor (tests/layout_test.c).
 *
 * Columns sit side by side on an infinite horizontal strip, separated by
 * `gap`, with the first column starting at x = gap. The output shows the part
 * of the strip starting at `view_x`. All values are logical pixels.
 */
#ifndef TW_LAYOUT_H
#define TW_LAYOUT_H

#include <stdbool.h>

struct tw_fraction {
	int num, den;
};

/*
 * Outer width of a column that takes fraction `f` of the output. Chosen so
 * that 1/f columns plus the gaps around them fill the output exactly.
 */
int tw_column_width(int view_width, int gap, struct tw_fraction f);

/*
 * Outer height of window `index` (from the top) of `count` windows stacked in
 * a column `avail` pixels tall with `gap` between them. The windows share the
 * height equally; the last one absorbs the rounding remainder, so the stack
 * always fills the column exactly.
 */
int tw_stack_height(int avail, int gap, int count, int index);

/*
 * New view_x after the smallest scroll that brings the column, plus a gap on
 * each side, fully into view. Returns view_x unchanged if it already is.
 */
int tw_view_fit(int view_x, int view_width, int col_x, int col_width, int gap);

/*
 * Keeps the view on the strip: never before its start, and never so far that
 * blank space shows past the last column while columns are hidden on the
 * left. `strip_width` is the whole strip including the gaps at both ends.
 */
int tw_view_clamp(int view_x, int view_width, int strip_width);

/* Whether any part of the column is on the output. */
bool tw_column_visible(int view_x, int view_width, int col_x, int col_width);

/*
 * Advances a critically damped spring by `dt` seconds: *pos moves toward
 * `target`, carrying on from whatever *vel (units per second) it had. From
 * rest it gets there as fast as it can without overshooting; flung toward
 * the target it may pass it once and come back. `omega` is sqrt(stiffness).
 * The step is the exact solution, so it is correct for any frame time, and a
 * retarget or a touchpad swipe that ends with some speed just continues from
 * that state.
 */
void tw_spring_step(double *pos, double *vel, double target, double omega, double dt);

/* Whether a spring is close enough to its target to stop animating. */
bool tw_spring_settled(double pos, double vel, double target);

/*
 * How fast a touchpad swipe moves, in pixels per second. `speed` was the
 * estimate at the last event; the fingers have since moved `moved` pixels in
 * `dt` seconds. Each event's own speed is averaged into the estimate to
 * smooth out jitter. A gap of 0.1 s or more between events means the fingers
 * rested, so the estimate starts again from 0.
 */
double tw_swipe_speed(double speed, double moved, double dt);

/* The speed to glide on with when the fingers lift `dt` seconds after the
 * last movement: 0 if they had come to rest before lifting. */
double tw_swipe_release(double speed, double dt);

/*
 * Whether an output on battery should use a mode that refreshes `candidate`
 * mHz in place of one that refreshes `current` mHz: only a slower one, and
 * none slower than `floor_hz`. Rates count in whole Hz, because what a
 * monitor calls 60 Hz is 59.94 or 59.999, and trading one for the other
 * would blink the screen for no saving.
 */
bool tw_refresh_saves(int candidate, int current, int floor_hz);

/* What the power supplies say at one look. */
struct tw_supplies {
	bool battery; /* the machine has a battery of its own */
	bool mains;   /* something else supplies it: the charger, a USB-C port */
	int charge;   /* how full the fullest battery is, in percent; -1 if none says */
};

/* What tw remembers about a battery that is running down. */
struct tw_battery {
	bool fresh;   /* somebody has just arrived; see tw_power_look() */
	bool warned;  /* the warning has been given since the machine went on battery */
	int low_mark; /* the machine is not suspended until the charge is below this */
};

#define TW_BATTERY_WARN 1
#define TW_BATTERY_SUSPEND 2
#define TW_BATTERY_SWITCH 4 /* the machine went on battery or came off it */

/*
 * What a look at the power supplies calls for, as a set of the flags above.
 * Call with each look; `*on_battery` follows what the supplies say: on
 * battery when the machine has one and nothing else supplies it. `woke`
 * says the machine has slept since the last look. Start from a zeroed
 * struct with `fresh` set and `*on_battery` as the machine was found.
 *
 * On battery, with a charge to read, the warning is due once, at `warn_at`
 * or below. Suspending is due when the charge has fallen to `suspend_at` or
 * below, and again for each percent it falls further, so a machine that is
 * woken and left unplugged goes back to sleep before it runs out. The first
 * charge read after somebody has arrived (the cable was pulled, tw started,
 * the machine woke up) is where it starts from, however low, and only a fall
 * from there suspends. A threshold of 0 turns that part off.
 */
int tw_power_look(bool *on_battery, struct tw_battery *battery,
	struct tw_supplies now, bool woke, int warn_at, int suspend_at);

/* Whether an output with this connector name is a laptop's own screen
 * (eDP, LVDS or DSI): the one its lid covers. */
bool tw_connector_builtin(const char *name);

/*
 * Whether an output rule's name is for this output: its connector ("eDP-1",
 * "HDMI-A-1"), or the monitor's own name as it gives it, "<make> <model>",
 * or "<make> <model> <serial>" to tell two alike apart. make, model and
 * serial may be NULL: a monitor that gives none has only its connector.
 */
bool tw_output_named(const char *rule, const char *connector,
	const char *make, const char *model, const char *serial);

/* What tw remembers about a laptop's lid. */
struct tw_lid {
	bool closed;
	int others; /* outputs other than the laptop's own screens, at the last look */
};

#define TW_LID_PANEL 1 /* the laptop's own screen is in use */
#define TW_LID_LOCK 2  /* lock the session */

/*
 * What a laptop's lid calls for, as a set of the flags above. Call whenever
 * the lid moves or an output comes or goes, with the lid's state and how
 * many of the laptop's own screens (`panels`) and other outputs (`others`)
 * there are now.
 *
 * The laptop's own screen is in use while the lid is open, and while it is
 * closed only if there is no other output, so that there is always a screen.
 * Locking is due when that screen becomes the only one behind a closed lid:
 * the lid closes with no other output, or the last other output goes away
 * while it is closed. That is when logind suspends the machine; with another
 * output it does not, and the user works on there. Before any output has
 * appeared nothing is due: tw is starting, and which outputs are about to
 * come is not known yet.
 */
int tw_lid_look(struct tw_lid *lid, bool closed, int panels, int others);

#endif
