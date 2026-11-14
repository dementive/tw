/*
 * Minimal xdg-shell client for the headless tests.
 *
 * Maps one window with a wl_shm buffer and prints every configure it gets:
 *
 *   configure <width>x<height> activated=<0|1> suspended=<0|1>
 *
 * It draws once per configure and never asks for frame callbacks, so an idle
 * compositor with these clients mapped should not wake up at all.
 */
#define _GNU_SOURCE
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>
#include <wayland-client.h>

#include "xdg-shell-client-protocol.h"

static struct wl_compositor *compositor;
static struct wl_shm *shm;
static struct xdg_wm_base *wm_base;
static struct wl_surface *surface;
static struct wl_buffer *buffer;

static int32_t pending_width, pending_height;
static bool pending_activated, pending_suspended;
static bool running = true;

static struct wl_buffer *create_buffer(int32_t width, int32_t height) {
	int32_t stride = width * 4;
	size_t size = (size_t)stride * (size_t)height;

	int fd = memfd_create("tw-test-client", MFD_CLOEXEC);
	if (fd < 0 || ftruncate(fd, (off_t)size) < 0) {
		perror("shm buffer");
		exit(1);
	}
	uint32_t *pixels = mmap(NULL, size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
	if (pixels == MAP_FAILED) {
		perror("mmap");
		exit(1);
	}
	for (size_t i = 0; i < size / 4; i++) {
		pixels[i] = 0xff336699;
	}
	munmap(pixels, size);

	struct wl_shm_pool *pool = wl_shm_create_pool(shm, fd, (int32_t)size);
	struct wl_buffer *result = wl_shm_pool_create_buffer(pool, 0, width, height, stride,
		WL_SHM_FORMAT_XRGB8888);
	wl_shm_pool_destroy(pool);
	close(fd);
	return result;
}

static void xdg_surface_configure(void *data, struct xdg_surface *xdg_surface, uint32_t serial) {
	xdg_surface_ack_configure(xdg_surface, serial);

	/* 0x0 means "pick your own size". */
	int32_t width = pending_width > 0 ? pending_width : 640;
	int32_t height = pending_height > 0 ? pending_height : 480;
	printf("configure %dx%d activated=%d suspended=%d\n",
		pending_width, pending_height, pending_activated, pending_suspended);
	fflush(stdout);

	struct wl_buffer *old = buffer;
	buffer = create_buffer(width, height);
	wl_surface_attach(surface, buffer, 0, 0);
	wl_surface_damage_buffer(surface, 0, 0, width, height);
	wl_surface_commit(surface);
	if (old != NULL) {
		wl_buffer_destroy(old);
	}
}

static const struct xdg_surface_listener xdg_surface_listener = {
	.configure = xdg_surface_configure,
};

static void toplevel_configure(void *data, struct xdg_toplevel *toplevel,
		int32_t width, int32_t height, struct wl_array *states) {
	pending_width = width;
	pending_height = height;
	pending_activated = pending_suspended = false;
	uint32_t *state;
	wl_array_for_each(state, states) {
		if (*state == XDG_TOPLEVEL_STATE_ACTIVATED) {
			pending_activated = true;
		} else if (*state == XDG_TOPLEVEL_STATE_SUSPENDED) {
			pending_suspended = true;
		}
	}
}

static void toplevel_close(void *data, struct xdg_toplevel *toplevel) {
	running = false;
}

static void toplevel_configure_bounds(void *data, struct xdg_toplevel *toplevel,
		int32_t width, int32_t height) {
}

static void toplevel_wm_capabilities(void *data, struct xdg_toplevel *toplevel,
		struct wl_array *capabilities) {
}

static const struct xdg_toplevel_listener toplevel_listener = {
	.configure = toplevel_configure,
	.close = toplevel_close,
	.configure_bounds = toplevel_configure_bounds,
	.wm_capabilities = toplevel_wm_capabilities,
};

static void wm_base_ping(void *data, struct xdg_wm_base *base, uint32_t serial) {
	xdg_wm_base_pong(base, serial);
}

static const struct xdg_wm_base_listener wm_base_listener = {
	.ping = wm_base_ping,
};

static void registry_global(void *data, struct wl_registry *registry, uint32_t name,
		const char *interface, uint32_t version) {
	if (strcmp(interface, wl_compositor_interface.name) == 0) {
		compositor = wl_registry_bind(registry, name, &wl_compositor_interface, 4);
	} else if (strcmp(interface, wl_shm_interface.name) == 0) {
		shm = wl_registry_bind(registry, name, &wl_shm_interface, 1);
	} else if (strcmp(interface, xdg_wm_base_interface.name) == 0) {
		/* Version 6 is the first with the `suspended` state. */
		wm_base = wl_registry_bind(registry, name, &xdg_wm_base_interface,
			version < 6 ? version : 6);
		xdg_wm_base_add_listener(wm_base, &wm_base_listener, NULL);
	}
}

static void registry_global_remove(void *data, struct wl_registry *registry, uint32_t name) {
}

static const struct wl_registry_listener registry_listener = {
	.global = registry_global,
	.global_remove = registry_global_remove,
};

int main(void) {
	struct wl_display *display = wl_display_connect(NULL);
	if (display == NULL) {
		fprintf(stderr, "cannot connect to a Wayland display\n");
		return 1;
	}
	struct wl_registry *registry = wl_display_get_registry(display);
	wl_registry_add_listener(registry, &registry_listener, NULL);
	wl_display_roundtrip(display);
	if (compositor == NULL || shm == NULL || wm_base == NULL) {
		fprintf(stderr, "compositor is missing wl_compositor, wl_shm or xdg_wm_base\n");
		return 1;
	}

	surface = wl_compositor_create_surface(compositor);
	struct xdg_surface *xdg_surface = xdg_wm_base_get_xdg_surface(wm_base, surface);
	xdg_surface_add_listener(xdg_surface, &xdg_surface_listener, NULL);
	struct xdg_toplevel *toplevel = xdg_surface_get_toplevel(xdg_surface);
	xdg_toplevel_add_listener(toplevel, &toplevel_listener, NULL);
	xdg_toplevel_set_title(toplevel, "tw-test-client");
	wl_surface_commit(surface);

	while (running && wl_display_dispatch(display) != -1) {
	}

	wl_display_disconnect(display);
	return 0;
}
