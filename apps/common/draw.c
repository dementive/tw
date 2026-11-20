#define _GNU_SOURCE /* memfd_create(), program_invocation_short_name */
#include <errno.h> /* program_invocation_short_name */
#include <fontconfig/fontconfig.h>
#include <ft2build.h>
#include FT_FREETYPE_H
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>
#include <wayland-client.h>

#include "draw.h"

#define LENGTH(a) (sizeof(a) / sizeof((a)[0]))

/* --- The picture ------------------------------------------------------------- */

bool picture_open(struct picture *picture, int width, int height) {
	*picture = (struct picture){.width = width, .height = height, .fd = -1};
	picture->size = (size_t)width * (size_t)height * 4;
	picture->fd = memfd_create(program_invocation_short_name, MFD_CLOEXEC);
	void *pixels = MAP_FAILED;
	if (picture->fd >= 0 && ftruncate(picture->fd, (off_t)picture->size) == 0) {
		pixels = mmap(NULL, picture->size, PROT_READ | PROT_WRITE, MAP_SHARED, picture->fd, 0);
	}
	if (pixels == MAP_FAILED) {
		if (picture->fd >= 0) {
			close(picture->fd);
		}
		*picture = (struct picture){.fd = -1};
		return false;
	}
	picture->pixels = pixels;
	return true;
}

struct wl_buffer *picture_close(struct picture *picture, struct wl_shm *shm) {
	struct wl_shm_pool *pool = wl_shm_create_pool(shm, picture->fd, (int32_t)picture->size);
	struct wl_buffer *buffer = wl_shm_pool_create_buffer(pool, 0, picture->width,
		picture->height, picture->width * 4, WL_SHM_FORMAT_XRGB8888);
	wl_shm_pool_destroy(pool);
	munmap(picture->pixels, picture->size);
	close(picture->fd);
	*picture = (struct picture){.fd = -1};
	return buffer;
}

void picture_fill(struct picture *picture, int x, int y, int width, int height,
		uint32_t colour) {
	for (int row = y < 0 ? 0 : y; row < y + height && row < picture->height; row++) {
		for (int column = x < 0 ? 0 : x; column < x + width && column < picture->width;
				column++) {
			picture->pixels[row * picture->width + column] = colour;
		}
	}
}

/* --- The font ---------------------------------------------------------------- */

/*
 * The font comes from fontconfig, as in any other program. Each face is
 * opened as a character first needs it, so a program that draws only Latin
 * text opens one file.
 */
static bool verbose;
static FT_Library library;
static FcFontSet *fonts;   /* the font, then those to fall back on; NULL without one */
static FT_Face *faces;     /* by place in `fonts`, opened as a character needs them */
static bool *unusable;     /* fonts that would not open, or that cannot be scaled */
static int font_px;        /* the size the faces are set to, in pixels */
static int ascent, line_height; /* of the first font, in pixels */

/* Advances already measured, by character: measuring a line loads each
 * character once and not once per try. Emptied when the size changes. */
static struct {
	uint32_t code;
	int advance;
} advances[256];

void font_close(void) {
	for (int i = 0; fonts != NULL && faces != NULL && i < fonts->nfont; i++) {
		if (faces[i] != NULL) {
			FT_Done_Face(faces[i]);
		}
	}
	free(faces);
	free(unusable);
	faces = NULL;
	unusable = NULL;
	if (fonts != NULL) {
		FcFontSetDestroy(fonts);
		fonts = NULL;
		FT_Done_FreeType(library);
		FcFini();
	}
}

void font_open(const char *name, bool say) {
	static bool tried;
	if (tried) {
		return;
	}
	tried = true;
	verbose = say;
	FcPattern *pattern = FcNameParse((const FcChar8 *)name);
	FcResult result;
	if (pattern != NULL && FcConfigSubstitute(NULL, pattern, FcMatchPattern)) {
		FcDefaultSubstitute(pattern);
		fonts = FcFontSort(NULL, pattern, FcTrue, NULL, &result);
	}
	if (pattern != NULL) {
		FcPatternDestroy(pattern);
	}
	if (fonts != NULL && fonts->nfont > 0 && FT_Init_FreeType(&library) == 0) {
		faces = calloc((size_t)fonts->nfont, sizeof(*faces));
		unusable = calloc((size_t)fonts->nfont, sizeof(*unusable));
		if (faces != NULL && unusable != NULL) {
			return;
		}
		free(faces);
		free(unusable);
		faces = NULL;
		unusable = NULL;
		FT_Done_FreeType(library);
	}
	fprintf(stderr, "%s: no font for '%s'\n", program_invocation_short_name, name);
	if (fonts != NULL) {
		FcFontSetDestroy(fonts);
		fonts = NULL;
	}
	FcFini();
}

/* The face of fonts->fonts[i] at the size in use, or NULL. Colour fonts
 * (emoji) and bitmap fonts are passed over: their pictures come in sizes of
 * their own and would need scaling. */
static FT_Face face_get(int i) {
	if (unusable[i]) {
		return NULL;
	}
	if (faces[i] == NULL) {
		FcPattern *pattern = fonts->fonts[i];
		FcChar8 *file;
		int index = 0;
		FcBool colour = FcFalse, scalable = FcTrue;
		FcPatternGetInteger(pattern, FC_INDEX, 0, &index);
		FcPatternGetBool(pattern, FC_COLOR, 0, &colour);
		FcPatternGetBool(pattern, FC_SCALABLE, 0, &scalable);
		if (colour || !scalable ||
				FcPatternGetString(pattern, FC_FILE, 0, &file) != FcResultMatch ||
				FT_New_Face(library, (const char *)file, index, &faces[i]) != 0) {
			faces[i] = NULL;
			unusable[i] = true;
			return NULL;
		}
		if (verbose) {
			fprintf(stderr, "font %s\n", file);
		}
		FT_Set_Pixel_Sizes(faces[i], 0, (FT_UInt)font_px);
	}
	return faces[i];
}

void font_size_set(int size) {
	if (size == font_px) {
		return;
	}
	font_px = size;
	memset(advances, 0, sizeof(advances));
	for (int i = 0; fonts != NULL && i < fonts->nfont; i++) {
		if (faces[i] != NULL) {
			FT_Set_Pixel_Sizes(faces[i], 0, (FT_UInt)size);
		}
	}
	/* Without a font only the frames are drawn, and their height still goes
	 * by lines of text. */
	ascent = size;
	line_height = size * 4 / 3;
	FT_Face first = fonts != NULL ? face_get(0) : NULL;
	if (first != NULL) {
		ascent = (int)((first->size->metrics.ascender + 63) >> 6);
		line_height = (int)((first->size->metrics.height + 32) >> 6);
	}
}

int font_ascent(void) {
	return ascent;
}

int font_line_height(void) {
	return line_height;
}

/*
 * Loads the picture of a character into its face's glyph slot, rendered if
 * `render`, and returns the slot; NULL without any font. A character no font
 * has comes out as the first font's box for a missing one.
 */
static FT_GlyphSlot glyph_load(uint32_t code, bool render) {
	if (fonts == NULL) {
		return NULL;
	}
	FT_Face face = NULL;
	FT_UInt index = 0;
	for (int i = 0; i < fonts->nfont && index == 0; i++) {
		FcCharSet *charset;
		if (FcPatternGetCharSet(fonts->fonts[i], FC_CHARSET, 0, &charset) == FcResultMatch &&
				FcCharSetHasChar(charset, code) && (face = face_get(i)) != NULL) {
			index = FT_Get_Char_Index(face, code);
		}
	}
	if (index == 0) {
		face = face_get(0);
	}
	/* Outlines only: a scalable font may also carry pictures of its
	 * characters at a few sizes, in one bit per pixel, which glyph_draw()
	 * would leave out. */
	FT_Int32 flags = FT_LOAD_TARGET_LIGHT | FT_LOAD_NO_BITMAP | (render ? FT_LOAD_RENDER : 0);
	if (face == NULL || FT_Load_Glyph(face, index, flags) != 0) {
		return NULL;
	}
	return face->glyph;
}

int text_advance(uint32_t code) {
	size_t slot = code % LENGTH(advances);
	if (advances[slot].code != code || advances[slot].advance == 0) {
		FT_GlyphSlot glyph = glyph_load(code, false);
		advances[slot].code = code;
		advances[slot].advance = glyph != NULL ? (int)glyph->advance.x : 0;
	}
	return advances[slot].advance;
}

/* --- Text -------------------------------------------------------------------- */

uint32_t text_character(const char **text) {
	const unsigned char *bytes = (const unsigned char *)*text;
	uint32_t code = bytes[0];
	int more = code >= 0xf0 ? 3 : code >= 0xe0 ? 2 : code >= 0xc0 ? 1 : 0, used = 1;
	if (more > 0) {
		code &= 0x3fu >> more;
	} else if (code >= 0x80) {
		code = 0xfffd;
	}
	for (; more > 0 && (bytes[used] & 0xc0) == 0x80; more--, used++) {
		code = code << 6 | (bytes[used] & 0x3f);
	}
	*text += used;
	return more > 0 ? 0xfffd : code < 0x20 ? ' ' : code;
}

size_t text_fit(const char *text, int width, bool words) {
	const char *at = text, *space = NULL;
	int pen = 0;
	while (*at != '\0' && *at != '\n') {
		const char *start = at;
		uint32_t code = text_character(&at);
		if (code == ' ') {
			space = start;
		}
		pen += text_advance(code);
		if (pen > width && start > text) {
			return (size_t)((words && space != NULL && space > text ? space : start) - text);
		}
	}
	return (size_t)(at - text);
}

/* Draws one character with its pen at x, and returns its advance. */
static int glyph_draw(struct picture *picture, uint32_t code, int x, int baseline,
		uint32_t colour) {
	FT_GlyphSlot glyph = glyph_load(code, true);
	if (glyph == NULL) {
		return 0;
	}
	const FT_Bitmap *bitmap = &glyph->bitmap;
	if (bitmap->pixel_mode != FT_PIXEL_MODE_GRAY) {
		return (int)glyph->advance.x;
	}
	for (int row = 0; row < (int)bitmap->rows; row++) {
		int at_y = baseline - glyph->bitmap_top + row;
		if (at_y < 0 || at_y >= picture->height) {
			continue;
		}
		for (int column = 0; column < (int)bitmap->width; column++) {
			int at_x = x + glyph->bitmap_left + column;
			uint32_t cover = bitmap->buffer[row * bitmap->pitch + column];
			if (cover == 0 || at_x < 0 || at_x >= picture->width) {
				continue;
			}
			uint32_t *pixel = &picture->pixels[at_y * picture->width + at_x], mixed = 0;
			for (int shift = 0; shift < 24; shift += 8) {
				uint32_t under = *pixel >> shift & 0xff, over = colour >> shift & 0xff;
				mixed |= (under * (255 - cover) + over * cover + 127) / 255 << shift;
			}
			*pixel = mixed;
		}
	}
	return (int)glyph->advance.x;
}

int text_draw(struct picture *picture, int x, int baseline, const char *text, size_t length,
		bool cut, uint32_t colour) {
	int pen = x * 64;
	for (const char *at = text; at < text + length;) {
		pen += glyph_draw(picture, text_character(&at), (pen + 32) >> 6, baseline, colour);
	}
	if (cut) {
		pen += glyph_draw(picture, 0x2026, (pen + 32) >> 6, baseline, colour);
	}
	return (pen + 32) >> 6;
}
