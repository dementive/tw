/*
 * What was copied, kept for when its program exits.
 *
 * On Wayland the clipboard is not a place. What was copied stays in the
 * program it was copied from, which hands it over when another program
 * pastes, and when that program exits the copy is gone. So tw reads every
 * copy as it is made, one format after the other, and keeps the bytes. The
 * program stays the owner of the clipboard for as long as it lives; only
 * when it goes away does tw offer what it read in the program's place,
 * until the next copy.
 *
 * Nothing here runs until something is copied or pasted: no timer, no
 * polling, and no memory but what the last copy holds, of which
 * clipboard_keep_bytes in config.h is the most.
 */
#include <errno.h>
#include <fcntl.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <wlr/types/wlr_data_device.h>
#include <wlr/types/wlr_seat.h>
#include <wlr/util/log.h>

#include "tw.h"
#include "config.h"

/* The bytes of one format. Programs offer the same text under several
 * names, so formats with the same bytes share one of these. */
struct tw_blob {
	size_t users;
	size_t size;
	unsigned char data[];
};

struct tw_format {
	char *mime;
	struct tw_blob *blob; /* NULL if it was not read */
};

/* One copy: its formats, in the order its program offered them. */
struct tw_copy {
	struct wlr_data_source source; /* in use once tw offers the copy itself */
	struct tw_server *server;
	size_t count;
	struct tw_format formats[];
};

/* A paste that did not fit into the reader's pipe in one go. */
struct tw_paste {
	struct wl_list link; /* tw_clipboard.pastes */
	struct wl_event_source *writable;
	struct tw_blob *blob;
	size_t sent;
	int fd;
};

struct tw_clipboard {
	struct tw_server *server;
	struct wl_listener set_selection;

	/* The copy a program made and still offers: `following` is the
	 * program's side of it, `copy` what tw has read of it so far. */
	struct wlr_data_source *following;
	struct tw_copy *copy;
	size_t next;  /* the format being read; `count` when all are */
	size_t bytes; /* held for this copy, the format being read included */

	/* The format being read. */
	int fd;
	struct wl_event_source *readable;
	struct tw_blob *incoming;
	size_t capacity;
	bool answered; /* something came of it: a byte, or its end */

	struct wl_event_source *takeover; /* idle: the program is gone, offer `copy` */
	struct wl_list pastes;
};

static void read_next(struct tw_clipboard *clip);

static void blob_release(struct tw_blob *blob) {
	if (blob != NULL && --blob->users == 0) {
		free(blob);
	}
}

static void copy_free(struct tw_copy *copy) {
	for (size_t i = 0; i < copy->count; i++) {
		free(copy->formats[i].mime);
		blob_release(copy->formats[i].blob);
	}
	free(copy);
}

/* Gives up on the format being read. */
static void reading_stop(struct tw_clipboard *clip) {
	if (clip->readable != NULL) {
		wl_event_source_remove(clip->readable);
		close(clip->fd);
		clip->readable = NULL;
	}
	free(clip->incoming);
	clip->incoming = NULL;
	clip->capacity = 0;
}

/* Lets go of the copy being followed and of everything read of it. */
static void forget(struct tw_clipboard *clip) {
	reading_stop(clip);
	if (clip->takeover != NULL) {
		wl_event_source_remove(clip->takeover);
		clip->takeover = NULL;
	}
	if (clip->copy != NULL) {
		copy_free(clip->copy);
		clip->copy = NULL;
	}
	clip->following = NULL;
	clip->next = 0;
	clip->bytes = 0;
}

/* The format being read has ended: keeps it and asks for the next one. */
static void format_ended(struct tw_clipboard *clip) {
	struct tw_copy *copy = clip->copy;
	struct tw_blob *blob = clip->incoming;
	clip->incoming = NULL;
	clip->capacity = 0;

	/* A format that came with nothing in it is not kept: its program had
	 * nothing to give for it, or died before it wrote. */
	if (blob != NULL) {
		struct tw_blob *same = NULL;
		for (size_t i = 0; i < clip->next && same == NULL; i++) {
			struct tw_blob *other = copy->formats[i].blob;
			if (other != NULL && other->size == blob->size &&
					memcmp(other->data, blob->data, blob->size) == 0) {
				same = other;
			}
		}
		if (same != NULL) {
			clip->bytes -= blob->size;
			free(blob);
			blob = same;
			blob->users++;
		} else {
			/* It grew by doubling: give back what it does not need. */
			struct tw_blob *fit = realloc(blob, sizeof(*blob) + blob->size);
			if (fit != NULL) {
				blob = fit;
			}
		}
		copy->formats[clip->next].blob = blob;
	}
	clip->next++;
	read_next(clip);
}

static int handle_readable(int fd, uint32_t mask, void *data) {
	struct tw_clipboard *clip = data;
	struct tw_blob *blob = clip->incoming;
	clip->answered = true;
	if (blob == NULL || blob->size == clip->capacity) {
		/* Doubling, but never past one byte more than the copy may hold:
		 * that byte is how a copy that is too large shows itself. */
		size_t size = blob != NULL ? blob->size : 0;
		size_t most = size + (clipboard_keep_bytes - clip->bytes) + 1;
		size_t capacity = clip->capacity == 0 ? 4096 : clip->capacity * 2;
		if (capacity > most) {
			capacity = most;
		}
		blob = realloc(clip->incoming, sizeof(*blob) + capacity);
		if (blob == NULL) {
			wlr_log(WLR_ERROR, "clipboard: out of memory, the copy stays with its program");
			forget(clip);
			return 0;
		}
		if (clip->incoming == NULL) {
			blob->users = 1;
			blob->size = 0;
		}
		clip->incoming = blob;
		clip->capacity = capacity;
	}

	ssize_t n = read(fd, blob->data + blob->size, clip->capacity - blob->size);
	if (n > 0) {
		blob->size += (size_t)n;
		clip->bytes += (size_t)n;
		if (clip->bytes > clipboard_keep_bytes) {
			wlr_log(WLR_DEBUG, "clipboard: more than %zu bytes, left with its program",
				(size_t)clipboard_keep_bytes);
			forget(clip);
		}
		return 0;
	}
	if (n < 0 && (errno == EAGAIN || errno == EINTR)) {
		return 0;
	}

	/* The other end was closed: that is how a program says it has written
	 * everything. (It is also what is left of one that died halfway, see
	 * handle_set_selection().) */
	wl_event_source_remove(clip->readable);
	close(fd);
	clip->readable = NULL;
	if (n < 0 && blob->size > 0) {
		clip->bytes -= blob->size;
		blob->size = 0;
	}
	if (blob->size == 0) {
		free(blob);
		clip->incoming = NULL;
	}
	format_ended(clip);
	return 0;
}

/* Asks the program for the next format it offers. */
static void read_next(struct tw_clipboard *clip) {
	struct tw_copy *copy = clip->copy;
	if (clip->next == copy->count) {
		size_t kept = 0;
		for (size_t i = 0; i < copy->count; i++) {
			kept += copy->formats[i].blob != NULL;
		}
		wlr_log(WLR_DEBUG, "clipboard: kept %zu formats in %zu bytes", kept, clip->bytes);
		return;
	}
	/* Only tw's end must not block. The other end is the program's, to
	 * write to as it is used to. */
	int fds[2];
	if (pipe(fds) < 0) {
		fds[0] = -1;
	} else if (fcntl(fds[0], F_SETFD, FD_CLOEXEC) == 0 &&
			fcntl(fds[0], F_SETFL, O_NONBLOCK) == 0) {
		clip->readable = wl_event_loop_add_fd(clip->server->loop, fds[0],
			WL_EVENT_READABLE, handle_readable, clip);
	}
	if (clip->readable == NULL) {
		/* Taking what was read so far would take the format before as
		 * whole on no evidence (see handle_set_selection()). */
		wlr_log_errno(WLR_ERROR, "clipboard: cannot read the copy, it stays with its program");
		if (fds[0] >= 0) {
			close(fds[0]);
			close(fds[1]);
		}
		forget(clip);
		return;
	}
	clip->fd = fds[0];
	clip->answered = false;
	/* This closes the other end once the request is on its way. */
	wlr_data_source_send(clip->following, copy->formats[clip->next].mime, fds[1]);
}

/* Starts reading a copy a program has just made. */
static void follow(struct tw_clipboard *clip, struct wlr_data_source *source) {
	size_t count = source->mime_types.size / sizeof(char *);
	char **mime;
	/* A password manager marks what it copies as secret, and empties the
	 * clipboard again soon after. Kept here, it would outlive that. */
	wl_array_for_each(mime, &source->mime_types) {
		if (strcmp(*mime, "x-kde-passwordManagerHint") == 0) {
			wlr_log(WLR_DEBUG, "clipboard: a secret, left with its program");
			return;
		}
	}
	if (count == 0) {
		return;
	}
	struct tw_copy *copy = calloc(1, sizeof(*copy) + count * sizeof(copy->formats[0]));
	if (copy == NULL) {
		return;
	}
	copy->server = clip->server;
	wl_array_for_each(mime, &source->mime_types) {
		copy->formats[copy->count].mime = strdup(*mime);
		if (copy->formats[copy->count].mime == NULL) {
			copy_free(copy);
			return;
		}
		copy->count++;
	}
	clip->copy = copy;
	clip->following = source;
	read_next(clip);
}

/* --- Offering a copy whose program is gone ---------------------------------- */

/* Writes what the reader has room for right now, in one go and no more, so
 * that a long paste to a fast reader does not keep tw from everything else.
 * True when there is no more to do: all is written, or the reader left. */
static bool paste_write(int fd, const struct tw_blob *blob, size_t *sent) {
	ssize_t n;
	do {
		n = write(fd, blob->data + *sent, blob->size - *sent);
	} while (n < 0 && errno == EINTR);
	if (n < 0) {
		return errno != EAGAIN;
	}
	*sent += (size_t)n;
	return *sent == blob->size;
}

static void paste_end(struct tw_paste *paste) {
	wl_list_remove(&paste->link);
	wl_event_source_remove(paste->writable);
	close(paste->fd);
	blob_release(paste->blob);
	free(paste);
}

static int handle_writable(int fd, uint32_t mask, void *data) {
	struct tw_paste *paste = data;
	if ((mask & (WL_EVENT_HANGUP | WL_EVENT_ERROR)) != 0 ||
			paste_write(fd, paste->blob, &paste->sent)) {
		paste_end(paste);
	}
	return 0;
}

/* A program pastes from the copy tw offers. */
static void copy_send(struct wlr_data_source *source, const char *mime, int32_t fd) {
	struct tw_copy *copy = wl_container_of(source, copy, source);
	struct tw_clipboard *clip = copy->server->clipboard;
	struct tw_blob *blob = NULL;
	for (size_t i = 0; i < copy->count && blob == NULL; i++) {
		if (copy->formats[i].blob != NULL && strcmp(copy->formats[i].mime, mime) == 0) {
			blob = copy->formats[i].blob;
		}
	}
	/* tw must never wait for a program to read, so its end does not block.
	 * A short text is written right here and that is all. */
	size_t sent = 0;
	int flags = fcntl(fd, F_GETFL);
	if (blob == NULL || flags < 0 || fcntl(fd, F_SETFL, flags | O_NONBLOCK) < 0 ||
			paste_write(fd, blob, &sent)) {
		close(fd);
		return;
	}
	/* More than the reader takes at once: the rest follows as it reads. */
	struct tw_paste *paste = calloc(1, sizeof(*paste));
	if (paste != NULL) {
		paste->writable = wl_event_loop_add_fd(copy->server->loop, fd,
			WL_EVENT_WRITABLE, handle_writable, paste);
	}
	if (paste == NULL || paste->writable == NULL) {
		free(paste);
		close(fd);
		return;
	}
	paste->fd = fd;
	paste->blob = blob;
	paste->sent = sent;
	blob->users++;
	wl_list_insert(&clip->pastes, &paste->link);
}

/* Another copy took its place, a program emptied the clipboard, or tw is
 * leaving. From the moment tw offers a copy the seat owns it. */
static void copy_destroy(struct wlr_data_source *source) {
	struct tw_copy *copy = wl_container_of(source, copy, source);
	copy_free(copy);
}

static const struct wlr_data_source_impl copy_impl = {
	.send = copy_send,
	.destroy = copy_destroy,
};

/* The program that made the copy is gone: tw offers what it read of it. */
static void handle_takeover(void *data) {
	struct tw_clipboard *clip = data;
	struct tw_copy *copy = clip->copy;
	clip->takeover = NULL;
	clip->copy = NULL;
	clip->next = 0;
	clip->bytes = 0;

	wlr_data_source_init(&copy->source, &copy_impl);
	size_t offered = 0;
	for (size_t i = 0; i < copy->count; i++) {
		if (copy->formats[i].blob == NULL) {
			continue;
		}
		/* The seat frees these names with the source. */
		char *name = strdup(copy->formats[i].mime);
		char **slot = name != NULL ?
			wl_array_add(&copy->source.mime_types, sizeof(*slot)) : NULL;
		if (slot == NULL) {
			free(name);
			break;
		}
		*slot = name;
		offered++;
	}
	wlr_log(WLR_DEBUG, "clipboard: its program is gone, offering %zu formats", offered);
	/* With the serial of the copy it stands in for, which the seat still
	 * holds: a copy that would have replaced the original replaces this.
	 * A new serial would refuse one a program makes later than the input
	 * event it gives (a screenshot tool when its capture is done). */
	struct wlr_seat *seat = clip->server->seat;
	wlr_seat_set_selection(seat, &copy->source, seat->selection_serial);
}

/* --- What is on the clipboard changed --------------------------------------- */

static void handle_set_selection(struct wl_listener *listener, void *data) {
	struct tw_clipboard *clip = wl_container_of(listener, clip, set_selection);
	struct wlr_data_source *source = clip->server->seat->selection_source;
	if (source != NULL && source->impl == &copy_impl) {
		return; /* tw's own offer */
	}
	if (source != NULL) {
		forget(clip);
		follow(clip, source);
		return;
	}
	/*
	 * Nothing is on the clipboard any more. If a copy was being followed,
	 * its program is gone, or dropped the copy without saying that it
	 * wants the clipboard empty (clipboard_cleared() comes before this
	 * when it does say so). What was read of the copy is offered again,
	 * once every program has been told that the clipboard is empty.
	 *
	 * Not the format on its way: whether it was all written cannot be
	 * told. Nor always the one read before it. A pipe ends the same way
	 * whether its program closed it or died over it, and tw may hear of
	 * the pipe before it hears of the connection (the kernel closes a
	 * dying program's files in the order of their numbers, but in which
	 * order tw is told is not certain). Then it took a format cut short
	 * for a whole one and asked for the next. What tells the two apart is
	 * that a program that died never reacts to that next request. So the
	 * newest format read counts only if something came of the one after
	 * it. The last format a program offers has none after it and is taken
	 * as it is.
	 */
	struct tw_copy *copy = clip->copy;
	if (copy != NULL && clip->next > 0 && clip->next < copy->count && !clip->answered) {
		blob_release(copy->formats[clip->next - 1].blob);
		copy->formats[clip->next - 1].blob = NULL;
	}
	reading_stop(clip);
	clip->following = NULL;
	bool any = false;
	for (size_t i = 0; copy != NULL && i < copy->count; i++) {
		any = any || copy->formats[i].blob != NULL;
	}
	if (any) {
		clip->takeover = wl_event_loop_add_idle(clip->server->loop, handle_takeover, clip);
	}
	if (clip->takeover == NULL) {
		forget(clip);
	}
}

void clipboard_cleared(struct tw_server *server) {
	if (server->clipboard != NULL) {
		forget(server->clipboard);
	}
}

void clipboard_init(struct tw_server *server) {
	if (clipboard_keep_bytes == 0) {
		return;
	}
	struct tw_clipboard *clip = calloc(1, sizeof(*clip));
	if (clip == NULL) {
		return;
	}
	clip->server = server;
	wl_list_init(&clip->pastes);
	clip->set_selection.notify = handle_set_selection;
	wl_signal_add(&server->seat->events.set_selection, &clip->set_selection);
	server->clipboard = clip;
}

void clipboard_finish(struct tw_server *server) {
	struct tw_clipboard *clip = server->clipboard;
	if (clip == NULL) {
		return;
	}
	wl_list_remove(&clip->set_selection.link);
	forget(clip);
	struct tw_paste *paste, *tmp;
	wl_list_for_each_safe(paste, tmp, &clip->pastes, link) {
		paste_end(paste);
	}
	server->clipboard = NULL;
	free(clip);
}
