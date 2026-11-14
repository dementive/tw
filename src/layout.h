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
 * New view_x after the smallest scroll that brings the column, plus a gap on
 * each side, fully into view. Returns view_x unchanged if it already is.
 */
int tw_view_fit(int view_x, int view_width, int col_x, int col_width, int gap);

/* Whether any part of the column is on the output. */
bool tw_column_visible(int view_x, int view_width, int col_x, int col_width);

#endif
