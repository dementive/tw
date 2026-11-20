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

/* Libinput sends gesture events only while the fingers move. */
#define SWIPE_REST_S 0.1

double tw_swipe_speed(double speed, double moved, double dt) {
	if (dt <= 0) {
		return speed; /* the same event, split in two */
	}
	if (dt >= SWIPE_REST_S) {
		return 0;
	}
	return 0.5 * speed + 0.5 * moved / dt;
}

double tw_swipe_release(double speed, double dt) {
	return dt < SWIPE_REST_S ? speed : 0;
}

static int whole_hz(int mhz) {
	return (mhz + 500) / 1000;
}

bool tw_refresh_saves(int candidate, int current, int floor_hz) {
	return whole_hz(candidate) < whole_hz(current) && whole_hz(candidate) >= floor_hz;
}

int tw_power_look(bool *on_battery, struct tw_battery *battery,
		struct tw_supplies now, bool woke, int warn_at, int suspend_at) {
	int due = 0;
	if ((now.battery && !now.mains) != *on_battery) {
		*on_battery = !*on_battery;
		*battery = (struct tw_battery){.fresh = true};
		due |= TW_BATTERY_SWITCH;
	}
	battery->fresh = battery->fresh || woke;
	/* Freshness waits for a charge to be read. */
	if (!*on_battery || now.charge < 0) {
		return due;
	}
	if (battery->fresh) {
		battery->fresh = false;
		battery->low_mark = now.charge;
	}
	if (warn_at > 0 && now.charge <= warn_at && !battery->warned) {
		battery->warned = true;
		due |= TW_BATTERY_WARN;
	}
	if (suspend_at > 0 && now.charge <= suspend_at && now.charge < battery->low_mark) {
		battery->low_mark = now.charge;
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

/* Whether `name` starts with `word` and a space, or is `word`; *rest is what follows. */
static bool starts_with_word(const char *name, const char *word, const char **rest) {
	size_t length = strlen(word);
	if (strncmp(name, word, length) != 0 || (name[length] != ' ' && name[length] != '\0')) {
		return false;
	}
	*rest = name[length] == ' ' ? name + length + 1 : name + length;
	return true;
}

bool tw_output_named(const char *rule, const char *connector,
		const char *make, const char *model, const char *serial) {
	if (strcmp(rule, connector) == 0) {
		return true;
	}
	const char *rest;
	if (make == NULL || model == NULL || make[0] == '\0' || model[0] == '\0' ||
			!starts_with_word(rule, make, &rest) || !starts_with_word(rest, model, &rest)) {
		return false;
	}
	return *rest == '\0' || (serial != NULL && strcmp(rest, serial) == 0);
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
