/*
 * Compile-time configuration: edit and rebuild.
 *
 * There is deliberately no config parser, so configuration costs no code,
 * no memory and no startup time.
 */
#ifndef TW_CONFIG_H
#define TW_CONFIG_H

#include <wlr/types/wlr_keyboard.h>
#include <xkbcommon/xkbcommon.h>

#include "tw.h"

/* Layout, in logical pixels. */
static const int gap = 8;
static const int border_width = 2;
static const float border_focused[4] = {0.40f, 0.62f, 0.90f, 1.0f};
static const float border_unfocused[4] = {0.22f, 0.22f, 0.25f, 1.0f};

/* Column widths as fractions of the output, cycled with Mod+R. */
static const struct tw_fraction column_widths[] = {{1, 3}, {1, 2}, {2, 3}};
static const size_t default_column_width = 1; /* index into column_widths */

/* Applied to every output; fractional values such as 1.5 work. */
static const float output_scale = 1.25f;

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

/* Programs. The launcher is any layer-shell client; fuzzel is small and fast. */
static const char terminal[] = "alacritty";
static const char launcher[] = "fuzzel";
static const char locker[] = "swaylock";
static const char screenshot[] = "grim";
static const char screenshot_region[] = "grim -g \"$(slurp)\"";

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

/* Mod+N shows workspace N, Mod+Ctrl+N takes the focused column there. */
#define WORKSPACE(key, index) \
	{MOD,        key, action_workspace,         {.i = index}}, \
	{MOD | CTRL, key, action_move_to_workspace, {.i = index}}

static const struct tw_binding bindings[] = {
	/* modifiers    key                 action                              argument */
	{MOD,           XKB_KEY_t,          action_spawn,                       {.s = terminal}},
	{MOD,           XKB_KEY_d,          action_spawn,                       {.s = launcher}},
	{MOD,           XKB_KEY_q,          action_close,                       {0}},
	{MOD | SHIFT,   XKB_KEY_e,          action_quit,                        {0}},

	/* Screen: lock, power off until the next input, screenshots. */
	{MOD | ALT,     XKB_KEY_l,          action_spawn,                       {.s = locker}},
	{MOD | SHIFT,   XKB_KEY_p,          action_blank,                       {0}},
	{0,             XKB_KEY_Print,      action_spawn,                       {.s = screenshot}},
	{SHIFT,         XKB_KEY_Print,      action_spawn,                       {.s = screenshot_region}},

	/* Focus along the strip. */
	{MOD,           XKB_KEY_h,          action_focus,                       {.i = -1}},
	{MOD,           XKB_KEY_l,          action_focus,                       {.i = +1}},
	{MOD,           XKB_KEY_Left,       action_focus,                       {.i = -1}},
	{MOD,           XKB_KEY_Right,      action_focus,                       {.i = +1}},
	{MOD,           XKB_KEY_Home,       action_focus_edge,                  {.i = -1}},
	{MOD,           XKB_KEY_End,        action_focus_edge,                  {.i = +1}},

	/* Move the focused column. */
	{MOD | CTRL,    XKB_KEY_h,          action_move,                        {.i = -1}},
	{MOD | CTRL,    XKB_KEY_l,          action_move,                        {.i = +1}},
	{MOD | CTRL,    XKB_KEY_Left,       action_move,                        {.i = -1}},
	{MOD | CTRL,    XKB_KEY_Right,      action_move,                        {.i = +1}},
	{MOD | CTRL,    XKB_KEY_Home,       action_move_edge,                   {.i = -1}},
	{MOD | CTRL,    XKB_KEY_End,        action_move_edge,                   {.i = +1}},

	/* Windows within a column. */
	{MOD,           XKB_KEY_j,          action_focus_window,                {.i = +1}},
	{MOD,           XKB_KEY_k,          action_focus_window,                {.i = -1}},
	{MOD,           XKB_KEY_Down,       action_focus_window,                {.i = +1}},
	{MOD,           XKB_KEY_Up,         action_focus_window,                {.i = -1}},
	{MOD | CTRL,    XKB_KEY_j,          action_move_window,                 {.i = +1}},
	{MOD | CTRL,    XKB_KEY_k,          action_move_window,                 {.i = -1}},
	{MOD | CTRL,    XKB_KEY_Down,       action_move_window,                 {.i = +1}},
	{MOD | CTRL,    XKB_KEY_Up,         action_move_window,                 {.i = -1}},
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

	/* Workspaces are stacked vertically: up is the previous one. */
	{MOD,           XKB_KEY_u,          action_workspace_relative,          {.i = +1}},
	{MOD,           XKB_KEY_i,          action_workspace_relative,          {.i = -1}},
	{MOD,           XKB_KEY_Page_Down,  action_workspace_relative,          {.i = +1}},
	{MOD,           XKB_KEY_Page_Up,    action_workspace_relative,          {.i = -1}},
	{MOD | CTRL,    XKB_KEY_u,          action_move_to_workspace_relative,  {.i = +1}},
	{MOD | CTRL,    XKB_KEY_i,          action_move_to_workspace_relative,  {.i = -1}},
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
