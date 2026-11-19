/*
 * What depends on the machine tw runs on: its screens, and whether tw may
 * send it to sleep. As it starts, tw reads the board's name
 * (`cat /sys/class/dmi/id/board_name`) and takes the entry of that name, or
 * else the last one, which is for every other machine. So one installed tw
 * suits every machine its disk boots, as the owner's SSD boots two laptops
 * (docs/g533zw.md, docs/g615lm.md). `tw -d` logs which entry it took.
 *
 * Included by config.h, in the place where these settings belong.
 *
 * outputs: up to TW_MACHINE_OUTPUTS, by connector name (`tw -d` logs the
 * name as each one appears). Scales may be fractional. x and y are where the
 * output's top left corner goes in the combined screen area, in logical
 * pixels; AUTO puts the output to the right of the others, in the order they
 * were plugged in. An output not listed is at scale 1, at AUTO.
 *
 * suspend_after_seconds: suspend the machine after this many seconds without
 * input; 0 never does. The session is locked first (lock_on_idle). A visible
 * window that inhibits idling holds it off, and so does anything holding a
 * systemd sleep inhibitor (`systemd-inhibit --what=sleep <command>` for a
 * long build). If the machine is still awake afterwards, tw tries again
 * after the same time. `tw -z` overrides this.
 *
 * battery_suspend_percent: on battery, lock and suspend when the charge is
 * down to this many percent, and again for each further percent lost if the
 * machine is woken and left unplugged. A charge that was already that low
 * when the cable was pulled, tw started or the machine woke does not suspend
 * it; only a fall from there does. 0 never does.
 *
 * Both stay 0 until a suspend and a wake-up have been seen to work on the
 * machine (`systemctl suspend` from a terminal, then a key): a timer must not
 * send a machine into a sleep nobody has seen it come back from. 900 and 5
 * are usual values after that.
 */
static const struct tw_machine machines[] = {
	{
		/* ASUS ROG Strix G533ZW: a 1920x1080 panel that runs at 300 Hz,
		 * an Intel GPU that drives it and an NVIDIA one behind HDMI. */
		.board = "G533ZW",
		.outputs = {
			/* name     scale   x     y */
			{"eDP-1",   1.25f,  AUTO, AUTO}, /* the laptop's own screen */
		},
		/* It does not wake from a suspend. The one time tw's idle timer
		 * sent it to sleep (2026-11-15, after 900 seconds) it had to be
		 * switched off and on again, and sleep has been left off since
		 * rather than looked into. See docs/g533zw.md. */
		.suspend_after_seconds = 0,
		.battery_suspend_percent = 0,
	},
	{
		/* ASUS ROG Strix G16 G615LM: a 2560x1600 panel that runs at 240
		 * Hz, an Intel GPU that drives it and an NVIDIA one behind HDMI
		 * and a DisplayPort. See docs/g615lm.md. */
		.board = "G615LM",
		.outputs = {
			/* name     scale   x     y */
			{"eDP-1",   1.25f,  AUTO, AUTO}, /* the laptop's own screen */
			/* The same panel, should the firmware's switch give it to the
			 * NVIDIA GPU; not tried. */
			{"eDP-2",   1.25f,  AUTO, AUTO},
		},
		/* Not suspended by hand yet. */
		.suspend_after_seconds = 0,
		.battery_suspend_percent = 0,
	},
	{
		/* Every other machine: every screen at scale 1, no sleep. */
		.board = NULL,
		.suspend_after_seconds = 0,
		.battery_suspend_percent = 0,
	},
};
