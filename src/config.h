/*
 * Compile-time configuration: edit and rebuild.
 *
 * There is deliberately no config parser, so configuration costs no code,
 * no memory and no startup time.
 *
 * What depends on the machine (its screens, whether it wakes from sleep) is
 * not in this file but in machines/<name>.h, one small file per machine;
 * see machines/default.h.
 */
#ifndef TW_CONFIG_H
#define TW_CONFIG_H

#include <wlr/types/wlr_keyboard.h>
#include <wlr/types/wlr_output_layout.h>
#include <xkbcommon/xkbcommon.h>

#include "tw.h"

/* --- Layout ------------------------------------------------------------------ */

/* Space between windows and around them, in logical pixels. 0: windows touch
 * each other and the screen edges. Windows have no borders either. */
static const int gap = 0;

/* Column widths as fractions of the output, cycled with Mod+R. */
static const struct tw_fraction column_widths[] = {{1, 3}, {1, 2}, {2, 3}};
static const size_t default_column_width = 1; /* index into column_widths */

/*
 * Stiffness of the spring that glides the strip to its place. Higher is
 * snappier (niri uses 800); 0 turns the animation off and the strip jumps.
 * Nothing is drawn or computed once the strip has arrived.
 */
static const int scroll_stiffness = 800;

/* --- Keyboard and touchpad --------------------------------------------------- */

/* Keyboard layout comes from the XKB_DEFAULT_* environment variables. */
static const int repeat_rate = 25;   /* keys per second */
static const int repeat_delay = 600; /* milliseconds */

static const bool touchpad_tap = true;
static const bool touchpad_natural_scroll = true;
static const bool touchpad_disable_while_typing = true;

/*
 * A swipe with this many fingers belongs to tw: sideways drags the strip,
 * up or down by at least swipe_workspace_distance pixels switches workspace.
 * Swipes with other finger counts, pinches and holds go to the application.
 */
static const unsigned swipe_fingers = 3;
static const int swipe_workspace_distance = 60;

/* --- Screen off, lock, sleep ------------------------------------------------- */

/*
 * Turn the screen off after this many seconds without input; 0 never does.
 * Windows are told to stop drawing while it is off. A visible window that
 * inhibits idling (a playing video) holds it off. `tw -b` overrides this.
 */
static const int blank_after_seconds = 300;

/*
 * Lock the session when the laptop lid closes with no monitor plugged in
 * (docked, the laptop's own screen just goes off), and when nobody is there:
 * as the screen turns off after blank_after_seconds, and before the machine
 * is suspended. tw locks at once and by itself, so nothing is visible or
 * reachable even if the machine suspends before the locker is up; the locker
 * then draws the lock screen. Neither does anything unless the locker below
 * is installed.
 */
static const bool lock_on_lid_close = true;
static const bool lock_on_idle = true;

/* What suspends the machine. When that happens is the machine's business:
 * suspend_after_seconds and battery_suspend_percent in machines/. */
static const char suspend_command[] = "systemctl suspend";

/* --- On battery -------------------------------------------------------------- */

/*
 * The kernel tells tw when the power cable goes in or out, so nothing is
 * polled, and on a machine without a battery none of this exists.
 *
 * On battery every output runs at the slowest rate it offers at its normal
 * size that is not below battery_refresh_hz, and at its full rate again on
 * mains. Whatever animates is drawn that many times a second and no more,
 * which is the saving: on a 300 Hz panel an animation took 16.5 W at 300
 * frames a second and about 12 W at 60. The screen may blink once as it
 * changes. 0 keeps the full rate on battery.
 */
static const int battery_refresh_hz = 60;

/*
 * Run when the machine goes on battery and when it is back on mains, and
 * once at startup for whichever of the two holds then. "" runs nothing, and
 * so does a command that is not installed.
 */
static const char battery_command[] = "tw-power saver";
static const char mains_command[] = "tw-power balanced";

/*
 * A battery running low. tw has no bar to show the charge, so when it is
 * down to battery_warn_percent tw runs battery_warn_command, once. Plugging
 * in resets it. 0 never warns.
 */
static const int battery_warn_percent = 10;
static const char battery_warn_command[] =
	"notify-send --urgency=critical 'Battery low' 'Plug in the charger.'";

/* --- Programs ---------------------------------------------------------------- */

/* The launcher is any layer-shell client. It starts each program through
 * `tw-gpu launch`, which switches the NVIDIA GPU on for the ones whose
 * desktop entry asks for the discrete GPU, and otherwise just runs them. */
/* The terminal is a window of foot's server (started below, in autostart):
 * one process draws every terminal, so each after the first costs about
 * 10 MB less and opens sooner. -E gives the shell tw's environment, as a
 * foot started by tw would have. If the server is gone (it crashed, taking
 * its windows with it), a foot of its own. */
static const char terminal[] = "footclient -E -N || foot";
static const char launcher[] = "fuzzel --launch-prefix='tw-gpu launch'";
static const char locker[] = "swaylock";
static const char screenshot[] = "grim";
static const char screenshot_region[] = "grim -g \"$(slurp)\"";

/*
 * The volume, brightness and keyboard-light keys show the new level in a
 * notification, which the next press replaces rather than adds to (the hint
 * is the tag tw-notify replaces by). That is a few programs for a moment on
 * each press and tw-notify while it shows; nothing in between. A level
 * changed some other way does not show: watching for that would take a
 * program running all session.
 */
#define SHOW(tag, text) \
	" && notify-send --app-name=tw --urgency=low --expire-time=1500" \
	" --hint=string:x-canonical-private-synchronous:" tag " \"" text "\""
#define VOLUME(device) "$(wpctl get-volume " device \
	" | awk '{printf \"%d%%%s\", $2 * 100 + 0.5, $3 ? \", muted\" : \"\"}')"
#define LEVEL(device) "$(brightnessctl -m " device " info | cut -d, -f4)"
#define SPEAKERS "@DEFAULT_AUDIO_SINK@"
#define MICROPHONE "@DEFAULT_AUDIO_SOURCE@"
#define SCREEN "--class=backlight"
#define KEYBOARD "--device='*::kbd_backlight'" /* the usual name of its light */

static const char volume_up[] = "wpctl set-volume -l 1.0 " SPEAKERS " 5%+"
	SHOW("volume", "Volume " VOLUME(SPEAKERS));
static const char volume_down[] = "wpctl set-volume " SPEAKERS " 5%-"
	SHOW("volume", "Volume " VOLUME(SPEAKERS));
static const char volume_mute[] = "wpctl set-mute " SPEAKERS " toggle"
	SHOW("volume", "Volume " VOLUME(SPEAKERS));
static const char mic_mute[] = "wpctl set-mute " MICROPHONE " toggle"
	SHOW("microphone", "Microphone " VOLUME(MICROPHONE));
static const char brightness_up[] = "brightnessctl -q " SCREEN " set 5%+"
	SHOW("brightness", "Brightness " LEVEL(SCREEN));
static const char brightness_down[] = "brightnessctl -q " SCREEN " set 5%-"
	SHOW("brightness", "Brightness " LEVEL(SCREEN));
/* A keyboard light has a few steps (three on the G533ZW), not a scale. */
static const char keyboard_light_up[] = "brightnessctl -q " KEYBOARD " set 1+"
	SHOW("keyboard-light", "Keyboard light " LEVEL(KEYBOARD));
static const char keyboard_light_down[] = "brightnessctl -q " KEYBOARD " set 1-"
	SHOW("keyboard-light", "Keyboard light " LEVEL(KEYBOARD));
#undef SHOW
#undef VOLUME
#undef LEVEL
#undef SPEAKERS
#undef MICROPHONE
#undef SCREEN
#undef KEYBOARD

/*
 * Programs started with the session, as shell commands; the list ends with
 * NULL. Each is a process that stays in memory until you log out, so keep it
 * short, and leave out what can start by itself when it is needed. One that
 * is not installed is skipped.
 *
 * D-Bus starts tw-notify when a program sends a notification, and tw keeps
 * what was copied itself (clipboard_keep_bytes). foot's terminal server is
 * the one here: about 10 MB from login, then the first terminal is no
 * dearer than a foot of its own and every further one cheaper. It listens
 * on a socket named after this tw's display, so a tw run nested has its
 * own.
 */
static const char *const autostart[] = {
	"foot --server",
	NULL,
};

/*
 * X11 applications run through this helper, which starts Xwayland. tw starts
 * it when the first X11 application connects and not before. "" turns X11
 * support off.
 */
static const char x11_helper[] = "xwayland-satellite";

/*
 * The two are stopped again once no X11 application is connected, which tw
 * looks at this many seconds after starting them and after a window closed.
 * Starting them takes a moment, so this is long enough that a program asking
 * X11 one question after another does not restart them each time. 0 keeps
 * them for the rest of the session. `tw -x` overrides this.
 */
static const int x11_idle_seconds = 30;

/*
 * What is copied stays in the program it was copied from, and would be gone
 * when that program exits. tw reads each copy as it is made and offers it in
 * the program's place once the program is gone. This is the most it holds
 * for one copy at any moment, in bytes: the formats it has read, those with
 * the same bytes held once, and the one it is reading. A larger copy is left
 * alone and goes with its program. (A paste still under way when the copy is
 * replaced keeps its format until the reader has it all or closes the pipe.)
 * 0 keeps nothing.
 */
static const size_t clipboard_keep_bytes = 64 << 20;

/* --- The machine ------------------------------------------------------------- */

/* The build names the file (-Dmachine=<name>, or by the machine's board). */
#define AUTO TW_AUTO
#ifndef TW_MACHINE_H
#define TW_MACHINE_H "machines/default.h"
#endif
#include TW_MACHINE_H
#undef AUTO

/* --- Key bindings ------------------------------------------------------------ */

/*
 * Key bindings, following niri's defaults where tw has the feature.
 * Mod is Super on real hardware and Alt when tw runs inside another session,
 * where a Mod+Alt binding therefore does not exist.
 * Keys are written unshifted: Mod+Shift+E is {MOD | SHIFT, XKB_KEY_e}.
 */
#define MOD TW_MOD
#define SHIFT WLR_MODIFIER_SHIFT
#define CTRL WLR_MODIFIER_CTRL
#define ALT WLR_MODIFIER_ALT

/*
 * One direction key, four meanings:
 *   Mod+key             focus: left and right along the strip, up and down
 *                       within a column
 *   Mod+Shift+key       workspaces: go to the monitor on that side; with no
 *                       monitor there, the previous (up, left) or next (down,
 *                       right) workspace
 *   Mod+Ctrl+key        move the focused column (left, right) or window (up,
 *                       down) that way
 *   Mod+Shift+Ctrl+key  take the focused column along to where Mod+Shift goes
 */
#define DIRECTION(key, focus, focus_arg, move, move_arg, direction) \
	{MOD,                key, focus,                           {.i = focus_arg}}, \
	{MOD | CTRL,         key, move,                            {.i = move_arg}}, \
	{MOD | SHIFT,        key, action_workspace_toward,         {.i = direction}}, \
	{MOD | SHIFT | CTRL, key, action_move_to_workspace_toward, {.i = direction}}

/* Mod+N shows workspace N, Mod+Ctrl+N takes the focused column there. */
#define WORKSPACE(key, index) \
	{MOD,        key, action_workspace,         {.i = index}}, \
	{MOD | CTRL, key, action_move_to_workspace, {.i = index}}

static const struct tw_binding bindings[] = {
	/* modifiers    key                 action                              argument */
	{MOD,           XKB_KEY_t,          action_spawn,                       {.s = terminal}},
	{MOD,           XKB_KEY_d,          action_spawn,                       {.s = launcher}},
	{MOD,           XKB_KEY_space,      action_spawn,                       {.s = launcher}},
	{MOD,           XKB_KEY_q,          action_close,                       {0}},
	{MOD | SHIFT,   XKB_KEY_e,          action_quit,                        {0}},

	/* Screen: lock, power off until the next input, screenshots. */
	{MOD | ALT,     XKB_KEY_l,          action_lock,                        {0}},
	{MOD | SHIFT,   XKB_KEY_p,          action_blank,                       {0}},
	{0,             XKB_KEY_Print,      action_spawn,                       {.s = screenshot}},
	{SHIFT,         XKB_KEY_Print,      action_spawn,                       {.s = screenshot_region}},

	/* Directions: I up, J left, K down, L right, or the arrow keys. */
	DIRECTION(XKB_KEY_j,     action_focus,        -1, action_move,        -1, WLR_DIRECTION_LEFT),
	DIRECTION(XKB_KEY_l,     action_focus,        +1, action_move,        +1, WLR_DIRECTION_RIGHT),
	DIRECTION(XKB_KEY_i,     action_focus_window, -1, action_move_window, -1, WLR_DIRECTION_UP),
	DIRECTION(XKB_KEY_k,     action_focus_window, +1, action_move_window, +1, WLR_DIRECTION_DOWN),
	DIRECTION(XKB_KEY_Left,  action_focus,        -1, action_move,        -1, WLR_DIRECTION_LEFT),
	DIRECTION(XKB_KEY_Right, action_focus,        +1, action_move,        +1, WLR_DIRECTION_RIGHT),
	DIRECTION(XKB_KEY_Up,    action_focus_window, -1, action_move_window, -1, WLR_DIRECTION_UP),
	DIRECTION(XKB_KEY_Down,  action_focus_window, +1, action_move_window, +1, WLR_DIRECTION_DOWN),

	/* The ends of the strip. */
	{MOD,           XKB_KEY_Home,       action_focus_edge,                  {.i = -1}},
	{MOD,           XKB_KEY_End,        action_focus_edge,                  {.i = +1}},
	{MOD | CTRL,    XKB_KEY_Home,       action_move_edge,                   {.i = -1}},
	{MOD | CTRL,    XKB_KEY_End,        action_move_edge,                   {.i = +1}},

	/* Into and out of columns: a lone window joins the column on that side,
	 * a window sharing a column leaves it. */
	{MOD,           XKB_KEY_bracketleft,  action_consume_or_expel,          {.i = -1}},
	{MOD,           XKB_KEY_bracketright, action_consume_or_expel,          {.i = +1}},
	{MOD,           XKB_KEY_comma,      action_consume,                     {0}},
	{MOD,           XKB_KEY_period,     action_expel,                       {0}},

	/* Column width. */
	{MOD,           XKB_KEY_r,          action_cycle_width,                 {0}},
	{MOD,           XKB_KEY_f,          action_maximize,                    {0}},
	{MOD | SHIFT,   XKB_KEY_f,          action_fullscreen,                  {0}},

	/* Workspaces by Page_Up and Page_Down too, and by number. */
	{MOD,           XKB_KEY_Page_Down,  action_workspace_relative,          {.i = +1}},
	{MOD,           XKB_KEY_Page_Up,    action_workspace_relative,          {.i = -1}},
	{MOD | CTRL,    XKB_KEY_Page_Down,  action_move_to_workspace_relative,  {.i = +1}},
	{MOD | CTRL,    XKB_KEY_Page_Up,    action_move_to_workspace_relative,  {.i = -1}},
	WORKSPACE(XKB_KEY_1, 0),
	WORKSPACE(XKB_KEY_2, 1),
	WORKSPACE(XKB_KEY_3, 2),
	WORKSPACE(XKB_KEY_4, 3),
	WORKSPACE(XKB_KEY_5, 4),
	WORKSPACE(XKB_KEY_6, 5),
	WORKSPACE(XKB_KEY_7, 6),
	WORKSPACE(XKB_KEY_8, 7),
	WORKSPACE(XKB_KEY_9, 8),

	/* Laptop keys. */
	{0,             XKB_KEY_XF86AudioRaiseVolume,  action_spawn,            {.s = volume_up}},
	{0,             XKB_KEY_XF86AudioLowerVolume,  action_spawn,            {.s = volume_down}},
	{0,             XKB_KEY_XF86AudioMute,         action_spawn,            {.s = volume_mute}},
	{0,             XKB_KEY_XF86AudioMicMute,      action_spawn,            {.s = mic_mute}},
	{0,             XKB_KEY_XF86MonBrightnessUp,   action_spawn,            {.s = brightness_up}},
	{0,             XKB_KEY_XF86MonBrightnessDown, action_spawn,            {.s = brightness_down}},
	{0,             XKB_KEY_XF86KbdBrightnessUp,   action_spawn,            {.s = keyboard_light_up}},
	{0,             XKB_KEY_XF86KbdBrightnessDown, action_spawn,            {.s = keyboard_light_down}},
};
#undef DIRECTION
#undef WORKSPACE
#undef MOD
#undef SHIFT
#undef CTRL
#undef ALT

#endif
