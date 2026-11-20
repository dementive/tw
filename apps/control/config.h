/*
 * tw-control's settings. Compile-time, like tw's own (src/config.h): change
 * one, build, install.
 */
#ifndef TW_CONTROL_CONFIG_H
#define TW_CONTROL_CONFIG_H

#include <stdint.h>

/* Sizes, in logical pixels as in tw. */
enum {
	box_width = 520,
	row_height = 28,
	rows = 11,         /* below the clock; longer lists scroll */
	padding = 12,
	border = 1,
	font_size = 15,
	label_width = 130, /* "Wi-Fi", "Power mode": the column before the values */
	volume_step = 5,   /* percent, for Left and Right */
	brightness_step = 5,
	brightness_least = 5, /* Left stops here: a screen at 0 shows nothing */
};

static const char font[] = "sans-serif";

/* Colours, as 0xRRGGBB; the same as the launcher's. */
static const uint32_t colour_background = 0x1d1f21;
static const uint32_t colour_border = 0x5c6370;
static const uint32_t colour_text = 0xc5c8c6;
static const uint32_t colour_label = 0x81a2be;
static const uint32_t colour_hint = 0x5c6370;
static const uint32_t colour_selected = 0x373b41;
static const uint32_t colour_selected_text = 0xffffff;

/* The clock at the top, as strftime(3) writes it. */
static const char clock_format[] = "%A %e %B   %H:%M";

/*
 * What each part runs. Volume and the microphone go through WirePlumber,
 * brightness and the keyboard light through brightnessctl, as tw's own keys
 * do (src/config.h); Wi-Fi and Bluetooth devices are iwd's and BlueZ's,
 * asked over the system bus.
 */
#define SPEAKERS "@DEFAULT_AUDIO_SINK@"
#define MICROPHONE "@DEFAULT_AUDIO_SOURCE@"
static const char screen_light[] = "--class=backlight";
static const char keyboard_light[] = "--device='*::kbd_backlight'";
/* Bluetooth on and off, and the power profiles, through tw's commands. */
static const char bluetooth_command[] = "tw-bluetooth";
static const char profile_command[] = "tw-power";
static const char *const profiles[] = {"saver", "balanced", "performance"};
/* The session. Suspend locks first; swaylock -f returns once it has. */
static const char lock_command[] = "swaylock -f";
static const char suspend_command[] = "swaylock -f && systemctl suspend";
static const char restart_command[] = "systemctl reboot";
static const char power_off_command[] = "systemctl poweroff";
static const char monitors_command[] = "tw-monitors";

#endif
