/* Unit tests for the scrolling-strip maths. Runs without a compositor. */
#include <stdio.h>

#include "layout.h"

static int failures;

/* Not assert(): these must still run in release builds, where NDEBUG is set. */
#define CHECK(cond) \
	do { \
		if (!(cond)) { \
			fprintf(stderr, "%s:%d: check failed: %s\n", __FILE__, __LINE__, #cond); \
			failures++; \
		} \
	} while (0)

int main(void) {
	const int view = 1280, gap = 8;
	const struct tw_fraction third = {1, 3}, half = {1, 2}, full = {1, 1};

	/* n columns of width 1/n, plus the gaps around them, fill the output. */
	CHECK(2 * tw_column_width(view, gap, half) + 3 * gap == view);
	CHECK(3 * tw_column_width(view, gap, third) + 4 * gap == view);
	CHECK(1 * tw_column_width(view, gap, full) + 2 * gap == view);
	/* Degenerate outputs never produce a zero or negative width. */
	CHECK(tw_column_width(0, gap, half) == 1);

	int w = tw_column_width(view, gap, half);
	int col0 = gap, col1 = col0 + w + gap, col2 = col1 + w + gap;

	/* Columns already in view do not move the view. */
	CHECK(tw_view_fit(0, view, col0, w, gap) == 0);
	CHECK(tw_view_fit(0, view, col1, w, gap) == 0);
	/* The third column scrolls in by exactly one column. */
	int view_x = tw_view_fit(0, view, col2, w, gap);
	CHECK(view_x == w + gap);
	CHECK(tw_view_fit(view_x, view, col1, w, gap) == view_x);
	/* Going back left aligns the column with the left edge. */
	CHECK(tw_view_fit(view_x, view, col0, w, gap) == 0);
	/* A column wider than the output is shown from its left edge. */
	CHECK(tw_view_fit(0, view, col2, 2 * view, gap) == col2 - gap);

	CHECK(tw_column_visible(0, view, col0, w));
	CHECK(tw_column_visible(0, view, col1, w));
	CHECK(!tw_column_visible(0, view, col2, w));
	CHECK(!tw_column_visible(view_x, view, col0, w));
	/* One pixel on the output still counts as visible. */
	CHECK(tw_column_visible(col0 + w - 1, view, col0, w));
	CHECK(!tw_column_visible(col0 + w, view, col0, w));

	if (failures > 0) {
		fprintf(stderr, "%d check(s) failed\n", failures);
		return 1;
	}
	return 0;
}
