/*
 * ASUS ROG Strix G533ZW: a 1920x1080 panel that runs at 300 Hz, an Intel GPU
 * that drives it and an NVIDIA one behind the HDMI port.
 *
 * What the settings mean is in default.h.
 */

static const struct tw_output_rule output_rules[] = {
	/* name     scale   x     y */
	{"eDP-1",   1.25f,  AUTO, AUTO}, /* the laptop's own screen */
	{NULL,      1.0f,   AUTO, AUTO},
};

/*
 * Both 0: this laptop does not wake from a suspend. The one time tw's idle
 * timer sent it to sleep (2026-11-15, after 900 seconds) it had to be
 * switched off and on again, and sleep has been left off since rather than
 * looked into. See docs/g533zw.md.
 */
static const int suspend_after_seconds = 0;
static const int battery_suspend_percent = 0;
