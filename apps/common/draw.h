/*
 * Drawing for tw's apps: a picture in shared memory that becomes a
 * wl_buffer, rectangles, and text through fontconfig and FreeType, as any
 * program draws it. One font and one picture at a time per program.
 */
#ifndef TW_DRAW_H
#define TW_DRAW_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

struct wl_buffer;
struct wl_shm;

/* Pixels as 0xXXRRGGBB, `width` to a row. */
struct picture {
	uint32_t *pixels;
	int width, height;
	size_t size;
	int fd;
};

/* A new picture, black. False if there is no memory for it. */
bool picture_open(struct picture *picture, int width, int height);
/* Hands the picture to the compositor as a buffer and lets go of our view of
 * it, so the program holds no pixels between two pictures. NULL on failure. */
struct wl_buffer *picture_close(struct picture *picture, struct wl_shm *shm);
void picture_fill(struct picture *picture, int x, int y, int width, int height,
	uint32_t colour);

/*
 * The font, as fontconfig names one ("sans-serif", "DejaVu Sans:bold"), then
 * whatever fontconfig ranks next for a character it lacks. Opened once;
 * without a font, text takes room but nothing is drawn. With `verbose` the
 * file of each face opened is said on stderr.
 */
void font_open(const char *name, bool verbose);
void font_close(void);
/* The size text is drawn at, in the picture's pixels. */
void font_size_set(int size);
/* Of the first font at that size, in pixels. */
int font_ascent(void);
int font_line_height(void);

/* The character at *text, which must not be at its end, and moves past it.
 * Bad UTF-8 comes out as U+FFFD, control characters as spaces. */
uint32_t text_character(const char **text);
/* How far a character moves the pen, in 64ths of a pixel. */
int text_advance(uint32_t code);
/*
 * How many bytes of `text` fit on one line of `width` (in 64ths of a pixel).
 * A line ends at a line break, and with `words` between two words where it
 * can; at least one character is always taken.
 */
size_t text_fit(const char *text, int width, bool words);
/* Draws `length` bytes of text with the pen starting at x, followed by "…"
 * if `cut`. Returns where the pen ends, in pixels. */
int text_draw(struct picture *picture, int x, int baseline, const char *text, size_t length,
	bool cut, uint32_t colour);

#endif
