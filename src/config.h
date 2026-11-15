/*
 * Compile-time configuration: edit and rebuild.
 *
 * There is deliberately no config parser, so configuration costs no code,
 * no memory and no startup time.
 */
#ifndef TW_CONFIG_H
#define TW_CONFIG_H

#include <wlr/types/wlr_keyboard.h>
#include <wlr/types/wlr_output_layout.h>
#include <xkbcommon/xkbcommon.h>

#include "tw.h"

/* Space between windows and around them, in logical pixels. 0: windows touch
 * each other and the screen edges. Windows have no borders either. */
static const int gap = 0;

/* Column widths as fractions of the output, cycled with Mod+R. */
static const struct tw_fraction column_widths[] = {{1, 3}, {1, 2}, {2, 3}};
static const size_t default_column_width = 1; /* index into column_widths */

/*
 * Outputs, by connector name (`tw -d` logs the name as each one appears).
 * The entry with a NULL name applies to every output not listed. Scales may
 * be fractional. x and y are where the output's top left corner goes in the
 * combined screen area, in logical pixels; AUTO puts the output to the right
 * of the others, in the order they were plugged in.
 */
#define AUTO TW_AUTO
static const struct tw_output_rule output_rules[] = {
	/* name     scale   x     y */
	{"eDP-1",   1.25f,  AUTO, AUTO}, /* the laptop's own screen */
	{NULL,      1.0f,   AUTO, AUTO},
};

/* Keyboard layout comes from the XKB_DEFAULT_* environment variables. */
static const int repeat_rate = 25;   /* keys per second */
static const int repeat_delay = 600; /* milliseconds */

/* Touchpads. */
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

/*
 * Stiffness of the spring that glides the strip to its place. Higher is
 * snappier (niri uses 800); 0 turns the animation off and the strip jumps.
 * Nothing is drawn or computed once the strip has arrived.
 */
static const int scroll_stiffness = 800;

/*
 * Turn the screen off after this many seconds without input; 0 never does.
 * Windows are told to stop drawing while it is off. A visible window that
 * inhibits idling (a playing video) holds it off. `tw -b` overrides this.
 */
static const int blank_after_seconds = 300;

/*
 * Suspend the machine after this many seconds without input; 0 never does.
 * The session is locked first (see lock_on_idle). A visible window that
 * inhibits idling holds it off, and so does anything holding a systemd sleep
 * inhibitor (`systemd-inhibit --what=sleep <command>` for a long build). If
 * the machine is still awake afterwards, tw tries again after the same time.
 * `tw -z` overrides this.
 */
static const int suspend_after_seconds = 900;
static const char suspend_command[] = "systemctl suspend";

/*
 * On battery. The kernel tells tw when the power cable goes in or out, so
 * nothing is polled, and on a machine without a battery none of this exists.
 *
 * Every output then runs at the slowest rate it offers at its normal size
 * that is not below battery_refresh_hz, and at its full rate again on mains.
 * Whatever animates is drawn that many times a second and no more, which is
 * the saving: on the 300 Hz panel this was written for, an animation took
 * 16.5 W at 300 frames a second, against 11.4 W for a still screen. The
 * screen may blink once as it changes. 0 keeps the full rate on battery.
 */
static const int battery_refresh_hz = 60;

/*
 * Run when the machine goes on battery and when it is back on mains, and
 * once at startup for whichever of the two holds then. "" runs nothing, and
 * so does a command that is not installed.
 */
static const char battery_command[] = "powerprofilesctl set power-saver";
static const char mains_command[] = "powerprofilesctl set balanced";

/*
 * Lock the session when the laptop lid closes, when the screen turns off
 * after blank_after_seconds, and before suspending. tw locks at once and by itself, so nothing is
 * visible or reachable even if the machine suspends before the locker is up;
 * the locker then draws the lock screen. Neither does anything unless the
 * locker below is installed.
 */
static const bool lock_on_lid_close = true;
static const bool lock_on_idle = true;

/* Programs. The launcher is any layer-shell client; fuzzel is small and fast.
 * It starts each program through `tw-gpu launch`, which switches the NVIDIA
 * GPU on for the ones whose desktop entry asks for the discrete GPU, and
 * otherwise just runs them. */
/* foot takes about 8 MB a window, measured under tw; alacritty takes 42. */
static const char terminal[] = "foot";
static const char launcher[] = "fuzzel --launch-prefix='tw-gpu launch'";
static const char locker[] = "swaylock";
static const char screenshot[] = "grim";
static const char screenshot_region[] = "grim -g \"$(slurp)\"";

/*
 * Programs started with the session, as shell commands; the list ends with
 * NULL. Each is a process that stays in memory until you log out, so keep it
 * short, and leave out what can start by itself when it is needed. One that
 * is not installed is skipped.
 *
 * The notification daemon (fnott) is not here for that reason: D-Bus starts
 * it when a program sends the first notification.
 */
static const char *const autostart[] = {
	"wl-clip-persist --clipboard regular", /* keeps a copy when its program exits */
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
 * them for the rest of the session.
 */
static const int x11_idle_seconds = 30;

static const char volume_up[] = "wpctl set-volume -l 1.0 @DEFAULT_AUDIO_SINK@ 5%+";
static const char volume_down[] = "wpctl set-volume @DEFAULT_AUDIO_SINK@ 5%-";
static const char volume_mute[] = "wpctl set-mute @DEFAULT_AUDIO_SINK@ toggle";
static const char mic_mute[] = "wpctl set-mute @DEFAULT_AUDIO_SOURCE@ toggle";
static const char brightness_up[] = "brightnessctl --class=backlight set 5%+";
static const char brightness_down[] = "brightnessctl --class=backlight set 5%-";

/*
 * Key bindings, following niri's defaults where tw has the feature.
 * Mod is Super on real hardware and Alt when tw runs inside another session.
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
};

#endif
