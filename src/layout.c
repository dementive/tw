#include "layout.h"

int tw_column_width(int view_width, int gap, struct tw_fraction f) {
	int width = (view_width - gap) * f.num / f.den - gap;
	return width > 1 ? width : 1;
}

int tw_view_fit(int view_x, int view_width, int col_x, int col_width, int gap) {
	int left = col_x - gap;
	int right = col_x + col_width + gap;

	/* A column wider than the output is shown from its left edge. */
	if (right - left >= view_width || left < view_x) {
		return left;
	}
	if (right > view_x + view_width) {
		return right - view_width;
	}
	return view_x;
}

bool tw_column_visible(int view_x, int view_width, int col_x, int col_width) {
	return col_x + col_width > view_x && col_x < view_x + view_width;
}
