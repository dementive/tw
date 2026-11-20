/*
 * Lists the windows and records one, the way a screen recorder does through
 * xdg-desktop-portal-wlr (ext-foreign-toplevel-list, ext-image-copy-capture),
 * and says what it sees.
 *
 *   tw-test-capture list     "window <app_id> <title>" for each window, as
 *                            it appears or is renamed ("-" for no app_id),
 *                            and "closed <title>" as one goes
 *   tw-test-capture record   records the first window listed: "frame <w>x<h>
 *                            <rrggbb>" for each frame, with the colour of
 *                            its middle pixel, and "stopped" if the
 *                            recording ends (the window went)
 *
 * Both run until they are killed.
 */
#define _GNU_SOURCE
#include <stdarg.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>
#include <wayland-client.h>

#include "ext-foreign-toplevel-list-v1-client-protocol.h"
#include "ext-image-capture-source-v1-client-protocol.h"
#include "ext-image-copy-capture-v1-client-protocol.h"

static struct wl_shm *shm;
static struct ext_foreign_toplevel_list_v1 *list;
static struct ext_foreign_toplevel_image_capture_source_manager_v1 *sources;
static struct ext_image_copy_capture_manager_v1 *copier;
static bool record;

/* The window being recorded, and the buffer its frames are copied into. */
static struct ext_image_copy_capture_session_v1 *session;
static int32_t width, height;
static uint32_t format = UINT32_MAX;
static struct wl_buffer *buffer;
static uint32_t *pixels;
static size_t size;

struct window {
	char title[256];
	char app_id[256];
};

static void say(const char *format_text, ...) __attribute__((format(printf, 1, 2)));
static void say(const char *format_text, ...) {
	va_list args;
	va_start(args, format_text);
	vprintf(format_text, args);
	va_end(args);
	putchar('\n');
	fflush(stdout);
}

/* --- Recording --------------------------------------------------------------- */

static void capture_frame(void);

static void frame_transform(void *data, struct ext_image_copy_capture_frame_v1 *frame,
		uint32_t transform) {
}

static void frame_damage(void *data, struct ext_image_copy_capture_frame_v1 *frame,
		int32_t x, int32_t y, int32_t w, int32_t h) {
}

static void frame_time(void *data, struct ext_image_copy_capture_frame_v1 *frame,
		uint32_t sec_hi, uint32_t sec_lo, uint32_t nsec) {
}

static void frame_ready(void *data, struct ext_image_copy_capture_frame_v1 *frame) {
	ext_image_copy_capture_frame_v1_destroy(frame);
	uint32_t middle = pixels[(size_t)(height / 2) * (size_t)width + (size_t)(width / 2)];
	say("frame %dx%d %06x", width, height, middle & 0xffffff);
	/* As a recorder does: ask for the next one straight away. */
	capture_frame();
}

static void frame_failed(void *data, struct ext_image_copy_capture_frame_v1 *frame,
		uint32_t reason) {
	ext_image_copy_capture_frame_v1_destroy(frame);
	say("frame failed %u", reason);
}

static const struct ext_image_copy_capture_frame_v1_listener frame_listener = {
	.transform = frame_transform,
	.damage = frame_damage,
	.presentation_time = frame_time,
	.ready = frame_ready,
	.failed = frame_failed,
};

static void capture_frame(void) {
	struct ext_image_copy_capture_frame_v1 *frame =
		ext_image_copy_capture_session_v1_create_frame(session);
	ext_image_copy_capture_frame_v1_add_listener(frame, &frame_listener, NULL);
	ext_image_copy_capture_frame_v1_attach_buffer(frame, buffer);
	ext_image_copy_capture_frame_v1_damage_buffer(frame, 0, 0, width, height);
	ext_image_copy_capture_frame_v1_capture(frame);
}

static void session_size(void *data, struct ext_image_copy_capture_session_v1 *s,
		uint32_t w, uint32_t h) {
	width = (int32_t)w;
	height = (int32_t)h;
}

static void session_shm_format(void *data, struct ext_image_copy_capture_session_v1 *s,
		uint32_t shm_format) {
	if (format == UINT32_MAX || shm_format == WL_SHM_FORMAT_XRGB8888) {
		format = shm_format;
	}
}

static void session_dmabuf_device(void *data, struct ext_image_copy_capture_session_v1 *s,
		struct wl_array *device) {
}

static void session_dmabuf_format(void *data, struct ext_image_copy_capture_session_v1 *s,
		uint32_t dmabuf_format, struct wl_array *modifiers) {
}

/* What the frames are to be like is known: a buffer for them, and the first. */
static void session_done(void *data, struct ext_image_copy_capture_session_v1 *s) {
	if (buffer != NULL || width <= 0 || height <= 0 || format == UINT32_MAX) {
		return;
	}
	size = (size_t)width * (size_t)height * 4;
	int fd = memfd_create("tw-test-capture", MFD_CLOEXEC);
	if (fd < 0 || ftruncate(fd, (off_t)size) < 0 ||
			(pixels = mmap(NULL, size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0)) == MAP_FAILED) {
		perror("shm buffer");
		exit(1);
	}
	struct wl_shm_pool *pool = wl_shm_create_pool(shm, fd, (int32_t)size);
	buffer = wl_shm_pool_create_buffer(pool, 0, width, height, width * 4, format);
	wl_shm_pool_destroy(pool);
	close(fd);
	capture_frame();
}

static void session_stopped(void *data, struct ext_image_copy_capture_session_v1 *s) {
	say("stopped");
}

static const struct ext_image_copy_capture_session_v1_listener session_listener = {
	.buffer_size = session_size,
	.shm_format = session_shm_format,
	.dmabuf_device = session_dmabuf_device,
	.dmabuf_format = session_dmabuf_format,
	.done = session_done,
	.stopped = session_stopped,
};

/* --- The window list --------------------------------------------------------- */

static void window_closed(void *data, struct ext_foreign_toplevel_handle_v1 *handle) {
	struct window *window = data;
	say("closed %s", window->title);
	ext_foreign_toplevel_handle_v1_destroy(handle);
	free(window);
}

static void window_done(void *data, struct ext_foreign_toplevel_handle_v1 *handle) {
	struct window *window = data;
	say("window %s %s", window->app_id[0] != '\0' ? window->app_id : "-", window->title);
	if (record && session == NULL) {
		struct ext_image_capture_source_v1 *source =
			ext_foreign_toplevel_image_capture_source_manager_v1_create_source(sources, handle);
		session = ext_image_copy_capture_manager_v1_create_session(copier, source, 0);
		ext_image_copy_capture_session_v1_add_listener(session, &session_listener, NULL);
	}
}

static void window_title(void *data, struct ext_foreign_toplevel_handle_v1 *handle,
		const char *title) {
	struct window *window = data;
	snprintf(window->title, sizeof(window->title), "%s", title);
}

static void window_app_id(void *data, struct ext_foreign_toplevel_handle_v1 *handle,
		const char *app_id) {
	struct window *window = data;
	snprintf(window->app_id, sizeof(window->app_id), "%s", app_id);
}

static void window_identifier(void *data, struct ext_foreign_toplevel_handle_v1 *handle,
		const char *identifier) {
}

static const struct ext_foreign_toplevel_handle_v1_listener window_listener = {
	.closed = window_closed,
	.done = window_done,
	.title = window_title,
	.app_id = window_app_id,
	.identifier = window_identifier,
};

static void list_toplevel(void *data, struct ext_foreign_toplevel_list_v1 *l,
		struct ext_foreign_toplevel_handle_v1 *handle) {
	struct window *window = calloc(1, sizeof(*window));
	if (window == NULL) {
		exit(1);
	}
	ext_foreign_toplevel_handle_v1_add_listener(handle, &window_listener, window);
}

static void list_finished(void *data, struct ext_foreign_toplevel_list_v1 *l) {
}

static const struct ext_foreign_toplevel_list_v1_listener list_listener = {
	.toplevel = list_toplevel,
	.finished = list_finished,
};

static void registry_global(void *data, struct wl_registry *registry, uint32_t name,
		const char *interface, uint32_t version) {
	if (strcmp(interface, wl_shm_interface.name) == 0) {
		shm = wl_registry_bind(registry, name, &wl_shm_interface, 1);
	} else if (strcmp(interface, ext_foreign_toplevel_list_v1_interface.name) == 0) {
		list = wl_registry_bind(registry, name, &ext_foreign_toplevel_list_v1_interface, 1);
	} else if (strcmp(interface,
			ext_foreign_toplevel_image_capture_source_manager_v1_interface.name) == 0) {
		sources = wl_registry_bind(registry, name,
			&ext_foreign_toplevel_image_capture_source_manager_v1_interface, 1);
	} else if (strcmp(interface, ext_image_copy_capture_manager_v1_interface.name) == 0) {
		copier = wl_registry_bind(registry, name, &ext_image_copy_capture_manager_v1_interface, 1);
	}
}

static void registry_global_remove(void *data, struct wl_registry *registry, uint32_t name) {
}

static const struct wl_registry_listener registry_listener = {
	.global = registry_global,
	.global_remove = registry_global_remove,
};

int main(int argc, char *argv[]) {
	if (argc != 2 || (strcmp(argv[1], "list") != 0 && strcmp(argv[1], "record") != 0)) {
		fprintf(stderr, "usage: %s list|record\n", argv[0]);
		return 1;
	}
	record = strcmp(argv[1], "record") == 0;
	struct wl_display *display = wl_display_connect(NULL);
	if (display == NULL) {
		fprintf(stderr, "cannot connect to a Wayland display\n");
		return 1;
	}
	struct wl_registry *registry = wl_display_get_registry(display);
	wl_registry_add_listener(registry, &registry_listener, NULL);
	wl_display_roundtrip(display);
	if (shm == NULL || list == NULL || sources == NULL || copier == NULL) {
		fprintf(stderr, "compositor is missing a required global\n");
		return 1;
	}
	ext_foreign_toplevel_list_v1_add_listener(list, &list_listener, NULL);
	while (wl_display_dispatch(display) != -1) {
	}
	return 1;
}
