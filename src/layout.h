/*
 * Scrolling-strip maths.
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
 * `target` as fast as it can without overshooting, carrying on from whatever
 * *vel (units per second) it had. `omega` is sqrt(stiffness). The step is the
 * exact solution, so it is correct for any frame time, and a retarget or a
 * touchpad swipe that ends with some speed just continues from that state.
 */
void tw_spring_step(double *pos, double *vel, double target, double omega, double dt);

/* Whether a spring is close enough to its target to stop animating. */
bool tw_spring_settled(double pos, double vel, double target);

/*
 * Whether an output on battery should use a mode that refreshes `candidate`
 * mHz in place of one that refreshes `current` mHz: only a slower one, and
 * none slower than `floor_hz`. Rates count in whole Hz, because what a
 * monitor calls 60 Hz is 59.94 or 59.999.
 */
bool tw_refresh_saves(int candidate, int current, int floor_hz);

/* What tw remembers about a battery that is running down. */
struct tw_battery {
	bool warned;  /* the warning has been given since the machine went on battery */
	int low_mark; /* the machine is not suspended until the charge is below this */
};

#define TW_BATTERY_WARN 1
#define TW_BATTERY_SUSPEND 2

/*
 * What a battery at `charge` percent calls for, as a set of the flags above.
 * Call with each reading while the machine runs on battery, starting from a
 * zeroed struct each time it goes on battery.
 *
 * The warning is due once, at `warn_at` or below. Suspending is due when the
 * charge has fallen to `suspend_at` or below, and again for each percent it
 * falls further, so a machine that is woken and left unplugged goes back to
 * sleep before it runs out. `fresh` says that somebody has just arrived: the
 * cable was pulled, tw started, or the machine woke up. The charge found
 * then is where it starts from, however low, and only a fall from there
 * suspends. A threshold of 0 turns that part off.
 */
int tw_battery_look(struct tw_battery *battery, int charge, bool fresh,
	int warn_at, int suspend_at);

#endif
