/*
 * tw-notify: shows the notifications programs send over D-Bus
 * (org.freedesktop.Notifications) as a list in the top right corner.
 *
 * It only runs while there is something to show. The bus starts it when a
 * program sends a notification or asks what it can do, and it leaves when
 * the last notification has gone, so the rest of the time it costs nothing.
 * To leave without losing a notification that is on its way, it first gives
 * up its name on the bus and waits for the answer: whatever was sent before
 * that has arrived by then, and whatever is sent after starts a new one.
 *
 * The D-Bus messages are read and written by bus.c, with no library; the
 * text is drawn with fontconfig and FreeType. No icons, no buttons, no
 * markup, no sound: a title, a few lines of text, a click.
 */
#define _GNU_SOURCE
#include <errno.h>
#include <fontconfig/fontconfig.h>
#include <ft2build.h>
#include FT_FREETYPE_H
#include <limits.h>
#include <linux/input-event-codes.h>
#include <poll.h>
#include <stdarg.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <time.h>
#include <unistd.h>
#include <wayland-client.h>

#include "bus.h"
#include "config.h"
#include "fractional-scale-v1-client-protocol.h"
#include "viewporter-client-protocol.h"
#include "wlr-layer-shell-unstable-v1-client-protocol.h"
#include "xdg-activation-v1-client-protocol.h"

#define LENGTH(a) (sizeof(a) / sizeof((a)[0]))
#define NAME "org.freedesktop.Notifications"
#define PATH "/org/freedesktop/Notifications"

/* Why a notification closed, as the specification numbers it. */
enum { CLOSED_EXPIRED = 1, CLOSED_DISMISSED = 2, CLOSED_BY_CALL = 3 };

struct note {
	uint32_t id;
	int64_t deadline;  /* on the monotonic clock, in ms; 0: stays until closed */
	bool critical;
	bool has_default;  /* its program wants to hear of a click */
	char sender[64];   /* that program's name on the bus */
	char summary[160];
	char body[640];
	int y, height;     /* where it is in the picture, in the picture's pixels */
};

/* Oldest first, which is top to bottom on screen. */
static struct note notes[max_shown];
static int note_count;
static uint32_t last_id;
static bool dirty; /* the screen does not show what `notes` says */

static bool verbose;
static int timeout_ms = default_timeout_ms;

_Noreturn static void quit(int status);

static void say(const char *format, ...) {
	if (verbose) {
		va_list args;
		va_start(args, format);
		vfprintf(stderr, format, args);
		va_end(args);
		fputc('\n', stderr);
	}
}

static int64_t now_ms(void) {
	struct timespec now;
	clock_gettime(CLOCK_MONOTONIC, &now);
	return (int64_t)now.tv_sec * 1000 + now.tv_nsec / 1000000;
}

/* --- The bus ---------------------------------------------------------------- */

static int bus_fd = -1;
static uint8_t *in; /* what has arrived and is not a whole message yet */
static size_t in_size, in_capacity;
static bool authenticated;
static uint8_t out[2048];
static uint32_t serial; /* of the last message sent */
static uint32_t name_serial, release_serial;
static bool have_name;
static bool leaving;    /* gave the name up and is waiting for the answer */
static int64_t idle_at; /* when to leave, if nothing is showing by then */

static void bus_send(struct bus_writer *writer) {
	size_t size = bus_write_end(writer), sent = 0;
	if (size == 0) {
		fprintf(stderr, "tw-notify: a message did not fit and was not sent\n");
		return;
	}
	while (sent < size) {
		ssize_t count = send(bus_fd, writer->data + sent, size - sent, MSG_NOSIGNAL);
		if (count < 0 && errno == EINTR) {
			continue;
		}
		if (count <= 0) {
			quit(0); /* the bus is gone, and the session with it */
		}
		sent += (size_t)count;
	}
}

/* A call to the bus itself. */
static struct bus_writer call_begin(const char *member, const char *signature) {
	struct bus_writer writer;
	bus_write_begin(&writer, out, sizeof(out), &(struct bus_header){
		.type = BUS_CALL, .serial = ++serial,
		.destination = "org.freedesktop.DBus", .path = "/org/freedesktop/DBus",
		.interface = "org.freedesktop.DBus", .member = member, .signature = signature});
	return writer;
}

static void name_request(void) {
	struct bus_writer writer = call_begin("RequestName", "su");
	bus_write_string(&writer, NAME);
	bus_write_u32(&writer, 4); /* do not wait in line behind another owner */
	name_serial = serial;
	bus_send(&writer);
}

static void name_release(void) {
	struct bus_writer writer = call_begin("ReleaseName", "s");
	bus_write_string(&writer, NAME);
	release_serial = serial;
	leaving = true;
	bus_send(&writer);
}

/* A signal about one notification, to the program that sent it. */
static struct bus_writer signal_begin(const struct note *note, const char *member,
		const char *signature) {
	struct bus_writer writer;
	bus_write_begin(&writer, out, sizeof(out), &(struct bus_header){
		.type = BUS_SIGNAL, .serial = ++serial,
		.destination = note->sender[0] != '\0' ? note->sender : NULL,
		.path = PATH, .interface = NAME, .member = member, .signature = signature});
	bus_write_u32(&writer, note->id);
	return writer;
}

static struct bus_writer reply_begin(const struct bus_message *call, const char *signature) {
	struct bus_writer writer;
	bus_write_begin(&writer, out, sizeof(out), &(struct bus_header){
		.type = BUS_RETURN, .serial = ++serial, .reply_serial = call->serial,
		.destination = call->sender[0] != '\0' ? call->sender : NULL,
		.signature = signature});
	return writer;
}

static void reply_send(const struct bus_message *call, struct bus_writer *writer) {
	if (call->wants_reply) {
		bus_send(writer);
	}
}

static void reply_error(const struct bus_message *call, const char *error, const char *text) {
	struct bus_writer writer;
	bus_write_begin(&writer, out, sizeof(out), &(struct bus_header){
		.type = BUS_ERROR, .serial = ++serial, .reply_serial = call->serial,
		.destination = call->sender[0] != '\0' ? call->sender : NULL,
		.error = error, .signature = "s"});
	bus_write_string(&writer, text);
	reply_send(call, &writer);
}

/* --- Notifications ---------------------------------------------------------- */

static struct note *note_find(uint32_t id) {
	for (int i = 0; i < note_count; i++) {
		if (notes[i].id == id) {
			return &notes[i];
		}
	}
	return NULL;
}

static void note_close(struct note *note, uint32_t reason) {
	say("close %u reason %u", note->id, reason);
	struct bus_writer writer = signal_begin(note, "NotificationClosed", "uu");
	bus_write_u32(&writer, reason);
	bus_send(&writer);
	memmove(note, note + 1, (size_t)(&notes[note_count] - (note + 1)) * sizeof(*note));
	note_count--;
	dirty = true;
	idle_at = now_ms() + linger_ms;
}

/* Before leaving with notifications still showing: their programs are
 * told they are gone, rather than wait for a signal that never comes. */
static void notes_close_all(void) {
	while (note_count > 0) {
		note_close(&notes[0], CLOSED_EXPIRED);
	}
}

/* The program is told of the click, with a token that lets it bring its
 * window forward if the compositor gave one, and the notification goes. */
static void note_invoke(struct note *note, const char *token) {
	say("action %u default%s", note->id, token != NULL ? " with a token" : "");
	struct bus_writer writer;
	if (token != NULL) {
		writer = signal_begin(note, "ActivationToken", "us");
		bus_write_string(&writer, token);
		bus_send(&writer);
	}
	writer = signal_begin(note, "ActionInvoked", "us");
	bus_write_string(&writer, "default");
	bus_send(&writer);
	note_close(note, CLOSED_DISMISSED);
}

/* Copies what fits, cutting between characters and not inside one. */
static void copy(char *to, size_t size, const char *from) {
	size_t length = strlen(from);
	if (length >= size) {
		length = size - 1;
		while (length > 0 && ((unsigned char)from[length] & 0xc0) == 0x80) {
			length--;
		}
	}
	memcpy(to, from, length);
	to[length] = '\0';
}

static void method_notify(const struct bus_message *call) {
	struct bus_notify read;
	if (!bus_notify_read(call, &read)) {
		reply_error(call, "org.freedesktop.DBus.Error.InvalidArgs", "cannot read this notification");
		return;
	}

	/* Any program may replace a notification by its id, as the
	 * specification has it (`notify-send -r` is a new program each time),
	 * and the one that replaced it hears of it from then on. */
	struct note *note = read.replaces != 0 ? note_find(read.replaces) : NULL;
	if (note == NULL) {
		if (note_count == max_shown) {
			/* The oldest goes to make room, but not a critical one while
			 * there is another: the battery warning is sent once and does
			 * not expire, and must not be pushed out by chatter. */
			struct note *oldest = &notes[0];
			for (int i = note_count - 1; i >= 0; i--) {
				if (!notes[i].critical) {
					oldest = &notes[i];
				}
			}
			note_close(oldest, CLOSED_EXPIRED);
		}
		note = &notes[note_count++];
		/* An id nobody holds: 0 means none, and a program may still hold
		 * one from a tw-notify that has left since. */
		do {
			last_id++;
		} while (last_id == 0 || note_find(last_id) != NULL);
		*note = (struct note){.id = last_id};
	}
	copy(note->sender, sizeof(note->sender), call->sender);
	copy(note->summary, sizeof(note->summary), read.summary);
	copy(note->body, sizeof(note->body), read.body);
	note->critical = read.urgency == 2;
	note->has_default = read.has_default;
	/* -1: the program leaves it to us. 0: it wants it to stay. */
	int stay = read.expire > 0 ? read.expire :
		read.expire == 0 || note->critical ? 0 : timeout_ms;
	note->deadline = stay > 0 ? now_ms() + stay : 0;
	dirty = true;
	say("show %u '%s' for %d ms", note->id, note->summary, stay);

	struct bus_writer writer = reply_begin(call, "u");
	bus_write_u32(&writer, note->id);
	reply_send(call, &writer);
}

static const char introspection[] =
	"<!DOCTYPE node PUBLIC \"-//freedesktop//DTD D-BUS Object Introspection 1.0//EN\"\n"
	"\"http://www.freedesktop.org/standards/dbus/1.0/introspect.dtd\">\n"
	"<node><interface name=\"" NAME "\">"
	"<method name=\"Notify\">"
	"<arg direction=\"in\" type=\"s\"/><arg direction=\"in\" type=\"u\"/>"
	"<arg direction=\"in\" type=\"s\"/><arg direction=\"in\" type=\"s\"/>"
	"<arg direction=\"in\" type=\"s\"/><arg direction=\"in\" type=\"as\"/>"
	"<arg direction=\"in\" type=\"a{sv}\"/><arg direction=\"in\" type=\"i\"/>"
	"<arg direction=\"out\" type=\"u\"/></method>"
	"<method name=\"CloseNotification\"><arg direction=\"in\" type=\"u\"/></method>"
	"<method name=\"GetCapabilities\"><arg direction=\"out\" type=\"as\"/></method>"
	"<method name=\"GetServerInformation\">"
	"<arg direction=\"out\" type=\"s\"/><arg direction=\"out\" type=\"s\"/>"
	"<arg direction=\"out\" type=\"s\"/><arg direction=\"out\" type=\"s\"/></method>"
	"<signal name=\"NotificationClosed\"><arg type=\"u\"/><arg type=\"u\"/></signal>"
	"<signal name=\"ActionInvoked\"><arg type=\"u\"/><arg type=\"s\"/></signal>"
	"<signal name=\"ActivationToken\"><arg type=\"u\"/><arg type=\"s\"/></signal>"
	"</interface></node>\n";

static void handle_call(const struct bus_message *call) {
	struct bus_writer writer;
	bool ours = strcmp(call->interface, NAME) == 0 || call->interface[0] == '\0';
	idle_at = now_ms() + linger_ms;

	if (ours && strcmp(call->member, "Notify") == 0) {
		method_notify(call);
	} else if (ours && strcmp(call->member, "CloseNotification") == 0 &&
			strcmp(call->signature, "u") == 0) {
		struct bus_reader reader = call->body;
		struct note *note = note_find(bus_read_u32(&reader));
		if (note != NULL) {
			note_close(note, CLOSED_BY_CALL);
		}
		writer = reply_begin(call, NULL);
		reply_send(call, &writer);
	} else if (ours && strcmp(call->member, "GetCapabilities") == 0) {
		/* "actions" although no buttons are drawn: a click is the action
		 * called "default", and Chromium shows notifications of its own
		 * making when a service does not list it. */
		writer = reply_begin(call, "as");
		size_t mark = bus_write_array_begin(&writer, 4);
		bus_write_string(&writer, "body");
		bus_write_string(&writer, "actions");
		bus_write_array_end(&writer, mark, 4);
		reply_send(call, &writer);
	} else if (ours && strcmp(call->member, "GetServerInformation") == 0) {
		writer = reply_begin(call, "ssss");
		bus_write_string(&writer, "tw-notify");
		bus_write_string(&writer, "tw");
		bus_write_string(&writer, "1");
		bus_write_string(&writer, "1.2"); /* of the specification */
		reply_send(call, &writer);
	} else if ((strcmp(call->interface, "org.freedesktop.DBus.Peer") == 0 ||
			call->interface[0] == '\0') && strcmp(call->member, "Ping") == 0) {
		/* The interface is optional in a call, and no other has a Ping. */
		writer = reply_begin(call, NULL);
		reply_send(call, &writer);
	} else if (strcmp(call->interface, "org.freedesktop.DBus.Properties") == 0 &&
			strcmp(call->member, "GetAll") == 0) {
		/* There are none, which some libraries want to hear. */
		writer = reply_begin(call, "a{sv}");
		bus_write_array_end(&writer, bus_write_array_begin(&writer, 8), 8);
		reply_send(call, &writer);
	} else if (strcmp(call->interface, "org.freedesktop.DBus.Introspectable") == 0 &&
			strcmp(call->member, "Introspect") == 0) {
		writer = reply_begin(call, "s");
		bus_write_string(&writer, introspection);
		reply_send(call, &writer);
	} else {
		reply_error(call, "org.freedesktop.DBus.Error.UnknownMethod", "no such method");
	}
}

static void handle_message(const struct bus_message *message) {
	bool answer = message->type == BUS_RETURN || message->type == BUS_ERROR;
	if (answer && message->reply_serial == name_serial) {
		/* 1: the name is ours. 4: it already was. */
		struct bus_reader reader = message->body;
		uint32_t result = message->type == BUS_RETURN ? bus_read_u32(&reader) : 0;
		have_name = result == 1 || result == 4;
		if (!have_name && note_count == 0) {
			fprintf(stderr, "tw-notify: another notification service is running\n");
			quit(1);
		}
		/* Without the name but with something to show: one that was on its
		 * way as this one was leaving. It is shown, and then this one goes. */
	} else if (answer && leaving && message->reply_serial == release_serial) {
		leaving = false;
		have_name = false;
		if (note_count == 0) {
			say("leave");
			quit(0);
		}
		name_request();
	} else if (message->type == BUS_CALL) {
		handle_call(message);
	}
	/* The rest: the bus's answer to Hello and its signals about our name.
	 * Signals we send say that they want no reply, so none comes back. */
}

static void bus_read(void) {
	size_t room = 4096;
	long whole = authenticated ? bus_message_size(in, in_size) : 0;
	if (whole > 0 && (size_t)whole > in_size + room) {
		room = (size_t)whole - in_size;
	}
	if (in_capacity - in_size < room) {
		uint8_t *grown = realloc(in, in_size + room);
		if (grown == NULL) {
			quit(1);
		}
		in = grown;
		in_capacity = in_size + room;
	}
	ssize_t count = recv(bus_fd, in + in_size, in_capacity - in_size, 0);
	if (count < 0 && errno == EINTR) {
		return;
	}
	if (count <= 0) {
		quit(0); /* the bus is gone, and the session with it */
	}
	in_size += (size_t)count;

	size_t pos = 0;
	if (!authenticated) {
		/* The answer to the login is one line of text. */
		uint8_t *line_end = memchr(in, '\n', in_size);
		if (line_end == NULL) {
			return;
		}
		if (in_size < 2 || memcmp(in, "OK", 2) != 0) {
			fprintf(stderr, "tw-notify: the bus did not let us in\n");
			quit(1);
		}
		authenticated = true;
		pos = (size_t)(line_end - in) + 1;
	}
	for (;;) {
		whole = bus_message_size(in + pos, in_size - pos);
		if (whole < 0) {
			/* Not even where this message ends can be told, so nothing
			 * after it can be read either. The bus does not send such a
			 * thing; if it does, the notifications go properly. */
			fprintf(stderr, "tw-notify: cannot read what the bus sent\n");
			notes_close_all();
			quit(1);
		}
		if (whole == 0 || (size_t)whole > in_size - pos) {
			break;
		}
		struct bus_message message;
		if (bus_message_parse(in + pos, (size_t)whole, &message)) {
			handle_message(&message);
		} else {
			/* Passed over. A call still gets an answer if it can be
			 * addressed, or its program waits half a minute for one. */
			say("cannot read a message of %ld bytes", whole);
			if (message.type == BUS_CALL && message.serial != 0 && message.sender[0] != '\0') {
				reply_error(&message, "org.freedesktop.DBus.Error.InvalidArgs",
					"cannot read this message");
			}
		}
		pos += (size_t)whole;
	}
	memmove(in, in + pos, in_size - pos);
	in_size -= pos;
	/* A notification can carry a picture of megabytes. Do not keep the room
	 * it took. */
	if (in_size == 0 && in_capacity > 65536) {
		free(in);
		in = NULL;
		in_capacity = 0;
	}
}

static int hex(char digit) {
	return digit >= '0' && digit <= '9' ? digit - '0' :
		digit >= 'a' && digit <= 'f' ? digit - 'a' + 10 :
		digit >= 'A' && digit <= 'F' ? digit - 'A' + 10 : -1;
}

/*
 * Where the session bus listens: DBUS_SESSION_BUS_ADDRESS, which reads
 * "unix:path=/run/user/1000/bus" with perhaps more after a comma, or the
 * usual place. Returns the address's length, 0 if there is none.
 */
static socklen_t bus_address(struct sockaddr_un *address) {
	const char *env = getenv("DBUS_SESSION_BUS_ADDRESS"), *runtime = getenv("XDG_RUNTIME_DIR");
	const char *value = NULL;
	char *path = address->sun_path;
	size_t room = sizeof(address->sun_path) - 1, length = 0;
	bool abstract = false;

	*address = (struct sockaddr_un){.sun_family = AF_UNIX};
	for (const char *pair = env; pair != NULL && value == NULL;) {
		pair += strncmp(pair, "unix:", 5) == 0 ? 5 : 0;
		if (strncmp(pair, "path=", 5) == 0) {
			value = pair + 5;
		} else if (strncmp(pair, "abstract=", 9) == 0) {
			value = pair + 9;
			abstract = true;
		}
		pair = strchr(pair, ',');
		pair = pair != NULL ? pair + 1 : NULL;
	}
	if (value != NULL) {
		length = abstract ? 1 : 0; /* an abstract name starts with a NUL */
		for (; *value != '\0' && *value != ',' && *value != ';' && length < room; value++) {
			if (*value == '%' && hex(value[1]) >= 0 && hex(value[2]) >= 0) {
				path[length++] = (char)(hex(value[1]) * 16 + hex(value[2]));
				value += 2;
			} else {
				path[length++] = *value;
			}
		}
	} else if (runtime != NULL && strlen(runtime) + 4 < room) {
		length = (size_t)sprintf(path, "%s/bus", runtime);
	} else {
		return 0;
	}
	return (socklen_t)(offsetof(struct sockaddr_un, sun_path) + length + (abstract ? 0 : 1));
}

static bool bus_connect(void) {
	struct sockaddr_un address;
	socklen_t length = bus_address(&address);
	bus_fd = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
	if (length == 0 || bus_fd < 0 || connect(bus_fd, (struct sockaddr *)&address, length) < 0) {
		return false;
	}
	/* The login: a NUL, then our user id as text, in hexadecimal. The bus
	 * checks it against who the kernel says is on the socket. What follows
	 * does not wait for the answer, which bus_read() takes when it comes. */
	char uid[16], login[96];
	int at = 0;
	snprintf(uid, sizeof(uid), "%u", (unsigned)getuid());
	login[at++] = '\0';
	at += sprintf(login + at, "AUTH EXTERNAL ");
	for (const char *digit = uid; *digit != '\0'; digit++) {
		at += sprintf(login + at, "%02x", (unsigned)*digit);
	}
	at += sprintf(login + at, "\r\nBEGIN\r\n");
	if (send(bus_fd, login, (size_t)at, MSG_NOSIGNAL) != at) {
		return false;
	}
	struct bus_writer writer = call_begin("Hello", NULL);
	bus_send(&writer);
	name_request();
	return true;
}

/* --- The font -------------------------------------------------------------- */

/*
 * The font comes from fontconfig, as in any other program: `font` in
 * config.h first, then whatever fontconfig ranks next for a character it
 * lacks. It is opened with the first picture, not at startup, so a program
 * that only asks what tw-notify can do costs no font; it goes when tw-notify
 * does, which is a second after the last notification.
 */
static FT_Library library;
static FcFontSet *fonts;   /* the font, then those to fall back on; NULL until opened */
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

static void font_close(void) {
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

static void font_open(void) {
	static bool tried;
	if (tried) {
		return;
	}
	tried = true;
	FcPattern *pattern = FcNameParse((const FcChar8 *)font);
	FcResult result;
	if (pattern != NULL && FcConfigSubstitute(NULL, pattern, FcMatchPattern)) {
		FcDefaultSubstitute(pattern);
		fonts = FcFontSort(NULL, pattern, FcTrue, NULL, &result);
	}
	if (pattern != NULL) {
		FcPatternDestroy(pattern);
	}
	if (fonts == NULL || fonts->nfont == 0 || FT_Init_FreeType(&library) != 0) {
		fprintf(stderr, "tw-notify: no font for '%s'\n", font);
		if (fonts != NULL) {
			FcFontSetDestroy(fonts);
			fonts = NULL;
		}
		FcFini();
		return;
	}
	faces = calloc((size_t)fonts->nfont, sizeof(*faces));
	unusable = calloc((size_t)fonts->nfont, sizeof(*unusable));
	if (faces == NULL || unusable == NULL) {
		quit(1);
	}
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
		say("font %s", file);
		FT_Set_Pixel_Sizes(faces[i], 0, (FT_UInt)font_px);
	}
	return faces[i];
}

/* Sets the size in pixels the text is drawn at. */
static void font_size_set(int size) {
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
	/* Without a font only the frames are drawn, and their height still
	 * goes by lines of text. */
	ascent = size;
	line_height = size * 4 / 3;
	FT_Face first = fonts != NULL ? face_get(0) : NULL;
	if (first != NULL) {
		ascent = (int)((first->size->metrics.ascender + 63) >> 6);
		line_height = (int)((first->size->metrics.height + 32) >> 6);
	}
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
	FT_Int32 flags = FT_LOAD_TARGET_LIGHT | (render ? FT_LOAD_RENDER : 0);
	if (face == NULL || FT_Load_Glyph(face, index, flags) != 0) {
		return NULL;
	}
	return face->glyph;
}

/* How far a character moves the pen, in 64ths of a pixel. */
static int advance_of(uint32_t code) {
	size_t slot = code % LENGTH(advances);
	if (advances[slot].code != code || advances[slot].advance == 0) {
		FT_GlyphSlot glyph = glyph_load(code, false);
		advances[slot].code = code;
		advances[slot].advance = glyph != NULL ? (int)glyph->advance.x : 0;
	}
	return advances[slot].advance;
}

/* --- Drawing ---------------------------------------------------------------- */

static uint32_t scale = 120; /* of the screen the list is on, in 120ths */
static int buffer_width, buffer_height; /* of the picture, in its own pixels */

/* Logical pixels to the picture's. */
static int px(int logical) {
	return (logical * (int)scale + 60) / 120;
}

/* The character at *text, which must not be at its end, and moves past it.
 * Control characters come out as spaces. */
static uint32_t character(const char **text) {
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

/*
 * How many bytes of `text` fit on one line of `width` (in 64ths of a pixel).
 * A line ends at a line break, and with `words` between two words where it
 * can; at least one character is always taken.
 */
static size_t line_fit(const char *text, int width, bool words) {
	const char *at = text, *space = NULL;
	int pen = 0;
	while (*at != '\0' && *at != '\n') {
		const char *start = at;
		uint32_t code = character(&at);
		if (code == ' ') {
			space = start;
		}
		pen += advance_of(code);
		if (pen > width && start > text) {
			return (size_t)((words && space != NULL && space > text ? space : start) - text);
		}
	}
	return (size_t)(at - text);
}

/* Draws one character with its pen at x, and returns its advance. */
static int glyph_draw(uint32_t *pixels, uint32_t code, int x, int baseline, uint32_t colour) {
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
		if (at_y < 0 || at_y >= buffer_height) {
			continue;
		}
		for (int column = 0; column < (int)bitmap->width; column++) {
			int at_x = x + glyph->bitmap_left + column;
			uint32_t cover = bitmap->buffer[row * bitmap->pitch + column];
			if (cover == 0 || at_x < 0 || at_x >= buffer_width) {
				continue;
			}
			uint32_t *pixel = &pixels[at_y * buffer_width + at_x], mixed = 0;
			for (int shift = 0; shift < 24; shift += 8) {
				uint32_t under = *pixel >> shift & 0xff, over = colour >> shift & 0xff;
				mixed |= (under * (255 - cover) + over * cover + 127) / 255 << shift;
			}
			*pixel = mixed;
		}
	}
	return (int)glyph->advance.x;
}

/* `length` bytes of text from x on, followed by "…" if `cut`. */
static void text_draw(uint32_t *pixels, int x, int baseline, const char *text, size_t length,
		bool cut, uint32_t colour) {
	int pen = x * 64;
	for (const char *at = text; at < text + length;) {
		pen += glyph_draw(pixels, character(&at), (pen + 32) >> 6, baseline, colour);
	}
	if (cut) {
		glyph_draw(pixels, 0x2026, (pen + 32) >> 6, baseline, colour);
	}
}

static void fill(uint32_t *pixels, int x, int y, int width, int height, uint32_t colour) {
	for (int row = y < 0 ? 0 : y; row < y + height && row < buffer_height; row++) {
		for (int column = x < 0 ? 0 : x; column < x + width && column < buffer_width; column++) {
			pixels[row * buffer_width + column] = colour;
		}
	}
}

/*
 * One line of a notification's text, at `top`: as much of `text` as fits, cut
 * with "…" if `last` and there is more. Draws nothing without pixels.
 * Returns what is left for the next line.
 */
static const char *line_draw(uint32_t *pixels, int top, const char *text, bool words,
		bool last, uint32_t colour) {
	int edge = px(note_border) + px(note_padding);
	int width = (buffer_width - 2 * edge) * 64;
	size_t length = line_fit(text, width, words);
	const char *rest = text + length;
	rest += strspn(rest, " \t\r\n");
	bool cut = last && *rest != '\0';
	if (cut) {
		length = line_fit(text, width - advance_of(0x2026), false);
	}
	if (pixels != NULL) {
		text_draw(pixels, edge, top + ascent, text, length, cut, colour);
	}
	return rest;
}

/* Draws a notification with its top edge at note->y, or without pixels only
 * measures it. Returns its height. */
static int note_draw(const struct note *note, uint32_t *pixels) {
	int border = px(note_border), edge = border + px(note_padding);
	int top = note->y + edge;
	if (pixels != NULL) {
		fill(pixels, 0, note->y, buffer_width, note->height,
			note->critical ? colour_critical : colour_border);
		fill(pixels, border, note->y + border, buffer_width - 2 * border,
			note->height - 2 * border, colour_background);
	}
	if (note->summary[0] != '\0') {
		line_draw(pixels, top, note->summary, false, true, colour_title);
		top += line_height;
	}
	const char *text = note->body + strspn(note->body, " \t\r\n");
	for (int line = 0; line < body_lines && *text != '\0'; line++) {
		text = line_draw(pixels, top, text, true, line == body_lines - 1, colour_text);
		top += line_height;
	}
	return top + edge - note->y;
}

/* Decides where each notification goes, and with that how large the picture
 * is. */
static void layout(void) {
	font_size_set(px(font_size));
	/* The picture's size must be a whole number of logical pixels, or the
	 * compositor would have to stretch it: at scale 1.25, a multiple of 5. */
	int step = (int)scale, divisor = 120;
	while (divisor != 0) {
		int remainder = step % divisor;
		step = divisor;
		divisor = remainder;
	}
	step = (int)scale / step;
	buffer_width = (px(note_width) + step - 1) / step * step;

	int y = 0;
	for (int i = 0; i < note_count; i++) {
		notes[i].y = y;
		notes[i].height = note_draw(&notes[i], NULL);
		/* Neighbours share a border. */
		y += notes[i].height - px(note_border);
	}
	buffer_height = y + px(note_border);
	int rounded = (buffer_height + step - 1) / step * step;
	notes[note_count - 1].height += rounded - buffer_height;
	buffer_height = rounded;
}

/* --- The window ------------------------------------------------------------- */

static struct wl_display *display;
static struct wl_registry *registry;
static struct wl_compositor *compositor;
static struct wl_shm *shm;
static struct wl_seat *seat;
static struct wl_pointer *pointer;
static struct zwlr_layer_shell_v1 *layer_shell;
static struct wp_viewporter *viewporter;
static struct wp_fractional_scale_manager_v1 *scale_manager;
static struct xdg_activation_v1 *activation;

static struct wl_surface *surface;
static struct zwlr_layer_surface_v1 *layer_surface;
static struct wp_viewport *viewport;
static struct wp_fractional_scale_v1 *surface_scale;
static struct wl_buffer *buffer;
static int asked_width, asked_height; /* the size last asked for, logical */
static bool configured;               /* the compositor has answered once */
static bool waiting;                  /* for its answer to a new size */
static wl_fixed_t pointer_y;
static struct xdg_activation_token_v1 *token; /* being made, for a click */
static uint32_t token_for;            /* the notification it is for */

static void window_close(void) {
	if (surface == NULL) {
		return;
	}
	if (surface_scale != NULL) {
		wp_fractional_scale_v1_destroy(surface_scale);
		wp_viewport_destroy(viewport);
	}
	zwlr_layer_surface_v1_destroy(layer_surface);
	wl_surface_destroy(surface);
	if (buffer != NULL) {
		wl_buffer_destroy(buffer);
	}
	surface = NULL;
	layer_surface = NULL;
	viewport = NULL;
	surface_scale = NULL;
	buffer = NULL;
	configured = waiting = false;
}

static void layer_configure(void *data, struct zwlr_layer_surface_v1 *layer, uint32_t configure,
		uint32_t width, uint32_t height) {
	zwlr_layer_surface_v1_ack_configure(layer, configure);
	/* A compositor repeats itself when other things on the screen change.
	 * Only the first answer and one to a new size call for a picture. */
	if (!configured || waiting) {
		dirty = true;
	}
	configured = true;
	waiting = false;
}

static void layer_closed(void *data, struct zwlr_layer_surface_v1 *layer) {
	/* The screen it was on is gone. Opened again, the list goes on one
	 * that is left. Only if it had been shown: with no screen at all the
	 * compositor closes it before its first configure, and it waits for
	 * one to be plugged in (registry_global) instead of trying forever. */
	bool shown = configured;
	window_close();
	dirty = dirty || shown;
}

static const struct zwlr_layer_surface_v1_listener layer_listener = {
	.configure = layer_configure,
	.closed = layer_closed,
};

static void scale_preferred(void *data, struct wp_fractional_scale_v1 *fractional,
		uint32_t value) {
	if (value != scale && value != 0) {
		scale = value;
		dirty = true;
	}
}

static const struct wp_fractional_scale_v1_listener scale_listener = {
	.preferred_scale = scale_preferred,
};

static void window_open(void) {
	surface = wl_compositor_create_surface(compositor);
	/* On the overlay layer, above a fullscreen window too: a game must not
	 * hide the battery warning. No output named: the compositor picks. */
	layer_surface = zwlr_layer_shell_v1_get_layer_surface(layer_shell, surface, NULL,
		ZWLR_LAYER_SHELL_V1_LAYER_OVERLAY, "notifications");
	zwlr_layer_surface_v1_add_listener(layer_surface, &layer_listener, NULL);
	zwlr_layer_surface_v1_set_anchor(layer_surface,
		ZWLR_LAYER_SURFACE_V1_ANCHOR_TOP | ZWLR_LAYER_SURFACE_V1_ANCHOR_RIGHT);
	zwlr_layer_surface_v1_set_margin(layer_surface, note_margin, note_margin, 0, 0);
	zwlr_layer_surface_v1_set_size(layer_surface, (uint32_t)asked_width, (uint32_t)asked_height);
	/* The two that make text sharp on a screen at a scale like 1.25: one
	 * says what the scale is, the other lets a picture in the screen's own
	 * pixels stand for a size in logical ones. */
	if (viewporter != NULL && scale_manager != NULL) {
		viewport = wp_viewporter_get_viewport(viewporter, surface);
		surface_scale = wp_fractional_scale_manager_v1_get_fractional_scale(scale_manager, surface);
		wp_fractional_scale_v1_add_listener(surface_scale, &scale_listener, NULL);
	}
	wl_surface_commit(surface);
	waiting = true;
}

/* Paints the list into a new picture and shows it. */
static void window_draw(void) {
	int stride = buffer_width * 4;
	size_t size = (size_t)stride * (size_t)buffer_height;
	int fd = memfd_create("tw-notify", MFD_CLOEXEC);
	uint32_t *pixels = MAP_FAILED;
	if (fd >= 0 && ftruncate(fd, (off_t)size) == 0) {
		pixels = mmap(NULL, size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
	}
	if (pixels == MAP_FAILED) {
		fprintf(stderr, "tw-notify: no memory for the picture\n");
		quit(1);
	}
	/* Urgent ones last: neighbours share a border, and theirs is the one
	 * that should show. */
	for (int urgent = 0; urgent <= 1; urgent++) {
		for (int i = 0; i < note_count; i++) {
			if (notes[i].critical == (urgent == 1)) {
				note_draw(&notes[i], pixels);
			}
		}
	}

	struct wl_shm_pool *pool = wl_shm_create_pool(shm, fd, (int32_t)size);
	struct wl_buffer *drawn = wl_shm_pool_create_buffer(pool, 0, buffer_width, buffer_height,
		stride, WL_SHM_FORMAT_XRGB8888);
	wl_shm_pool_destroy(pool);
	/* The compositor has the picture now. Our view of it can go, and so
	 * this program holds no pixels between two changes. */
	munmap(pixels, size);
	close(fd);

	wl_surface_attach(surface, drawn, 0, 0);
	if (viewport != NULL) {
		wp_viewport_set_destination(viewport, asked_width, asked_height);
	}
	wl_surface_damage_buffer(surface, 0, 0, buffer_width, buffer_height);
	wl_surface_commit(surface);
	if (buffer != NULL) {
		wl_buffer_destroy(buffer);
	}
	buffer = drawn;
	say("draw %dx%d at scale %u/120", buffer_width, buffer_height, scale);
}

static void token_done(void *data, struct xdg_activation_token_v1 *made, const char *string) {
	struct note *note = note_find(token_for);
	xdg_activation_token_v1_destroy(made);
	token = NULL;
	token_for = 0;
	if (note != NULL) {
		note_invoke(note, string);
	}
}

static const struct xdg_activation_token_v1_listener token_listener = {
	.done = token_done,
};

static void pointer_enter(void *data, struct wl_pointer *wl_pointer, uint32_t enter,
		struct wl_surface *entered, wl_fixed_t x, wl_fixed_t y) {
	pointer_y = y;
}

static void pointer_leave(void *data, struct wl_pointer *wl_pointer, uint32_t leave,
		struct wl_surface *left) {
}

static void pointer_motion(void *data, struct wl_pointer *wl_pointer, uint32_t time,
		wl_fixed_t x, wl_fixed_t y) {
	pointer_y = y;
}

static void pointer_button(void *data, struct wl_pointer *wl_pointer, uint32_t press,
		uint32_t time, uint32_t button, uint32_t state) {
	/* While the list is changing, what is on screen is not what `notes`
	 * says, and a click would land on the wrong notification. It is let
	 * go: the picture follows within a frame. */
	if (state != WL_POINTER_BUTTON_STATE_PRESSED || surface == NULL || waiting || dirty) {
		return;
	}
	int y = (int)(wl_fixed_to_double(pointer_y) * scale / 120);
	struct note *note = NULL;
	for (int i = 0; i < note_count; i++) {
		if (y >= notes[i].y && y < notes[i].y + notes[i].height) {
			note = &notes[i];
		}
	}
	if (note == NULL) {
		return;
	}
	if (button != BTN_LEFT || !note->has_default) {
		note_close(note, CLOSED_DISMISSED);
	} else if (activation == NULL || token_for != 0) {
		note_invoke(note, NULL);
	} else {
		/* The program will want its window brought forward, which the
		 * compositor allows to whoever holds a token made from a click.
		 * So have one made from this click and pass it on. */
		token_for = note->id;
		token = xdg_activation_v1_get_activation_token(activation);
		xdg_activation_token_v1_add_listener(token, &token_listener, NULL);
		xdg_activation_token_v1_set_serial(token, press, seat);
		xdg_activation_token_v1_set_surface(token, surface);
		xdg_activation_token_v1_commit(token);
	}
}

static void pointer_axis(void *data, struct wl_pointer *wl_pointer, uint32_t time,
		uint32_t axis, wl_fixed_t value) {
}

static const struct wl_pointer_listener pointer_listener = {
	.enter = pointer_enter,
	.leave = pointer_leave,
	.motion = pointer_motion,
	.button = pointer_button,
	.axis = pointer_axis,
};

static void seat_capabilities(void *data, struct wl_seat *wl_seat, uint32_t capabilities) {
	if ((capabilities & WL_SEAT_CAPABILITY_POINTER) != 0 && pointer == NULL) {
		pointer = wl_seat_get_pointer(wl_seat);
		wl_pointer_add_listener(pointer, &pointer_listener, NULL);
	}
}

static const struct wl_seat_listener seat_listener = {
	.capabilities = seat_capabilities,
};

static void registry_global(void *data, struct wl_registry *wl_registry, uint32_t name,
		const char *interface, uint32_t version) {
	/* Each at the oldest version that has what is used here: a newer one
	 * would only send events nobody listens to. */
	if (strcmp(interface, wl_compositor_interface.name) == 0 && version >= 4) {
		compositor = wl_registry_bind(wl_registry, name, &wl_compositor_interface, 4);
	} else if (strcmp(interface, wl_shm_interface.name) == 0) {
		shm = wl_registry_bind(wl_registry, name, &wl_shm_interface, 1);
	} else if (strcmp(interface, wl_seat_interface.name) == 0 && seat == NULL) {
		seat = wl_registry_bind(wl_registry, name, &wl_seat_interface, 1);
		wl_seat_add_listener(seat, &seat_listener, NULL);
	} else if (strcmp(interface, zwlr_layer_shell_v1_interface.name) == 0) {
		layer_shell = wl_registry_bind(wl_registry, name, &zwlr_layer_shell_v1_interface, 1);
	} else if (strcmp(interface, wp_viewporter_interface.name) == 0) {
		viewporter = wl_registry_bind(wl_registry, name, &wp_viewporter_interface, 1);
	} else if (strcmp(interface, wp_fractional_scale_manager_v1_interface.name) == 0) {
		scale_manager = wl_registry_bind(wl_registry, name,
			&wp_fractional_scale_manager_v1_interface, 1);
	} else if (strcmp(interface, xdg_activation_v1_interface.name) == 0) {
		activation = wl_registry_bind(wl_registry, name, &xdg_activation_v1_interface, 1);
	} else if (strcmp(interface, wl_output_interface.name) == 0 && note_count > 0 &&
			surface == NULL) {
		/* A screen to show the list on again, after the compositor closed
		 * it with the one it was on: a notification that does not expire
		 * would otherwise stay out of sight until the next one. */
		dirty = true;
	}
}

static void registry_global_remove(void *data, struct wl_registry *wl_registry, uint32_t name) {
}

static const struct wl_registry_listener registry_listener = {
	.global = registry_global,
	.global_remove = registry_global_remove,
};

/* Lets go of everything on the compositor, and of the connection. */
static void wayland_disconnect(void) {
	window_close();
	struct wl_proxy *proxies[] = {
		(struct wl_proxy *)token, (struct wl_proxy *)pointer, (struct wl_proxy *)seat,
		(struct wl_proxy *)compositor, (struct wl_proxy *)shm, (struct wl_proxy *)layer_shell,
		(struct wl_proxy *)viewporter, (struct wl_proxy *)scale_manager,
		(struct wl_proxy *)activation, (struct wl_proxy *)registry,
	};
	/* Only our side of each: the compositor drops its own with the
	 * connection, and a request to some of these is an error at the
	 * versions bound. */
	for (size_t i = 0; i < LENGTH(proxies); i++) {
		if (proxies[i] != NULL) {
			wl_proxy_destroy(proxies[i]);
		}
	}
	wl_display_flush(display);
	wl_display_disconnect(display);
	display = NULL;
	registry = NULL;
	compositor = NULL;
	shm = NULL;
	seat = NULL;
	pointer = NULL;
	layer_shell = NULL;
	viewporter = NULL;
	scale_manager = NULL;
	activation = NULL;
	token = NULL;
	token_for = 0;
}

static bool wayland_connect(void) {
	display = wl_display_connect(NULL);
	if (display == NULL) {
		fprintf(stderr, "tw-notify: no Wayland compositor to show notifications on\n");
		return false;
	}
	registry = wl_display_get_registry(display);
	wl_registry_add_listener(registry, &registry_listener, NULL);
	if (wl_display_roundtrip(display) < 0 || compositor == NULL || shm == NULL ||
			layer_shell == NULL) {
		fprintf(stderr, "tw-notify: this compositor has no layer shell\n");
		wayland_disconnect();
		return false;
	}
	return true;
}

/* Brings the screen in line with `notes`. */
static void window_update(void) {
	dirty = false;
	if (note_count == 0) {
		window_close();
		return;
	}
	if (display == NULL && !wayland_connect()) {
		/* Nowhere to show them. Their programs are told they are gone. */
		notes_close_all();
		return;
	}
	font_open(); /* once; without a font, only the frames are drawn */
	layout();
	int width = buffer_width * 120 / (int)scale, height = buffer_height * 120 / (int)scale;
	bool resized = width != asked_width || height != asked_height;
	asked_width = width;
	asked_height = height;
	if (surface == NULL) {
		window_open();
	} else if (resized) {
		/* The compositor answers a new size with a configure event, and
		 * the picture follows that. */
		zwlr_layer_surface_v1_set_size(layer_surface, (uint32_t)width, (uint32_t)height);
		wl_surface_commit(surface);
		waiting = true;
	} else if (configured && !waiting) {
		window_draw();
	}
}

/* --- Main ------------------------------------------------------------------- */

_Noreturn static void quit(int status) {
	free(in);
	in = NULL;
	font_close();
	if (display != NULL) {
		wayland_disconnect();
	}
	exit(status);
}

/* The compositor has gone, and with it the screen: tw has stopped or
 * crashed, and the session is ending or starting it again. */
_Noreturn static void compositor_gone(void) {
	say("the compositor is gone");
	notes_close_all();
	quit(0);
}

int main(int argc, char *argv[]) {
	for (int i = 1; i < argc; i++) {
		if (strcmp(argv[i], "-d") == 0) {
			verbose = true;
			continue;
		}
		if (strcmp(argv[i], "-t") == 0 && i + 1 < argc) {
			/* Digits and nothing else: read as 0, a typo would mean "stay
			 * until clicked". */
			char *end;
			long value = strtol(argv[++i], &end, 10);
			if (end != argv[i] && *end == '\0' && value >= 0 && value <= INT_MAX) {
				timeout_ms = (int)value;
				continue;
			}
		}
		fprintf(stderr, "usage: %s [-d] [-t <milliseconds>]\n"
			"Shows desktop notifications. The session's D-Bus starts it when a\n"
			"program sends one; it is not meant to be started by hand.\n"
			"  -d  say what it does\n"
			"  -t  how long a notification stays if its program does not say\n", argv[0]);
		return 1;
	}
	/* Ids go on from the time of day in milliseconds. One tw-notify lives
	 * at least a second, so the next starts past every id the last gave
	 * out unless it gave out more than a thousand a second: a program
	 * holding an id from an earlier one does not reach somebody else's
	 * notification with it. */
	struct timespec wall;
	clock_gettime(CLOCK_REALTIME, &wall);
	last_id = (uint32_t)((uint64_t)wall.tv_sec * 1000 + (uint64_t)wall.tv_nsec / 1000000);
	if (!bus_connect()) {
		fprintf(stderr, "tw-notify: cannot reach the session's D-Bus\n");
		return 1;
	}
	idle_at = now_ms() + linger_ms;

	for (;;) {
		if (dirty) {
			window_update();
		}
		if (display != NULL && wl_display_flush(display) < 0 && errno != EAGAIN) {
			compositor_gone();
		}

		/* Sleep until a message, an event, or the next thing due: a
		 * notification's time running out, or our own when none is left. */
		int64_t now = now_ms(), wake = -1;
		if (note_count == 0 && !leaving) {
			if (now >= idle_at && !have_name) {
				quit(0);
			} else if (now >= idle_at) {
				name_release();
			} else {
				wake = idle_at;
			}
		}
		for (int i = 0; i < note_count; i++) {
			if (notes[i].deadline != 0 && (wake < 0 || notes[i].deadline < wake)) {
				wake = notes[i].deadline;
			}
		}
		struct pollfd fds[2] = {
			{bus_fd, POLLIN, 0},
			{display != NULL ? wl_display_get_fd(display) : -1, POLLIN, 0},
		};
		int timeout = wake < 0 ? -1 : wake > now ? (int)(wake - now) : 0;
		if (poll(fds, 2, timeout) < 0 && errno != EINTR) {
			quit(1);
		}
		if (fds[0].revents != 0) {
			bus_read();
		}
		if (fds[1].revents != 0 && display != NULL && wl_display_dispatch(display) < 0) {
			compositor_gone();
		}

		now = now_ms();
		for (int i = 0; i < note_count;) {
			if (notes[i].deadline != 0 && notes[i].deadline <= now) {
				note_close(&notes[i], CLOSED_EXPIRED);
			} else {
				i++;
			}
		}
	}
}
