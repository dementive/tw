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
static const struct tw_fraction column_widths[] = {{1, 3}, {1, 2}, {2, 3}, {1, 1}};
static const size_t default_column_width = 1; /* index into column_widths */

/* Applied to every output; fractional values such as 1.5 work. */
static const float output_scale = 1.0f;

/* Keyboard layout comes from the XKB_DEFAULT_* environment variables. */
static const int repeat_rate = 25;   /* keys per second */
static const int repeat_delay = 600; /* milliseconds */

static const char terminal[] = "alacritty";

/* Mod is Super on real hardware and Alt when tw runs inside another session. */
#define MOD TW_MOD
#define SHIFT WLR_MODIFIER_SHIFT

static const struct tw_binding bindings[] = {
	/* modifiers    key             action              argument */
	{MOD,           XKB_KEY_Return, action_spawn,       {.s = terminal}},
	{MOD,           XKB_KEY_q,      action_close,       {0}},
	{MOD,           XKB_KEY_h,      action_focus,       {.i = -1}},
	{MOD,           XKB_KEY_l,      action_focus,       {.i = +1}},
	{MOD,           XKB_KEY_Left,   action_focus,       {.i = -1}},
	{MOD,           XKB_KEY_Right,  action_focus,       {.i = +1}},
	{MOD | SHIFT,   XKB_KEY_h,      action_move,        {.i = -1}},
	{MOD | SHIFT,   XKB_KEY_l,      action_move,        {.i = +1}},
	{MOD | SHIFT,   XKB_KEY_Left,   action_move,        {.i = -1}},
	{MOD | SHIFT,   XKB_KEY_Right,  action_move,        {.i = +1}},
	{MOD,           XKB_KEY_r,      action_cycle_width, {0}},
	{MOD | SHIFT,   XKB_KEY_e,      action_quit,        {0}},
};

#endif
