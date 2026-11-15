/*
 * Build tool: draws the characters tw-notify shows, once, as tw is built, and
 * prints them as C. So tw-notify needs no font library when it runs, which is
 * where a notification service's memory goes: fnott, which loads fontconfig,
 * FreeType and HarfBuzz, held 9.5 MB of its own once it had shown something.
 *
 * What that costs: only the characters listed below can be shown, anything
 * else appears as the font's "missing" box; and the text is as large as
 * font_size in notify/config.h says, drawn here for a handful of screen
 * scales.
 *
 *   bake <font file>  >font.h
 */
#include <ft2build.h>
#include FT_FREETYPE_H
#include <stdint.h>
#include <stdio.h>

#include "config.h"

/* Latin with its accented letters, and the punctuation that turns up in
 * notifications: dashes, curly quotes, the bullet, the ellipsis, the euro
 * sign, arrows. */
static const struct {
	uint32_t first, last;
} ranges[] = {
	{0x20, 0x7e}, {0xa0, 0x17f}, {0x2010, 0x2027}, {0x20ac, 0x20ac}, {0x2190, 0x2193},
};

/* Screen scales to draw for, in 120ths as Wayland counts them: 1, 1.25, 1.5,
 * 1.75 and 2. A screen at another scale gets the nearest size. */
static const int scales[] = {120, 150, 180, 210, 240};

struct glyph {
	uint32_t code, offset;
	unsigned advance, width, height;
	int left, top;
};

int main(int argc, char *argv[]) {
	FT_Library library;
	FT_Face face;
	if (argc != 2) {
		fprintf(stderr, "usage: %s <font file>\n", argv[0]);
		return 1;
	}
	if (FT_Init_FreeType(&library) != 0 || FT_New_Face(library, argv[1], 0, &face) != 0) {
		fprintf(stderr, "%s: cannot read the font %s\n", argv[0], argv[1]);
		return 1;
	}

	printf("/* Made by notify/bake.c from %s. */\n", argv[1]);
	printf("struct font_glyph {\n"
		"\tuint16_t code;    /* the character; 0 is the box for one that is missing */\n"
		"\tuint16_t advance; /* to the next character, in 64ths of a pixel */\n"
		"\tuint32_t offset;  /* of its picture in the font's `alpha` */\n"
		"\tuint8_t width, height;\n"
		"\tint8_t left, top; /* of the picture, from the pen and up from the baseline */\n"
		"};\n"
		"struct font {\n"
		"\tint size, ascent, line; /* in pixels */\n"
		"\tint count;\n"
		"\tconst struct font_glyph *glyphs; /* by character, ascending */\n"
		"\tconst uint8_t *alpha;\n"
		"};\n");

	static struct glyph glyphs[1024];
	int sizes[sizeof(scales) / sizeof(scales[0])];
	size_t size_count = sizeof(scales) / sizeof(scales[0]);
	for (size_t s = 0; s < size_count; s++) {
		int size = (font_size * scales[s] + 60) / 120;
		sizes[s] = size;
		/* Two scales can round to the same size; one copy is enough. */
		if (s > 0 && size == sizes[s - 1]) {
			continue;
		}
		if (FT_Set_Pixel_Sizes(face, 0, (FT_UInt)size) != 0) {
			fprintf(stderr, "%s: no size %d in %s\n", argv[0], size, argv[1]);
			return 1;
		}

		size_t count = 0;
		uint32_t offset = 0;
		printf("static const uint8_t font_alpha_%d[] = {", size);
		for (size_t r = 0; r <= sizeof(ranges) / sizeof(ranges[0]); r++) {
			/* First the missing-character box, which is glyph 0 of any font. */
			uint32_t first = r == 0 ? 0 : ranges[r - 1].first;
			uint32_t last = r == 0 ? 0 : ranges[r - 1].last;
			for (uint32_t code = first; code <= last; code++) {
				FT_UInt index = code == 0 ? 0 : FT_Get_Char_Index(face, code);
				if ((code != 0 && index == 0) || count == sizeof(glyphs) / sizeof(glyphs[0]) ||
						FT_Load_Glyph(face, index, FT_LOAD_RENDER | FT_LOAD_TARGET_LIGHT) != 0) {
					continue;
				}
				const FT_GlyphSlot slot = face->glyph;
				const FT_Bitmap *bitmap = &slot->bitmap;
				if (bitmap->width > 255 || bitmap->rows > 255 ||
						(bitmap->rows > 0 && bitmap->pixel_mode != FT_PIXEL_MODE_GRAY)) {
					continue;
				}
				glyphs[count++] = (struct glyph){
					code, offset, (unsigned)slot->advance.x, bitmap->width, bitmap->rows,
					slot->bitmap_left, slot->bitmap_top,
				};
				for (unsigned y = 0; y < bitmap->rows; y++) {
					printf("\n\t");
					for (unsigned x = 0; x < bitmap->width; x++) {
						printf("%u,", bitmap->buffer[(int)y * bitmap->pitch + (int)x]);
					}
				}
				offset += bitmap->width * bitmap->rows;
			}
		}
		printf("\n\t0};\nstatic const struct font_glyph font_glyphs_%d[%zu] = {\n", size, count);
		for (size_t i = 0; i < count; i++) {
			const struct glyph *glyph = &glyphs[i];
			printf("\t{%u, %u, %u, %u, %u, %d, %d},\n", glyph->code, glyph->advance,
				glyph->offset, glyph->width, glyph->height, glyph->left, glyph->top);
		}
		printf("};\n");
		printf("#define FONT_%zu {%d, %d, %d, %zu, font_glyphs_%d, font_alpha_%d}\n", s, size,
			(int)((face->size->metrics.ascender + 63) >> 6),
			(int)((face->size->metrics.height + 32) >> 6), count, size, size);
	}
	printf("static const struct font fonts[] = {");
	for (size_t s = 0; s < size_count; s++) {
		if (s == 0 || sizes[s] != sizes[s - 1]) {
			printf("FONT_%zu, ", s);
		}
	}
	printf("};\n");

	FT_Done_Face(face);
	FT_Done_FreeType(library);
	return 0;
}
