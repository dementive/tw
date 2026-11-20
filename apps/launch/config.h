/*
 * tw-launch's settings. Compile-time, like tw's own (src/config.h): change
 * one, build, install.
 */
#ifndef TW_LAUNCH_CONFIG_H
#define TW_LAUNCH_CONFIG_H

#include <stdint.h>

/*
 * Sizes, in logical pixels as in tw: on a screen at scale 1.25 everything is
 * a quarter larger in real pixels, the text too. The box is always this size,
 * however many programs match, so it never jumps as you type.
 */
enum {
	box_width = 520,
	row_height = 28, /* of the line you type in, and of each match */
	rows = 10,       /* matches shown at once; the list scrolls past them */
	padding = 12,    /* inside the border, around everything */
	border = 1,
	font_size = 15,  /* of the text, in pixels */
};

/* The font, as fontconfig names one ("DejaVu Sans", "monospace:bold").
 * Characters it lacks come from whichever font fontconfig picks next. */
static const char font[] = "sans-serif";

/* What is in front of what you type. tw-launch --dmenu -p replaces it. */
static const char prompt[] = "Run: ";

/* Colours, as 0xRRGGBB; the same as tw-notify's. */
static const uint32_t colour_background = 0x1d1f21;
static const uint32_t colour_border = 0x5c6370;
static const uint32_t colour_text = 0xc5c8c6;
static const uint32_t colour_prompt = 0x81a2be;
static const uint32_t colour_hint = 0x5c6370;    /* a program's kind: "Web Browser" */
static const uint32_t colour_selected = 0x373b41; /* behind the match Return starts */
static const uint32_t colour_selected_text = 0xffffff;

/*
 * How a program that runs in a terminal (Terminal=true: htop, btop) is
 * started, by /bin/sh with the program and its arguments as "$@". The same
 * terminal as tw's Mod+T: a window of foot's server, or a foot of its own.
 */
static const char terminal[] = "footclient -E -N -- \"$@\" || exec foot -- \"$@\"";

/*
 * A program whose desktop entry asks for the discrete GPU
 * (PrefersNonDefaultGPU=true, as games and Godot often do) is started through
 * this, which switches the NVIDIA GPU on for it. Where it is not installed
 * the program is started as it is.
 */
static const char *const gpu_command[] = {"tw-gpu", "run"};

#endif
