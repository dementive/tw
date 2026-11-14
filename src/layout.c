#include <math.h>

#include "layout.h"

int tw_column_width(int view_width, int gap, struct tw_fraction f) {
	int width = (view_width - gap) * f.num / f.den - gap;
	return width > 1 ? width : 1;
}

int tw_stack_height(int avail, int gap, int count, int index) {
	int each = (avail - (count - 1) * gap) / count;
	int height = index == count - 1 ? avail - (count - 1) * (each + gap) : each;
	return height > 1 ? height : 1;
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

int tw_view_clamp(int view_x, int view_width, int strip_width) {
	int max = strip_width > view_width ? strip_width - view_width : 0;
	if (view_x > max) {
		view_x = max;
	}
	return view_x > 0 ? view_x : 0;
}

bool tw_column_visible(int view_x, int view_width, int col_x, int col_width) {
	return col_x + col_width > view_x && col_x < view_x + view_width;
}

void tw_spring_step(double *pos, double *vel, double target, double omega, double dt) {
	/* x(t) = target + (a + b t) e^(-omega t), with a and b from the state. */
	double a = *pos - target;
	double b = *vel + omega * a;
	double decay = exp(-omega * dt);
	*pos = target + (a + b * dt) * decay;
	*vel = (b - omega * (a + b * dt)) * decay;
}

bool tw_spring_settled(double pos, double vel, double target) {
	return fabs(pos - target) < 0.5 && fabs(vel) < 10.0;
}
