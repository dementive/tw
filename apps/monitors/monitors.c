/*
 * tw-monitors: turns screens on and off and sets their mode, scale and
 * place, through the compositor's output management (wlr-output-management).
 *
 *   tw-monitors           a box: the screens drawn as they are placed, and a
 *                         row for each setting of the one picked
 *   tw-monitors --apply   puts back what was kept for the screens plugged in
 *                         now, and leaves; tw runs it as the session starts
 *                         and whenever a screen comes or goes
 *   tw-monitors --list    says how each screen is set, a line each
 *   -d                    say what it does, on stderr
 *
 * Keys in the box: Up and Down pick a row, Left and Right change it (Tab
 * picks the next screen); Return applies the changes, and then keeps them
 * (Return again) or goes back (Escape, or keep_seconds without an answer).
 * Escape leaves. A click on a screen picks it, a click on a value takes the
 * next one, the wheel goes through them.
 *
 * What is kept is written to $XDG_CONFIG_HOME/tw-monitors: a block for each
 * set of screens plugged in together, so that the laptop alone, at the desk
 * and with the TV each come back as they were. A screen goes by its make,
 * model and serial number where it gives them, else by its connector.
 */
#define _GNU_SOURCE
#include <errno.h>
#include <math.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
#include <wayland-client.h>
#include <xkbcommon/xkbcommon.h>

#include "box.h"
#include "config.h"
#include "draw.h"
#include "wlr-output-management-unstable-v1-client-protocol.h"

#define LENGTH(a) (sizeof(a) / sizeof((a)[0]))
#define MAX_SCREENS 8

static bool verbose;

static void say(const char *format, ...) __attribute__((format(printf, 1, 2)));
static void say(const char *format, ...) {
	if (verbose) {
		va_list args;
		va_start(args, format);
		vfprintf(stderr, format, args);
		va_end(args);
		fputc('\n', stderr);
	}
}

/* --- The screens ------------------------------------------------------------- */

struct mode {
	struct zwlr_output_mode_v1 *object;
	int width, height, refresh; /* refresh in mHz */
};

/* How a screen is set: as it is, as the box will set it, as it was. */
struct setting {
	bool on;
	struct mode *mode; /* NULL: one the screen does not list (a virtual one) */
	double scale;
	int x, y;
};

struct screen {
	struct zwlr_output_head_v1 *object;
	char name[64]; /* its connector: "HDMI-A-1" */
	char make[64], model[128], serial[64];
	struct mode *modes[64];
	size_t mode_count;
	struct setting now, wanted, before;
	bool gone;
};

static struct zwlr_output_manager_v1 *manager;
static struct screen screens[MAX_SCREENS];
static size_t screen_count;
static uint32_t serial; /* of the compositor's last word on the screens */
static bool heard;      /* it has said how they all are, once */

/* How the screen is saved: "<make> <model> <serial>", the same as tw names it
 * in machines.h, or its connector if it gives no make and model. */
static void screen_id(const struct screen *screen, char *id, size_t size) {
	if (screen->make[0] != '\0' && screen->model[0] != '\0') {
		snprintf(id, size, "%s %s%s%s", screen->make, screen->model,
			screen->serial[0] != '\0' ? " " : "", screen->serial);
	} else {
		snprintf(id, size, "%s", screen->name);
	}
}

/* Its size on the desktop: its mode's pixels at its scale. */
static void screen_size(const struct screen *screen, const struct setting *setting,
		int *width, int *height) {
	const struct mode *mode = setting->mode;
	double scale = setting->scale > 0 ? setting->scale : 1;
	*width = mode != NULL ? (int)lround(mode->width / scale) : 0;
	*height = mode != NULL ? (int)lround(mode->height / scale) : 0;
}

static void mode_size(void *data, struct zwlr_output_mode_v1 *object, int32_t width,
		int32_t height) {
	struct mode *mode = data;
	mode->width = width;
	mode->height = height;
}

static void mode_refresh(void *data, struct zwlr_output_mode_v1 *object, int32_t refresh) {
	struct mode *mode = data;
	mode->refresh = refresh;
}

static void mode_preferred(void *data, struct zwlr_output_mode_v1 *object) {
}

static void mode_finished(void *data, struct zwlr_output_mode_v1 *object) {
}

static const struct zwlr_output_mode_v1_listener mode_listener = {
	.size = mode_size,
	.refresh = mode_refresh,
	.preferred = mode_preferred,
	.finished = mode_finished,
};

static void head_name(void *data, struct zwlr_output_head_v1 *object, const char *name) {
	struct screen *screen = data;
	snprintf(screen->name, sizeof(screen->name), "%s", name);
}

static void head_description(void *data, struct zwlr_output_head_v1 *object,
		const char *description) {
}

static void head_physical_size(void *data, struct zwlr_output_head_v1 *object,
		int32_t width, int32_t height) {
}

static void head_mode(void *data, struct zwlr_output_head_v1 *object,
		struct zwlr_output_mode_v1 *mode_object) {
	struct screen *screen = data;
	struct mode *mode = calloc(1, sizeof(*mode));
	if (mode == NULL || screen->mode_count == LENGTH(screen->modes)) {
		free(mode);
		zwlr_output_mode_v1_destroy(mode_object);
		return;
	}
	mode->object = mode_object;
	zwlr_output_mode_v1_add_listener(mode_object, &mode_listener, mode);
	screen->modes[screen->mode_count++] = mode;
}

static void head_enabled(void *data, struct zwlr_output_head_v1 *object, int32_t enabled) {
	struct screen *screen = data;
	screen->now.on = enabled != 0;
}

static void head_current_mode(void *data, struct zwlr_output_head_v1 *object,
		struct zwlr_output_mode_v1 *mode_object) {
	struct screen *screen = data;
	for (size_t i = 0; i < screen->mode_count; i++) {
		if (screen->modes[i]->object == mode_object) {
			screen->now.mode = screen->modes[i];
		}
	}
}

static void head_position(void *data, struct zwlr_output_head_v1 *object, int32_t x, int32_t y) {
	struct screen *screen = data;
	screen->now.x = x;
	screen->now.y = y;
}

static void head_transform(void *data, struct zwlr_output_head_v1 *object, int32_t transform) {
}

static void head_scale(void *data, struct zwlr_output_head_v1 *object, wl_fixed_t scale) {
	struct screen *screen = data;
	screen->now.scale = wl_fixed_to_double(scale);
}

static void head_finished(void *data, struct zwlr_output_head_v1 *object) {
	struct screen *screen = data;
	screen->gone = true;
}

static void head_make(void *data, struct zwlr_output_head_v1 *object, const char *make) {
	struct screen *screen = data;
	snprintf(screen->make, sizeof(screen->make), "%s", make);
}

static void head_model(void *data, struct zwlr_output_head_v1 *object, const char *model) {
	struct screen *screen = data;
	snprintf(screen->model, sizeof(screen->model), "%s", model);
}

static void head_serial(void *data, struct zwlr_output_head_v1 *object, const char *number) {
	struct screen *screen = data;
	snprintf(screen->serial, sizeof(screen->serial), "%s", number);
}

static const struct zwlr_output_head_v1_listener head_listener = {
	.name = head_name,
	.description = head_description,
	.physical_size = head_physical_size,
	.mode = head_mode,
	.enabled = head_enabled,
	.current_mode = head_current_mode,
	.position = head_position,
	.transform = head_transform,
	.scale = head_scale,
	.finished = head_finished,
	.make = head_make,
	.model = head_model,
	.serial_number = head_serial,
};

static void screen_free(struct screen *screen) {
	for (size_t i = 0; i < screen->mode_count; i++) {
		zwlr_output_mode_v1_destroy(screen->modes[i]->object);
		free(screen->modes[i]);
	}
	zwlr_output_head_v1_destroy(screen->object);
}

static void manager_head(void *data, struct zwlr_output_manager_v1 *object,
		struct zwlr_output_head_v1 *head) {
	if (screen_count == LENGTH(screens)) {
		zwlr_output_head_v1_destroy(head);
		return;
	}
	struct screen *screen = &screens[screen_count++];
	*screen = (struct screen){.object = head};
	zwlr_output_head_v1_add_listener(head, &head_listener, screen);
}

static void screens_changed(void);

/* The compositor has said all there is: screens gone are dropped, and what
 * the box shows follows. */
static void manager_done(void *data, struct zwlr_output_manager_v1 *object, uint32_t done) {
	serial = done;
	size_t kept = 0;
	for (size_t i = 0; i < screen_count; i++) {
		if (screens[i].gone) {
			screen_free(&screens[i]);
			continue;
		}
		screens[kept] = screens[i];
		/* The listeners point at the screen, which has moved. */
		wl_proxy_set_user_data((struct wl_proxy *)screens[kept].object, &screens[kept]);
		kept++;
	}
	screen_count = kept;
	heard = true;
	for (size_t i = 0; i < screen_count; i++) {
		const struct screen *screen = &screens[i];
		char id[300];
		screen_id(screen, id, sizeof(id));
		const struct mode *mode = screen->now.mode;
		say("screen %s (%s): %s %dx%d@%d scale %.2f at %d,%d", screen->name, id,
			screen->now.on ? "on" : "off", mode != NULL ? mode->width : 0,
			mode != NULL ? mode->height : 0, mode != NULL ? mode->refresh : 0,
			screen->now.scale, screen->now.x, screen->now.y);
	}
	screens_changed();
}

static void manager_finished(void *data, struct zwlr_output_manager_v1 *object) {
}

static const struct zwlr_output_manager_v1_listener manager_listener = {
	.head = manager_head,
	.done = manager_done,
	.finished = manager_finished,
};

/* --- Applying ---------------------------------------------------------------- */

/* The answer to the last configuration sent: 0 none yet, 1 taken, -1 not. */
static int answer;
static void (*answered)(bool taken);

static void config_done(struct zwlr_output_configuration_v1 *config, int result) {
	zwlr_output_configuration_v1_destroy(config);
	answer = result;
	say("%s", result > 0 ? "applied" : "refused");
	if (answered != NULL) {
		answered(result > 0);
	}
}

static void config_succeeded(void *data, struct zwlr_output_configuration_v1 *config) {
	config_done(config, 1);
}

static void config_failed(void *data, struct zwlr_output_configuration_v1 *config) {
	config_done(config, -1);
}

static void config_cancelled(void *data, struct zwlr_output_configuration_v1 *config) {
	/* The screens changed meanwhile: the compositor took none of it. */
	config_done(config, -1);
}

static const struct zwlr_output_configuration_v1_listener config_listener = {
	.succeeded = config_succeeded,
	.failed = config_failed,
	.cancelled = config_cancelled,
};

static bool setting_equal(const struct setting *a, const struct setting *b) {
	return a->on == b->on && (!a->on || (a->mode == b->mode &&
		fabs(a->scale - b->scale) < 0.001 && a->x == b->x && a->y == b->y));
}

/* Whether the screens are not as `wanted` says. */
static bool screens_differ(void) {
	for (size_t i = 0; i < screen_count; i++) {
		if (!setting_equal(&screens[i].now, &screens[i].wanted)) {
			return true;
		}
	}
	return false;
}

/* Asks the compositor to set every screen as `wanted` says; the answer
 * comes to answered(). */
static void screens_apply(void) {
	answer = 0;
	struct zwlr_output_configuration_v1 *config =
		zwlr_output_manager_v1_create_configuration(manager, serial);
	zwlr_output_configuration_v1_add_listener(config, &config_listener, NULL);
	for (size_t i = 0; i < screen_count; i++) {
		const struct screen *screen = &screens[i];
		const struct setting *wanted = &screen->wanted;
		if (!wanted->on) {
			zwlr_output_configuration_v1_disable_head(config, screen->object);
			continue;
		}
		struct zwlr_output_configuration_head_v1 *head =
			zwlr_output_configuration_v1_enable_head(config, screen->object);
		if (wanted->mode != NULL) {
			zwlr_output_configuration_head_v1_set_mode(head, wanted->mode->object);
		}
		zwlr_output_configuration_head_v1_set_scale(head, wl_fixed_from_double(wanted->scale));
		zwlr_output_configuration_head_v1_set_position(head, wanted->x, wanted->y);
		/* Ours to let go of: the compositor's goes with the configuration. */
		zwlr_output_configuration_head_v1_destroy(head);
	}
	zwlr_output_configuration_v1_apply(config);
}

/* --- What is kept ------------------------------------------------------------ */

static bool kept_path(char *path, size_t size) {
	const char *config = getenv("XDG_CONFIG_HOME");
	int length = config != NULL && config[0] != '\0' ?
		snprintf(path, size, "%s/tw-monitors", config) :
		snprintf(path, size, "%s/.config/tw-monitors", getenv("HOME") ?: "");
	return length > 0 && (size_t)length < size;
}

/* The file, whole, or NULL if there is none. */
static char *kept_read(void) {
	char path[4096];
	FILE *stream = kept_path(path, sizeof(path)) ? fopen(path, "re") : NULL;
	if (stream == NULL) {
		return NULL;
	}
	char *text = NULL;
	size_t size = 0;
	FILE *into = open_memstream(&text, &size);
	char chunk[4096];
	size_t got;
	while (into != NULL && (got = fread(chunk, 1, sizeof(chunk), stream)) > 0) {
		fwrite(chunk, 1, got, into);
	}
	fclose(stream);
	if (into != NULL) {
		fclose(into);
	}
	return text;
}

/* The screen of that id among those plugged in, or NULL. */
static struct screen *screen_by_id(const char *id) {
	for (size_t i = 0; i < screen_count; i++) {
		char own[300];
		screen_id(&screens[i], own, sizeof(own));
		if (strcmp(own, id) == 0) {
			return &screens[i];
		}
	}
	return NULL;
}

/*
 * The block of `text` for the screens plugged in now: one with a line for
 * each of them and for nothing else. Returns where it starts and sets *end,
 * or NULL.
 */
static const char *kept_block(const char *text, const char **end) {
	for (const char *block = text; block != NULL && *block != '\0';) {
		size_t lines = 0;
		bool fits = true;
		const char *at = block;
		while (*at != '\0' && *at != '\n') {
			const char *line_end = strchrnul(at, '\n');
			if (*at != '#') {
				char id[300];
				size_t length = strcspn(at, "\t\n");
				snprintf(id, sizeof(id), "%.*s", (int)length, at);
				fits = fits && screen_by_id(id) != NULL;
				lines++;
			}
			at = *line_end == '\n' ? line_end + 1 : line_end;
		}
		if (fits && lines == screen_count && lines > 0) {
			*end = at;
			return block;
		}
		block = at + strspn(at, "\n");
	}
	return NULL;
}

/* Sets `wanted` from a block's lines: "<id>\ton\t<w>x<h>@<mHz>\t<scale>\t<x>,<y>"
 * or "<id>\toff". A mode the screen no longer has keeps the one it has. */
static void kept_take(const char *block, const char *end) {
	for (const char *at = block; at < end;) {
		const char *line_end = strchrnul(at, '\n');
		char line[512];
		snprintf(line, sizeof(line), "%.*s", (int)(line_end - at), at);
		at = *line_end == '\n' ? line_end + 1 : line_end;
		char *id = strtok(line, "\t"), *state = strtok(NULL, "\t");
		struct screen *screen = id != NULL && id[0] != '#' ? screen_by_id(id) : NULL;
		if (screen == NULL || state == NULL) {
			continue;
		}
		struct setting *wanted = &screen->wanted;
		wanted->on = strcmp(state, "on") == 0;
		char *mode = strtok(NULL, "\t"), *scale = strtok(NULL, "\t"), *place = strtok(NULL, "\t");
		int width, height, refresh, x, y;
		if (mode != NULL && sscanf(mode, "%dx%d@%d", &width, &height, &refresh) == 3) {
			for (size_t i = 0; i < screen->mode_count; i++) {
				const struct mode *offered = screen->modes[i];
				if (offered->width == width && offered->height == height &&
						offered->refresh == refresh) {
					wanted->mode = screen->modes[i];
				}
			}
		}
		if (scale != NULL && strtod(scale, NULL) > 0) {
			wanted->scale = strtod(scale, NULL);
		}
		if (place != NULL && sscanf(place, "%d,%d", &x, &y) == 2) {
			wanted->x = x;
			wanted->y = y;
		}
	}
}

/* Keeps how the screens are now for this set of screens: its block in the
 * file is written anew, the others are left as they were. */
static void kept_write(void) {
	char path[4096], temporary[4200];
	if (!kept_path(path, sizeof(path))) {
		return;
	}
	char *old = kept_read();
	const char *block_end = NULL;
	const char *block = old != NULL ? kept_block(old, &block_end) : NULL;
	snprintf(temporary, sizeof(temporary), "%s.%d", path, (int)getpid());
	FILE *stream = fopen(temporary, "we");
	if (stream == NULL) {
		free(old);
		return;
	}
	if (old == NULL) {
		fputs("# How tw-monitors last left the screens, for each set of screens plugged in\n"
			"# together: a block per set, a line per screen, written when a change is\n"
			"# kept. Name, on or off, mode, scale and place, between tabs.\n\n", stream);
	} else if (block == NULL) {
		fputs(old, stream);
		if (old[0] != '\0' && old[strlen(old) - 1] != '\n') {
			fputc('\n', stream);
		}
		fputc('\n', stream);
	} else {
		fwrite(old, 1, (size_t)(block - old), stream);
	}
	for (size_t i = 0; i < screen_count; i++) {
		const struct screen *screen = &screens[i];
		char id[300];
		screen_id(screen, id, sizeof(id));
		if (!screen->now.on) {
			fprintf(stream, "%s\toff\n", id);
			continue;
		}
		const struct mode *mode = screen->now.mode;
		fprintf(stream, "%s\ton\t%dx%d@%d\t%.2f\t%d,%d\n", id, mode != NULL ? mode->width : 0,
			mode != NULL ? mode->height : 0, mode != NULL ? mode->refresh : 0,
			screen->now.scale, screen->now.x, screen->now.y);
	}
	if (block != NULL) {
		fputs(block_end, stream);
	}
	free(old);
	if (fclose(stream) != 0 || rename(temporary, path) != 0) {
		unlink(temporary);
		fprintf(stderr, "tw-monitors: could not write %s\n", path);
		return;
	}
	say("kept");
}

/* --- The box ----------------------------------------------------------------- */

enum row { ROW_SCREEN, ROW_ON, ROW_MODE, ROW_SCALE, ROW_PLACE, ROWS };
static const char *const row_names[ROWS] = {"Screen", "On", "Mode", "Scale", "Place"};

static size_t picked;    /* the screen the rows are about */
static enum row row;     /* the row Left and Right change */
static bool confirming;  /* a change is applied, waiting to be kept */
static int seconds_left; /* until it goes back by itself */
static char status[128];

enum { box_height = 2 * border + 2 * padding + preview_height + padding +
	row_height * (ROWS + 1) };

static void say_status(const char *text) {
	say("status: %s", text);
	snprintf(status, sizeof(status), "%s", text);
	box_redraw();
}

/* The screens the picked one can be placed beside: those on but it. Each has
 * four sides, so place k is side k % 4 of the one k / 4. */
static const char *const sides[] = {"right of", "left of", "below", "above"};

static size_t place_count(void) {
	size_t count = 0;
	for (size_t i = 0; i < screen_count; i++) {
		count += i != picked && screens[i].wanted.on ? 4 : 0;
	}
	return count;
}

static const struct screen *place_screen(size_t place) {
	for (size_t i = 0, seen = 0; i < screen_count; i++) {
		if (i != picked && screens[i].wanted.on && seen++ == place / 4) {
			return &screens[i];
		}
	}
	return NULL;
}

/* Where the picked screen goes for place k. */
static void place_at(size_t place, int *x, int *y) {
	const struct screen *other = place_screen(place);
	int width, height, other_width, other_height;
	screen_size(&screens[picked], &screens[picked].wanted, &width, &height);
	screen_size(other, &other->wanted, &other_width, &other_height);
	const struct setting *at = &other->wanted;
	switch (place % 4) {
	case 0: *x = at->x + other_width; *y = at->y; break;
	case 1: *x = at->x - width; *y = at->y; break;
	case 2: *x = at->x; *y = at->y + other_height; break;
	default: *x = at->x; *y = at->y - height; break;
	}
}

/* Which place the picked screen is at, or -1 if none. */
static long place_now(void) {
	for (size_t place = 0; place < place_count(); place++) {
		int x, y;
		place_at(place, &x, &y);
		if (x == screens[picked].wanted.x && y == screens[picked].wanted.y) {
			return (long)place;
		}
	}
	return -1;
}

static bool any_other_on(void) {
	for (size_t i = 0; i < screen_count; i++) {
		if (i != picked && screens[i].wanted.on) {
			return true;
		}
	}
	return false;
}

static int mode_order(const void *a, const void *b) {
	const struct mode *x = *(const struct mode *const *)a, *y = *(const struct mode *const *)b;
	long area = (long)y->width * y->height - (long)x->width * x->height;
	return area != 0 ? (area > 0 ? 1 : -1) : y->refresh - x->refresh;
}

/* Changes the picked screen's setting on the current row by `by`. */
static void row_change(int by) {
	struct screen *screen = &screens[picked];
	struct setting *wanted = &screen->wanted;
	long place = place_now();
	switch (row) {
	case ROW_SCREEN:
		picked = (picked + screen_count + (size_t)(by > 0 ? 1 : screen_count - 1)) % screen_count;
		return;
	case ROW_ON:
		if (wanted->on && !any_other_on()) {
			say_status("One screen stays on.");
			return;
		}
		wanted->on = !wanted->on;
		if (wanted->on && place_count() > 0) {
			/* Back on, beside the first of the others, on its right. */
			place_at(0, &wanted->x, &wanted->y);
		}
		return;
	case ROW_MODE: {
		if (screen->mode_count == 0) {
			return;
		}
		struct mode *sorted[LENGTH(screen->modes)];
		memcpy(sorted, screen->modes, screen->mode_count * sizeof(*sorted));
		qsort(sorted, screen->mode_count, sizeof(*sorted), mode_order);
		size_t at = 0;
		for (size_t i = 0; i < screen->mode_count; i++) {
			at = sorted[i] == wanted->mode ? i : at;
		}
		at = (at + screen->mode_count + (size_t)(by > 0 ? 1 : screen->mode_count - 1)) %
			screen->mode_count;
		wanted->mode = sorted[at];
		break;
	}
	case ROW_SCALE: {
		double scale = wanted->scale;
		size_t i = by > 0 ? 0 : LENGTH(scales);
		if (by > 0) {
			while (i < LENGTH(scales) && scales[i] <= scale + 0.001) {
				i++;
			}
			scale = i < LENGTH(scales) ? scales[i] : scale;
		} else {
			while (i > 0 && scales[i - 1] >= scale - 0.001) {
				i--;
			}
			scale = i > 0 ? scales[i - 1] : scale;
		}
		wanted->scale = scale;
		break;
	}
	case ROW_PLACE: {
		size_t count = place_count();
		if (count == 0) {
			return;
		}
		long next = place < 0 ? (by > 0 ? 0 : (long)count - 1) :
			(place + (long)count + by) % (long)count;
		place_at((size_t)next, &wanted->x, &wanted->y);
		return;
	}
	default:
		return;
	}
	/* A new size keeps the screen on the same side of the same screen. */
	if (place >= 0) {
		place_at((size_t)place, &wanted->x, &wanted->y);
	}
}

/* What the row shows for the picked screen. */
static void row_value(enum row which, char *text, size_t size) {
	const struct screen *screen = &screens[picked];
	const struct setting *wanted = &screen->wanted;
	const struct mode *mode = wanted->mode;
	switch (which) {
	case ROW_SCREEN: {
		char id[300];
		screen_id(screen, id, sizeof(id));
		if (strcmp(id, screen->name) == 0) {
			snprintf(text, size, "%s", screen->name);
		} else {
			snprintf(text, size, "%s  %s", screen->name, id);
		}
		break;
	}
	case ROW_ON:
		snprintf(text, size, "%s", wanted->on ? "yes" : "no");
		break;
	case ROW_MODE:
		if (mode == NULL) {
			snprintf(text, size, "as it is");
		} else if (mode->refresh == 0) {
			snprintf(text, size, "%dx%d", mode->width, mode->height);
		} else if (mode->refresh % 1000 == 0) {
			snprintf(text, size, "%dx%d, %d Hz", mode->width, mode->height, mode->refresh / 1000);
		} else {
			snprintf(text, size, "%dx%d, %.2f Hz", mode->width, mode->height,
				mode->refresh / 1000.0);
		}
		break;
	case ROW_SCALE:
		snprintf(text, size, "%.2f", wanted->scale);
		break;
	case ROW_PLACE: {
		long place = place_now();
		if (place < 0) {
			snprintf(text, size, place_count() == 0 ? "the only one" : "where it is");
		} else {
			snprintf(text, size, "%s %s", sides[place % 4], place_screen((size_t)place)->name);
		}
		break;
	}
	default:
		text[0] = '\0';
	}
}

static int text_line(struct picture *picture, int x, int right, int baseline,
		const char *text, uint32_t colour) {
	int width = (right - x) * 64;
	size_t length = text_fit(text, width, false);
	bool cut = text[length] != '\0';
	if (cut) {
		length = text_fit(text, width - text_advance(0x2026), false);
	}
	return text_draw(picture, x, baseline, text, length, cut, colour);
}

/* The screens that are to be on, as boxes placed as they would be, scaled to
 * fit the picture's top. Where they are, in the picture's pixels, goes to
 * `boxes` for clicks. */
static struct {
	int x, y, width, height;
} boxes[MAX_SCREENS];

static void preview_draw(struct picture *picture, int left, int top, int width, int height) {
	int min_x = 0, min_y = 0, max_x = 0, max_y = 0;
	bool any = false;
	for (size_t i = 0; i < screen_count; i++) {
		const struct setting *wanted = &screens[i].wanted;
		int w, h;
		screen_size(&screens[i], wanted, &w, &h);
		if (!wanted->on || w <= 0 || h <= 0) {
			continue;
		}
		min_x = !any || wanted->x < min_x ? wanted->x : min_x;
		min_y = !any || wanted->y < min_y ? wanted->y : min_y;
		max_x = !any || wanted->x + w > max_x ? wanted->x + w : max_x;
		max_y = !any || wanted->y + h > max_y ? wanted->y + h : max_y;
		any = true;
	}
	memset(boxes, 0, sizeof(boxes));
	if (!any) {
		return;
	}
	double fit = fmin((double)width / (max_x - min_x), (double)height / (max_y - min_y));
	int offset_x = left + (width - (int)((max_x - min_x) * fit)) / 2;
	int offset_y = top + (height - (int)((max_y - min_y) * fit)) / 2;
	int lift = font_ascent() - font_line_height() / 2;
	for (size_t i = 0; i < screen_count; i++) {
		const struct setting *wanted = &screens[i].wanted;
		int w, h;
		screen_size(&screens[i], wanted, &w, &h);
		if (!wanted->on || w <= 0 || h <= 0) {
			continue;
		}
		int x = offset_x + (int)((wanted->x - min_x) * fit);
		int y = offset_y + (int)((wanted->y - min_y) * fit);
		int bw = (int)(w * fit), bh = (int)(h * fit), edge = box_px(border);
		boxes[i].x = x;
		boxes[i].y = y;
		boxes[i].width = bw;
		boxes[i].height = bh;
		picture_fill(picture, x, y, bw, bh, i == picked ? colour_selected_text : colour_border);
		picture_fill(picture, x + edge, y + edge, bw - 2 * edge, bh - 2 * edge,
			i == picked ? colour_selected : colour_screen);
		const char *name = screens[i].name;
		int name_width = 0;
		for (const char *c = name; *c != '\0';) {
			name_width += text_advance(text_character(&c));
		}
		text_line(picture, x + (bw - name_width / 64) / 2 > x ? x + (bw - name_width / 64) / 2 : x,
			x + bw, y + bh / 2 + lift, name, i == picked ? colour_selected_text : colour_text);
	}
}

static void draw(struct picture *picture) {
	font_size_set(box_px(font_size));
	int edge = box_px(border), inside = edge + box_px(padding), line = box_px(row_height);
	int right = picture->width - inside;
	picture_fill(picture, 0, 0, picture->width, picture->height, colour_border);
	picture_fill(picture, edge, edge, picture->width - 2 * edge, picture->height - 2 * edge,
		colour_background);
	if (screen_count == 0) {
		return;
	}
	preview_draw(picture, inside, inside, right - inside, box_px(preview_height));
	int lift = (line - font_line_height()) / 2 + font_ascent();
	int top = inside + box_px(preview_height) + box_px(padding);
	for (enum row which = 0; which < ROWS; which++, top += line) {
		bool here = which == row && !confirming;
		if (here) {
			picture_fill(picture, edge, top, picture->width - 2 * edge, line, colour_selected);
		}
		text_line(picture, inside, inside + box_px(label_width), top + lift, row_names[which],
			colour_label);
		char value[400], shown[420];
		row_value(which, value, sizeof(value));
		snprintf(shown, sizeof(shown), here ? "‹ %s ›" : "%s", value);
		text_line(picture, inside + box_px(label_width), right, top + lift, shown,
			here ? colour_selected_text : colour_text);
	}
	char hint[160];
	if (confirming) {
		snprintf(hint, sizeof(hint), "Keep this? Return keeps, Escape goes back (%d s)",
			seconds_left);
	} else if (status[0] != '\0') {
		snprintf(hint, sizeof(hint), "%s", status);
	} else {
		snprintf(hint, sizeof(hint), screens_differ() ? "Return applies, Escape leaves" :
			"Escape leaves");
	}
	text_line(picture, inside, right, top + lift, hint, confirming ? colour_text : colour_hint);
}

/* --- Changing, keeping, going back -------------------------------------------- */

static bool running;
static size_t shown_count; /* how many screens the box was showing */
/* Return or Escape came before the compositor answered the change: carried
 * out once it has. A configuration sent before would carry an old serial
 * and be refused. */
static void (*after_answer)(void);

static void back(void) {
	if (answer == 0) {
		after_answer = back;
		return;
	}
	for (size_t i = 0; i < screen_count; i++) {
		screens[i].wanted = screens[i].before;
	}
	confirming = false;
	box_timer(0);
	screens_apply();
	say_status("Back as it was.");
	say("back");
}

static void applied(bool taken) {
	if (!running) {
		return;
	}
	void (*then)(void) = after_answer;
	after_answer = NULL;
	if (!taken && confirming) {
		/* The change did not take: the screens are as they were. */
		confirming = false;
		box_timer(0);
		for (size_t i = 0; i < screen_count; i++) {
			screens[i].wanted = screens[i].now;
		}
		say_status("The screens would not take that.");
		return;
	}
	box_redraw();
	if (then != NULL && confirming) {
		then();
	}
}

static void apply(void) {
	if (!screens_differ()) {
		return;
	}
	for (size_t i = 0; i < screen_count; i++) {
		screens[i].before = screens[i].now;
	}
	confirming = true;
	seconds_left = keep_seconds;
	box_timer(1000);
	screens_apply();
}

static void keep(void) {
	if (answer == 0) {
		after_answer = keep;
		return;
	}
	confirming = false;
	box_timer(0);
	kept_write();
	say_status("Kept.");
}

static void tick(void) {
	if (!confirming) {
		return;
	}
	if (--seconds_left <= 0) {
		back();
		return;
	}
	box_timer(1000);
	box_redraw();
}

/* What the box shows follows the screens: one plugged in or out drops what
 * was being changed. */
static void screens_changed(void) {
	if (!running) {
		return;
	}
	if (screen_count != shown_count) {
		shown_count = screen_count;
		for (size_t i = 0; i < screen_count; i++) {
			screens[i].wanted = screens[i].now;
		}
		confirming = false;
		box_timer(0);
		picked = picked < screen_count ? picked : 0;
	}
	box_redraw();
}

static void key(xkb_keysym_t sym, const char *text, bool ctrl, bool alt) {
	status[0] = '\0';
	box_redraw();
	if (confirming) {
		if (sym == XKB_KEY_Return || sym == XKB_KEY_KP_Enter) {
			keep();
		} else if (sym == XKB_KEY_Escape) {
			back();
		}
		return;
	}
	switch (sym) {
	case XKB_KEY_Escape:
		running = false;
		box_quit();
		break;
	case XKB_KEY_Return:
	case XKB_KEY_KP_Enter:
		apply();
		break;
	case XKB_KEY_Up:
		row = row > 0 ? row - 1 : row;
		break;
	case XKB_KEY_Down:
		row = row + 1 < ROWS ? row + 1 : row;
		break;
	case XKB_KEY_Left:
		row_change(-1);
		break;
	case XKB_KEY_Right:
	case XKB_KEY_space:
		row_change(+1);
		break;
	case XKB_KEY_Tab:
	case XKB_KEY_ISO_Left_Tab: {
		enum row was = row;
		row = ROW_SCREEN;
		row_change(sym == XKB_KEY_Tab ? +1 : -1);
		row = was;
		break;
	}
	}
}

static void click(int x, int y) {
	if (confirming) {
		return;
	}
	int px = box_px(x), py = box_px(y);
	for (size_t i = 0; i < screen_count; i++) {
		if (px >= boxes[i].x && px < boxes[i].x + boxes[i].width &&
				py >= boxes[i].y && py < boxes[i].y + boxes[i].height) {
			picked = i;
			box_redraw();
			return;
		}
	}
	int top = border + padding + preview_height + padding;
	if (y >= top && y < top + ROWS * row_height) {
		row = (enum row)((y - top) / row_height);
		if (x >= padding + label_width) {
			row_change(+1);
		}
		box_redraw();
	}
}

static void scroll(int x, int y, int steps) {
	if (confirming) {
		return;
	}
	int top = border + padding + preview_height + padding;
	if (y >= top && y < top + ROWS * row_height) {
		row = (enum row)((y - top) / row_height);
		row_change(steps);
		box_redraw();
	}
}

/* The screen the box was on went (it was turned off): it opens on another. */
static void closed(void) {
	box_open();
}

static void global(struct wl_registry *registry, uint32_t name, const char *interface,
		uint32_t version) {
	if (strcmp(interface, zwlr_output_manager_v1_interface.name) == 0 && version >= 2) {
		/* 2: screens say their make, model and serial number. */
		manager = wl_registry_bind(registry, name, &zwlr_output_manager_v1_interface, 2);
		zwlr_output_manager_v1_add_listener(manager, &manager_listener, NULL);
	}
}

/* --- Main -------------------------------------------------------------------- */

/* --apply: what was kept for these screens, if anything and if it differs. */
static int restore(void) {
	char *text = kept_read();
	const char *end = NULL;
	const char *block = text != NULL ? kept_block(text, &end) : NULL;
	if (block == NULL) {
		say("nothing kept for these screens");
		free(text);
		return 0;
	}
	for (size_t i = 0; i < screen_count; i++) {
		screens[i].wanted = screens[i].now;
	}
	kept_take(block, end);
	free(text);
	if (!screens_differ()) {
		say("as kept already");
		return 0;
	}
	screens_apply();
	while (answer == 0 && box_roundtrip()) {
	}
	return answer > 0 ? 0 : 1;
}

static void list(void) {
	for (size_t i = 0; i < screen_count; i++) {
		const struct screen *screen = &screens[i];
		const struct mode *mode = screen->now.mode;
		char id[300];
		screen_id(screen, id, sizeof(id));
		if (!screen->now.on) {
			printf("%s\t%s\toff\n", screen->name, id);
		} else {
			printf("%s\t%s\ton\t%dx%d@%d\t%.2f\t%d,%d\n", screen->name, id,
				mode != NULL ? mode->width : 0, mode != NULL ? mode->height : 0,
				mode != NULL ? mode->refresh : 0, screen->now.scale, screen->now.x,
				screen->now.y);
		}
	}
}

int main(int argc, char *argv[]) {
	bool do_apply = false, do_list = false;
	for (int i = 1; i < argc; i++) {
		if (strcmp(argv[i], "-d") == 0) {
			verbose = true;
		} else if (strcmp(argv[i], "--apply") == 0) {
			do_apply = true;
		} else if (strcmp(argv[i], "--list") == 0) {
			do_list = true;
		} else {
			fprintf(stderr, "usage: %s [-d] [--apply | --list]\n"
				"Sets the screens: on or off, mode, scale, place.\n"
				"  --apply  put back what was kept for the screens plugged in now\n"
				"  --list   say how each screen is set\n"
				"  -d       say what it does, on stderr\n", argv[0]);
			return 2;
		}
	}
	static const struct box_app app = {
		.name = "monitors",
		.width = box_width,
		.height = box_height,
		.font = font,
		.draw = draw,
		.key = key,
		.click = click,
		.scroll = scroll,
		.global = global,
		.closed = closed,
		.timer = tick,
	};
	int result = 1;
	answered = applied;
	if (!box_connect(&app, verbose)) {
		box_disconnect();
		return 1;
	}
	if (manager == NULL) {
		fprintf(stderr, "tw-monitors: this compositor does not let programs set the screens\n");
	} else {
		/* The screens, and then the word that that is all of them. */
		while (!heard && box_roundtrip()) {
		}
		for (size_t i = 0; i < screen_count; i++) {
			screens[i].wanted = screens[i].now;
		}
		if (do_apply) {
			result = restore();
		} else if (do_list) {
			list();
			result = 0;
		} else {
			running = true;
			shown_count = screen_count;
			box_open();
			box_run();
			running = false;
			result = 0;
		}
		for (size_t i = 0; i < screen_count; i++) {
			screen_free(&screens[i]);
		}
		zwlr_output_manager_v1_destroy(manager);
	}
	box_disconnect();
	return result;
}
