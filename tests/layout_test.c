/* Unit tests for src/layout.c. Runs without a compositor. */
#include <math.h>
#include <stdint.h>
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
	CHECK(tw_column_width(10, 20, half) == 1);
	CHECK(tw_column_width(-100, gap, full) == 1);

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
	/* So is one exactly as wide, such as a fullscreen column with no margin. */
	CHECK(tw_view_fit(0, view, col1, view, 0) == col1);

	CHECK(tw_column_visible(0, view, col0, w));
	CHECK(tw_column_visible(0, view, col1, w));
	CHECK(!tw_column_visible(0, view, col2, w));
	CHECK(!tw_column_visible(view_x, view, col0, w));
	/* One pixel on the output still counts as visible. */
	CHECK(tw_column_visible(col0 + w - 1, view, col0, w));
	CHECK(!tw_column_visible(col0 + w, view, col0, w));

	/* The view stays on the strip. Three half columns are wider than the
	 * output, so the view can scroll by exactly one column and no further. */
	int strip = 3 * w + 4 * gap;
	CHECK(tw_view_clamp(0, view, strip) == 0);
	CHECK(tw_view_clamp(-50, view, strip) == 0);
	CHECK(tw_view_clamp(view_x, view, strip) == view_x);
	CHECK(tw_view_clamp(view_x + 1, view, strip) == view_x);
	/* A strip that fits on the output never scrolls. */
	CHECK(tw_view_clamp(view_x, view, 2 * w + 3 * gap) == 0);
	CHECK(tw_view_clamp(view_x, view, 0) == 0);

	/* Stacked windows share a column and always fill it exactly. */
	CHECK(tw_stack_height(560, gap, 1, 0) == 560);
	CHECK(tw_stack_height(560, gap, 2, 0) == 276);
	CHECK(tw_stack_height(560, gap, 2, 1) == 276);
	for (int count = 1; count <= 6; count++) {
		int sum = 0;
		for (int i = 0; i < count; i++) {
			sum += tw_stack_height(563, gap, count, i);
		}
		CHECK(sum + (count - 1) * gap == 563);
	}
	/* Too many windows for the height still get a positive size. */
	CHECK(tw_stack_height(10, gap, 4, 0) >= 1);
	for (int i = 0; i < 1000; i++) {
		CHECK(tw_stack_height(10, gap, 1000, i) >= 1);
		CHECK(tw_stack_height(-50, gap, 1000, i) >= 1);
	}

	/* The scroll spring. From rest it approaches the target monotonically
	 * and never passes it. */
	const double omega = sqrt(800.0), target = 500.0;
	double pos = 0, vel = 0;
	CHECK(!tw_spring_settled(pos, vel, target));
	for (int frame = 0; frame < 60; frame++) {
		double before = pos;
		tw_spring_step(&pos, &vel, target, omega, 1.0 / 60.0);
		CHECK(pos >= before);
		CHECK(pos <= target);
	}
	CHECK(tw_spring_settled(pos, vel, target));
	/* The step is exact, so the frame rate does not change the motion. */
	double coarse_pos = 0, coarse_vel = 0, fine_pos = 0, fine_vel = 0;
	tw_spring_step(&coarse_pos, &coarse_vel, target, omega, 0.1);
	for (int i = 0; i < 10; i++) {
		tw_spring_step(&fine_pos, &fine_vel, target, omega, 0.01);
	}
	CHECK(fabs(coarse_pos - fine_pos) < 1e-6);
	CHECK(fabs(coarse_vel - fine_vel) < 1e-6);
	/* A swipe that ends moving toward the target gets there sooner. */
	double flung_pos = 0, flung_vel = 2000, rest_pos = 0, rest_vel = 0;
	tw_spring_step(&flung_pos, &flung_vel, target, omega, 0.05);
	tw_spring_step(&rest_pos, &rest_vel, target, omega, 0.05);
	CHECK(flung_pos > rest_pos);
	/* However it starts, far away, flung either way, at any frame rate up
	 * to the 0.1 s a step is capped at, it settles within two seconds, so
	 * the animation always ends. */
	const double starts[][2] = {{-20000, 0}, {0, 50000}, {0, -50000}, {499, 0}};
	const double steps[] = {1.0 / 300.0, 1.0 / 60.0, 0.1};
	for (size_t s = 0; s < sizeof(starts) / sizeof(starts[0]); s++) {
		for (size_t d = 0; d < sizeof(steps) / sizeof(steps[0]); d++) {
			pos = starts[s][0];
			vel = starts[s][1];
			double t = 0;
			while (!tw_spring_settled(pos, vel, target) && t < 2.0) {
				tw_spring_step(&pos, &vel, target, omega, steps[d]);
				t += steps[d];
			}
			CHECK(tw_spring_settled(pos, vel, target));
		}
	}
	/* Swipe speed: events 10 ms apart moving 20 px each build up toward
	 * 2000 px/s; a second half of one event (same time) changes nothing. */
	double speed = 0;
	for (int i = 0; i < 10; i++) {
		speed = tw_swipe_speed(speed, 20, 0.01);
	}
	CHECK(fabs(speed - 2000) < 5);
	CHECK(tw_swipe_speed(speed, 5, 0) == speed);
	/* Fingers that rested between two events start the estimate afresh. */
	CHECK(tw_swipe_speed(speed, 20, 0.1) == 0);
	CHECK(tw_swipe_speed(speed, 20, 1.0) == 0);
	/* Lifting right after moving keeps the fling; lifting after holding
	 * still sets the strip down where it is. */
	CHECK(tw_swipe_release(speed, 0.008) == speed);
	CHECK(tw_swipe_release(speed, 1.0) == 0);
	/* A timestamp that wrapped or went backwards reads as a long rest. */
	CHECK(tw_swipe_release(speed, (double)(uint32_t)(5u - 10u) / 1000.0) == 0);
	/* A zero-length step changes nothing. */
	double still_pos = 123, still_vel = 45;
	tw_spring_step(&still_pos, &still_vel, target, omega, 0);
	CHECK(still_pos == 123 && still_vel == 45);

	/* On battery an output takes a slower mode, down to the floor. The panel
	 * this was written for has 300 Hz and 59.999 Hz, which counts as 60. */
	CHECK(tw_refresh_saves(59999, 300000, 60));
	CHECK(tw_refresh_saves(59940, 144000, 60));
	CHECK(tw_refresh_saves(120000, 300000, 60));
	CHECK(tw_refresh_saves(60000, 120000, 60));
	/* Never a faster one or the same again, and none below the floor. */
	CHECK(!tw_refresh_saves(300000, 59999, 60));
	CHECK(!tw_refresh_saves(60000, 60000, 60));
	CHECK(!tw_refresh_saves(48000, 300000, 60));
	CHECK(!tw_refresh_saves(59499, 300000, 60));
	CHECK(tw_refresh_saves(48000, 300000, 48));
	/* 59.94 Hz is no saving over 60: the screen would blink for nothing. */
	CHECK(!tw_refresh_saves(59940, 60000, 60));
	CHECK(!tw_refresh_saves(59940, 60000, 30));
	CHECK(!tw_refresh_saves(143856, 144000, 60));
	/* A mode whose rate is not known (0) is never taken. */
	CHECK(!tw_refresh_saves(0, 60000, 1));

	/* A battery running down from 12%, warning at 10 and suspending at 5.
	 * The first reading is where it starts from. */
	struct tw_battery battery = {0};
	CHECK(tw_battery_look(&battery, 12, true, 10, 5) == 0);
	CHECK(tw_battery_look(&battery, 11, false, 10, 5) == 0);
	/* One warning, however long the charge stays low. */
	CHECK(tw_battery_look(&battery, 10, false, 10, 5) == TW_BATTERY_WARN);
	CHECK(tw_battery_look(&battery, 10, false, 10, 5) == 0);
	CHECK(tw_battery_look(&battery, 6, false, 10, 5) == 0);
	/* Suspended on reaching 5%, and not again while it stays there. */
	CHECK(tw_battery_look(&battery, 5, false, 10, 5) == TW_BATTERY_SUSPEND);
	CHECK(tw_battery_look(&battery, 5, false, 10, 5) == 0);
	/* Woken at 5% and left unplugged: nothing at once, and nothing for a
	 * reading that wobbles up and back, but again at the next percent lost. */
	CHECK(tw_battery_look(&battery, 5, true, 10, 5) == 0);
	CHECK(tw_battery_look(&battery, 6, false, 10, 5) == 0);
	CHECK(tw_battery_look(&battery, 5, false, 10, 5) == 0);
	CHECK(tw_battery_look(&battery, 4, false, 10, 5) == TW_BATTERY_SUSPEND);
	/* Having slept down to 2%, it is not suspended the moment it wakes. */
	CHECK(tw_battery_look(&battery, 2, true, 10, 5) == 0);
	CHECK(tw_battery_look(&battery, 1, false, 10, 5) == TW_BATTERY_SUSPEND);

	/* The cable pulled at 4%: warned at once, suspended only after a fall. */
	struct tw_battery pulled = {0};
	CHECK(tw_battery_look(&pulled, 4, true, 10, 5) == TW_BATTERY_WARN);
	CHECK(tw_battery_look(&pulled, 4, false, 10, 5) == 0);
	CHECK(tw_battery_look(&pulled, 3, false, 10, 5) == TW_BATTERY_SUSPEND);
	/* A reading that skips the thresholds asks for both at once. */
	struct tw_battery skipped = {0};
	CHECK(tw_battery_look(&skipped, 40, true, 10, 5) == 0);
	CHECK(tw_battery_look(&skipped, 3, false, 10, 5) == (TW_BATTERY_WARN | TW_BATTERY_SUSPEND));
	/* A threshold of 0 turns its half off, down to an empty battery. */
	struct tw_battery off = {0};
	CHECK(tw_battery_look(&off, 40, true, 0, 0) == 0);
	CHECK(tw_battery_look(&off, 0, false, 0, 0) == 0);
	CHECK(tw_battery_look(&off, 0, false, 10, 0) == TW_BATTERY_WARN);
	struct tw_battery quiet = {0};
	CHECK(tw_battery_look(&quiet, 40, true, 0, 5) == 0);
	CHECK(tw_battery_look(&quiet, 5, false, 0, 5) == TW_BATTERY_SUSPEND);

	/* A laptop's own screen, by its connector's name. */
	CHECK(tw_connector_builtin("eDP-1"));
	CHECK(tw_connector_builtin("LVDS-1"));
	CHECK(tw_connector_builtin("DSI-1"));
	CHECK(!tw_connector_builtin("HDMI-A-1"));
	CHECK(!tw_connector_builtin("DP-1"));
	CHECK(!tw_connector_builtin("eDP"));
	CHECK(!tw_connector_builtin("HEADLESS-1"));

	/* The lid on a laptop with nothing plugged in: closing it locks and
	 * leaves the screen on, so that there is one; opening it does nothing
	 * more. */
	struct tw_lid lid = {0};
	CHECK(tw_lid_look(&lid, false, 1, 0) == TW_LID_PANEL);
	CHECK(tw_lid_look(&lid, true, 1, 0) == (TW_LID_PANEL | TW_LID_LOCK));
	CHECK(tw_lid_look(&lid, true, 1, 0) == TW_LID_PANEL);
	CHECK(tw_lid_look(&lid, false, 1, 0) == TW_LID_PANEL);
	/* Docked: closing it takes the screen out, without locking. */
	CHECK(tw_lid_look(&lid, false, 1, 1) == TW_LID_PANEL);
	CHECK(tw_lid_look(&lid, true, 1, 1) == 0);
	CHECK(tw_lid_look(&lid, true, 1, 2) == 0);
	CHECK(tw_lid_look(&lid, true, 1, 1) == 0);
	/* Opened again: the screen comes back. */
	CHECK(tw_lid_look(&lid, false, 1, 1) == TW_LID_PANEL);
	/* Closed, then the last monitor unplugged: the screen comes back behind
	 * the lid, and the session locks, as logind is about to suspend. */
	CHECK(tw_lid_look(&lid, true, 1, 1) == 0);
	CHECK(tw_lid_look(&lid, true, 1, 0) == (TW_LID_PANEL | TW_LID_LOCK));
	/* A monitor plugged in behind the closed lid takes over from the screen. */
	CHECK(tw_lid_look(&lid, true, 1, 1) == 0);
	/* Started with the lid closed: the lid is known before any output, and
	 * nothing locks while the outputs appear, the laptop's own first. */
	struct tw_lid docked = {0};
	CHECK(tw_lid_look(&docked, true, 0, 0) == TW_LID_PANEL);
	CHECK(tw_lid_look(&docked, true, 1, 0) == TW_LID_PANEL);
	CHECK(tw_lid_look(&docked, true, 1, 1) == 0);
	/* With no screen of its own among the outputs, nothing ever locks. */
	struct tw_lid unknown = {0};
	CHECK(tw_lid_look(&unknown, true, 0, 1) == 0);
	CHECK(tw_lid_look(&unknown, true, 0, 0) == TW_LID_PANEL);

	if (failures > 0) {
		fprintf(stderr, "%d check(s) failed\n", failures);
		return 1;
	}
	return 0;
}
