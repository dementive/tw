/*
 * tw-notify's settings. Compile-time, like tw's own (src/config.h): change
 * one, build, install.
 */
#ifndef TW_NOTIFY_CONFIG_H
#define TW_NOTIFY_CONFIG_H

#include <stdint.h>

/*
 * Sizes, in logical pixels as in tw: on a screen at scale 1.25 everything is
 * a quarter larger in real pixels, the text too.
 */
enum {
	note_width = 380,   /* of a notification */
	note_margin = 12,   /* to the top and the right edge of the screen */
	note_padding = 10,  /* around the text */
	note_border = 1,
	font_size = 14,     /* the height of the text */
	body_lines = 4,     /* under the title; what does not fit ends in "…" */
	max_shown = 5,      /* at once; the oldest goes to make room */
};

/* Colours, as 0xRRGGBB. */
static const uint32_t colour_background = 0x1d1f21;
static const uint32_t colour_title = 0xffffff;
static const uint32_t colour_text = 0xc5c8c6;
static const uint32_t colour_border = 0x5c6370;
static const uint32_t colour_critical = 0xe06c75; /* the border of an urgent one */

/*
 * How long a notification stays if its program does not say, in
 * milliseconds. An urgent one (the low-battery warning is) stays until it
 * is clicked. A click anywhere on a notification closes it, and a left click
 * first tells the program, which brings its window forward if it wants.
 */
static const int default_timeout_ms = 6000;

/*
 * tw-notify leaves this long after the last notification has gone, in
 * milliseconds. Long enough that a program asking three questions in a row
 * does not start it three times; it starts in a few milliseconds.
 */
static const int linger_ms = 1000;

#endif
