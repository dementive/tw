/*
 * tw-monitors' settings. Compile-time, like tw's own (src/config.h): change
 * one, build, install.
 */
#ifndef TW_MONITORS_CONFIG_H
#define TW_MONITORS_CONFIG_H

#include <stdint.h>

/* Sizes, in logical pixels as in tw. */
enum {
	box_width = 560,
	preview_height = 180, /* the screens drawn as boxes, as they are placed */
	row_height = 28,
	padding = 12,
	border = 1,
	font_size = 15,
	label_width = 90,     /* "Screen", "Mode": the column before the values */
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
static const uint32_t colour_screen = 0x2a2e33;   /* a screen in the picture */

/* The scales offered, smallest first. A screen at another one keeps it until
 * a step is taken from it. */
static const double scales[] = {0.5, 0.75, 1.0, 1.25, 1.5, 1.75, 2.0, 2.5, 3.0};

/*
 * After a change, the screens go back to how they were unless it is kept
 * within this many seconds: a mode a screen cannot show leaves it dark, and
 * then nothing on it can be read.
 */
static const int keep_seconds = 10;

#endif
