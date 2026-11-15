/*
 * Copies and pastes the way a clipboard tool does, through the data-control
 * protocol, and says what happens to it.
 *
 *   tw-test-clip copy text/plain=hello image/png=@3000000
 *   tw-test-clip paste text/plain
 *   tw-test-clip clear
 *
 * copy offers each format with the content after its '=' and serves it until
 * it is killed or another copy takes its place ("cancelled"). It prints
 * "content <format>: ..." for each, "ready" once the copy is made, and
 * "sent <format>" each time one has been read. A content can also be:
 *
 *   @N       N bytes, the alphabet over and over
 *   %hang    never answered: whoever reads it is kept waiting ("asked")
 *   %die:N   N bytes are written and then the program exits at once, as one
 *            that crashes halfway does
 *   %die0:N  the same, with the pipe at a lower number than the connection
 *            to the compositor, so that the kernel closes the pipe first
 *
 * paste prints "formats: a b c" and then "paste <format>: ..." in the words
 * of copy's "content" line: the text itself, or for more than 100 bytes
 * their number and sum. Without a format it prints the formats alone. It
 * prints "nothing" when the clipboard is empty and "no <format>" when the
 * copy lacks that format. Two readers that misbehave:
 *
 *   paste-slow <format>   waits a second before it reads
 *   paste-part <format>   reads the first bytes and closes ("left after ...")
 *   paste-none <format>   closes before anything can be written ("left")
 */
#define _GNU_SOURCE
#include <errno.h>
#include <signal.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <wayland-client.h>

#include "wlr-data-control-unstable-v1-client-protocol.h"

#define MAX_FORMATS 32

static struct wl_display *display;
static struct wl_seat *seat;
static struct zwlr_data_control_manager_v1 *manager;

/* copy: what is offered. */
static struct {
	const char *mime;
	const char *content;
} formats[MAX_FORMATS];
static int format_count;

/* paste: what is on the clipboard. */
static struct zwlr_data_control_offer_v1 *selection;
static char offered[MAX_FORMATS][128];
static int offered_count;

static void say(const char *line) {
	printf("%s\n", line);
	fflush(stdout);
}

/* The bytes a content stands for. */
static unsigned char *content_bytes(const char *content, size_t *size) {
	unsigned long count;
	if (sscanf(content, "@%lu", &count) == 1 || sscanf(content, "%%die:%lu", &count) == 1 ||
			sscanf(content, "%%die0:%lu", &count) == 1) {
		unsigned char *bytes = malloc(count + 1);
		for (size_t i = 0; i < count; i++) {
			bytes[i] = (unsigned char)('a' + i % 26);
		}
		*size = count;
		return bytes;
	}
	*size = strlen(content);
	return (unsigned char *)strdup(content);
}

/* "<what> <format>: <the text>", or the number of bytes and their sum. */
static void describe(const char *what, const char *mime, const unsigned char *bytes, size_t size) {
	if (size <= 100) {
		printf("%s %s: %.*s\n", what, mime, (int)size, (const char *)bytes);
	} else {
		uint32_t sum = 0;
		for (size_t i = 0; i < size; i++) {
			sum += bytes[i];
		}
		printf("%s %s: %zu bytes, sum %u\n", what, mime, size, sum);
	}
	fflush(stdout);
}

/* --- copy ------------------------------------------------------------------- */

static void source_send(void *data, struct zwlr_data_control_source_v1 *source,
		const char *mime, int32_t fd) {
	const char *content = NULL;
	for (int i = 0; i < format_count; i++) {
		if (strcmp(formats[i].mime, mime) == 0) {
			content = formats[i].content;
		}
	}
	char line[256];
	if (content == NULL) {
		close(fd);
		return;
	}
	if (strcmp(content, "%hang") == 0) {
		snprintf(line, sizeof(line), "asked %s", mime);
		say(line);
		return; /* and the pipe stays open */
	}
	if (strncmp(content, "%die0:", 6) == 0) {
		dup2(fd, STDIN_FILENO);
		close(fd);
		fd = STDIN_FILENO;
	}
	size_t size, written = 0;
	unsigned char *bytes = content_bytes(content, &size);
	while (written < size) {
		ssize_t n = write(fd, bytes + written, size - written);
		if (n < 0 && errno == EINTR) {
			continue;
		}
		if (n < 0) {
			break;
		}
		written += (size_t)n;
	}
	if (strncmp(content, "%die", 4) == 0) {
		_exit(0);
	}
	free(bytes);
	close(fd);
	snprintf(line, sizeof(line), "%s %s", written == size ? "sent" : "refused", mime);
	say(line);
}

static void source_cancelled(void *data, struct zwlr_data_control_source_v1 *source) {
	say("cancelled");
	exit(0);
}

static const struct zwlr_data_control_source_v1_listener source_listener = {
	.send = source_send,
	.cancelled = source_cancelled,
};

static int copy(struct zwlr_data_control_device_v1 *device, int argc, char *argv[]) {
	struct zwlr_data_control_source_v1 *source =
		zwlr_data_control_manager_v1_create_data_source(manager);
	zwlr_data_control_source_v1_add_listener(source, &source_listener, NULL);
	for (int i = 0; i < argc && format_count < MAX_FORMATS; i++) {
		/* The format may hold a '=' itself (text/plain;charset=utf-8), so
		 * the content starts after the last one. */
		char *equals = strrchr(argv[i], '=');
		if (equals == NULL) {
			fprintf(stderr, "copy: %s is not <format>=<content>\n", argv[i]);
			return 1;
		}
		*equals = '\0';
		formats[format_count].mime = argv[i];
		formats[format_count].content = equals + 1;
		format_count++;
		zwlr_data_control_source_v1_offer(source, argv[i]);
		if (equals[1] != '%') {
			size_t size;
			unsigned char *bytes = content_bytes(equals + 1, &size);
			describe("content", argv[i], bytes, size);
			free(bytes);
		}
	}
	zwlr_data_control_device_v1_set_selection(device, source);
	wl_display_roundtrip(display);
	say("ready");
	while (wl_display_dispatch(display) != -1) {
	}
	return 0;
}

/* --- paste ------------------------------------------------------------------ */

static void offer_offer(void *data, struct zwlr_data_control_offer_v1 *offer, const char *mime) {
	if (offered_count < MAX_FORMATS) {
		snprintf(offered[offered_count++], sizeof(offered[0]), "%s", mime);
	}
}

static const struct zwlr_data_control_offer_v1_listener offer_listener = {
	.offer = offer_offer,
};

static void device_data_offer(void *data, struct zwlr_data_control_device_v1 *device,
		struct zwlr_data_control_offer_v1 *offer) {
	offered_count = 0;
	zwlr_data_control_offer_v1_add_listener(offer, &offer_listener, NULL);
}

static void device_selection(void *data, struct zwlr_data_control_device_v1 *device,
		struct zwlr_data_control_offer_v1 *offer) {
	selection = offer;
}

static void device_finished(void *data, struct zwlr_data_control_device_v1 *device) {
}

static const struct zwlr_data_control_device_v1_listener device_listener = {
	.data_offer = device_data_offer,
	.selection = device_selection,
	.finished = device_finished,
};

static int paste(const char *mode, const char *mime) {
	if (selection == NULL) {
		say("nothing");
		return 0;
	}
	printf("formats:");
	bool has = false;
	for (int i = 0; i < offered_count; i++) {
		printf(" %s", offered[i]);
		has = has || (mime != NULL && strcmp(offered[i], mime) == 0);
	}
	printf("\n");
	fflush(stdout);
	if (mime == NULL) {
		return 0;
	}
	if (!has) {
		printf("no %s\n", mime);
		return 0;
	}

	int fds[2];
	if (pipe(fds) < 0) {
		perror("pipe");
		return 1;
	}
	zwlr_data_control_offer_v1_receive(selection, mime, fds[1]);
	close(fds[1]);
	if (strcmp(mode, "paste-none") == 0) {
		close(fds[0]);
		wl_display_roundtrip(display);
		say("left");
		return 0;
	}
	wl_display_roundtrip(display);
	if (strcmp(mode, "paste-slow") == 0) {
		nanosleep(&(struct timespec){.tv_sec = 1}, NULL);
	}

	size_t size = 0, capacity = 1 << 16;
	unsigned char *bytes = malloc(capacity);
	for (;;) {
		if (size == capacity) {
			bytes = realloc(bytes, capacity *= 2);
		}
		ssize_t n = read(fds[0], bytes + size, capacity - size);
		if (n < 0 && errno == EINTR) {
			continue;
		}
		if (n <= 0) {
			break;
		}
		size += (size_t)n;
		if (strcmp(mode, "paste-part") == 0) {
			close(fds[0]);
			wl_display_roundtrip(display);
			printf("left after %zu bytes\n", size);
			free(bytes);
			return 0;
		}
	}
	describe("paste", mime, bytes, size);
	free(bytes);
	return 0;
}

/* ---------------------------------------------------------------------------- */

static void registry_global(void *data, struct wl_registry *registry, uint32_t name,
		const char *interface, uint32_t version) {
	if (strcmp(interface, wl_seat_interface.name) == 0) {
		seat = wl_registry_bind(registry, name, &wl_seat_interface, 1);
	} else if (strcmp(interface, zwlr_data_control_manager_v1_interface.name) == 0) {
		/* Version 1 knows the clipboard alone, not the primary selection. */
		manager = wl_registry_bind(registry, name, &zwlr_data_control_manager_v1_interface, 1);
	}
}

static void registry_global_remove(void *data, struct wl_registry *registry, uint32_t name) {
}

static const struct wl_registry_listener registry_listener = {
	.global = registry_global,
	.global_remove = registry_global_remove,
};

int main(int argc, char *argv[]) {
	if (argc < 2) {
		fprintf(stderr, "usage: %s copy <format>=<content>... | paste [format] | clear\n", argv[0]);
		return 2;
	}
	const char *mode = argv[1];
	/* A reader that leaves early must not kill the program that copied. */
	signal(SIGPIPE, SIG_IGN);

	display = wl_display_connect(NULL);
	if (display == NULL) {
		fprintf(stderr, "cannot connect to the compositor\n");
		return 1;
	}
	struct wl_registry *registry = wl_display_get_registry(display);
	wl_registry_add_listener(registry, &registry_listener, NULL);
	wl_display_roundtrip(display);
	if (seat == NULL || manager == NULL) {
		fprintf(stderr, "the compositor has no seat or no data-control protocol\n");
		return 1;
	}
	struct zwlr_data_control_device_v1 *device =
		zwlr_data_control_manager_v1_get_data_device(manager, seat);
	zwlr_data_control_device_v1_add_listener(device, &device_listener, NULL);
	/* What is on the clipboard now arrives in answer to that. */
	wl_display_roundtrip(display);

	if (strcmp(mode, "copy") == 0 && argc > 2) {
		return copy(device, argc - 2, argv + 2);
	}
	if (strcmp(mode, "clear") == 0) {
		zwlr_data_control_device_v1_set_selection(device, NULL);
		wl_display_roundtrip(display);
		say("cleared");
		return 0;
	}
	if (strncmp(mode, "paste", 5) == 0) {
		return paste(mode, argc > 2 ? argv[2] : NULL);
	}
	fprintf(stderr, "%s: unknown mode %s\n", argv[0], mode);
	return 2;
}
