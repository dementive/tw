/*
 * What depends on the machine tw runs on. This file is for a machine that
 * has none of its own; to give one its own, copy this to <name>.h, edit, and
 * build with -Dmachine=<name>. With <name> the machine's board name
 * (`cat /sys/class/dmi/id/board_name`, in lower case, anything but letters
 * and digits as `_`) the build finds the file by itself.
 *
 * Included by config.h, in the place where these settings belong.
 */

/*
 * Outputs, by connector name (`tw -d` logs the name as each one appears).
 * The entry with a NULL name applies to every output not listed. Scales may
 * be fractional. x and y are where the output's top left corner goes in the
 * combined screen area, in logical pixels; AUTO puts the output to the right
 * of the others, in the order they were plugged in.
 */
static const struct tw_output_rule output_rules[] = {
	/* name     scale   x     y */
	{NULL,      1.0f,   AUTO, AUTO},
};

/*
 * Suspend the machine after this many seconds without input; 0 never does.
 * The session is locked first (lock_on_idle). A visible window that inhibits
 * idling holds it off, and so does anything holding a systemd sleep
 * inhibitor (`systemd-inhibit --what=sleep <command>` for a long build). If
 * the machine is still awake afterwards, tw tries again after the same time.
 * `tw -z` overrides this.
 *
 * 0 until a suspend and a wake-up have been seen to work on the machine
 * (`systemctl suspend` from a terminal, then a key): a timer must not send a
 * machine into a sleep nobody has seen it come back from. 900 is a usual
 * value after that.
 */
static const int suspend_after_seconds = 0;

/*
 * On battery, lock and suspend when the charge is down to this many percent,
 * and again for each further percent lost if the machine is woken and left
 * unplugged. A charge that was already that low when the cable was pulled,
 * tw started or the machine woke does not suspend it; only a fall from there
 * does. 0 never does, and is right for as long as suspend_after_seconds is
 * 0, for the same reason. 5 is a usual value after that.
 */
static const int battery_suspend_percent = 0;
