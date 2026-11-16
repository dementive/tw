#include <math.h>
#include <string.h>

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

static int whole_hz(int mhz) {
	return (mhz + 500) / 1000;
}

bool tw_refresh_saves(int candidate, int current, int floor_hz) {
	return whole_hz(candidate) < whole_hz(current) && whole_hz(candidate) >= floor_hz;
}

int tw_battery_look(struct tw_battery *battery, int charge, bool fresh,
		int warn_at, int suspend_at) {
	int due = 0;
	if (fresh) {
		battery->low_mark = charge;
	}
	if (warn_at > 0 && charge <= warn_at && !battery->warned) {
		battery->warned = true;
		due |= TW_BATTERY_WARN;
	}
	if (suspend_at > 0 && charge <= suspend_at && charge < battery->low_mark) {
		battery->low_mark = charge;
		due |= TW_BATTERY_SUSPEND;
	}
	return due;
}

bool tw_connector_builtin(const char *name) {
	static const char *const prefixes[] = {"eDP-", "LVDS-", "DSI-"};
	for (size_t i = 0; i < sizeof(prefixes) / sizeof(prefixes[0]); i++) {
		if (strncmp(name, prefixes[i], strlen(prefixes[i])) == 0) {
			return true;
		}
	}
	return false;
}

int tw_lid_look(struct tw_lid *lid, bool closed, int panels, int others) {
	/* The laptop's own screen is the only one, behind a closed lid. */
	bool alone = closed && others == 0 && panels > 0;
	bool was_alone = lid->closed && lid->others == 0;
	lid->closed = closed;
	lid->others = others;
	return (!closed || others == 0 ? TW_LID_PANEL : 0) |
		(alone && !was_alone ? TW_LID_LOCK : 0);
}
