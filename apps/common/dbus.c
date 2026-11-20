#define _GNU_SOURCE
#include <errno.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

#include "dbus.h"

#define PENDING_MAX 32

static int fd = -1;
static bool authenticated;
static void (*incoming_handler)(const struct bus_message *message);
static uint32_t serial; /* of the last message sent */
static uint8_t out[8192];
static struct bus_writer writer;
static uint8_t *in;
static size_t in_size, in_capacity;

/* Calls waiting for their replies. */
static struct {
	uint32_t serial;
	dbus_reply reply;
	void *data;
} pending[PENDING_MAX];

static int hex(char digit) {
	return digit >= '0' && digit <= '9' ? digit - '0' :
		digit >= 'a' && digit <= 'f' ? digit - 'a' + 10 :
		digit >= 'A' && digit <= 'F' ? digit - 'A' + 10 : -1;
}

/* Where the system bus listens: "unix:path=..." in DBUS_SYSTEM_BUS_ADDRESS,
 * else the usual socket. Returns the address's length, 0 if there is none. */
static socklen_t bus_address(struct sockaddr_un *address) {
	const char *env = getenv("DBUS_SYSTEM_BUS_ADDRESS");
	const char *value = "/run/dbus/system_bus_socket";
	char *path = address->sun_path;
	size_t room = sizeof(address->sun_path) - 1, length = 0;
	*address = (struct sockaddr_un){.sun_family = AF_UNIX};
	if (env != NULL) {
		const char *found = strstr(env, "path=");
		if (found == NULL) {
			return 0;
		}
		value = found + 5;
	}
	for (; *value != '\0' && *value != ',' && *value != ';' && length < room; value++) {
		if (*value == '%' && hex(value[1]) >= 0 && hex(value[2]) >= 0) {
			path[length++] = (char)(hex(value[1]) * 16 + hex(value[2]));
			value += 2;
		} else {
			path[length++] = *value;
		}
	}
	return (socklen_t)(offsetof(struct sockaddr_un, sun_path) + length + 1);
}

static void send_out(void) {
	size_t size = bus_write_end(&writer), sent = 0;
	if (size == 0) {
		fprintf(stderr, "%s: a D-Bus message did not fit\n", program_invocation_short_name);
		return;
	}
	while (fd >= 0 && sent < size) {
		ssize_t count = send(fd, out + sent, size - sent, MSG_NOSIGNAL);
		if (count < 0 && errno == EINTR) {
			continue;
		}
		if (count <= 0) {
			dbus_close();
			return;
		}
		sent += (size_t)count;
	}
}

struct bus_writer *dbus_call(const char *destination, const char *path,
		const char *interface, const char *member, const char *signature) {
	bus_write_begin(&writer, out, sizeof(out), &(struct bus_header){
		.type = BUS_CALL, .serial = ++serial, .destination = destination, .path = path,
		.interface = interface, .member = member, .signature = signature});
	return &writer;
}

void dbus_send(dbus_reply reply, void *data) {
	if (reply != NULL) {
		for (size_t i = 0; i < PENDING_MAX; i++) {
			if (pending[i].reply == NULL) {
				pending[i].serial = serial;
				pending[i].reply = reply;
				pending[i].data = data;
				break;
			}
		}
	}
	send_out();
}

struct bus_writer *dbus_answer(const struct bus_message *call, const char *signature) {
	bus_write_begin(&writer, out, sizeof(out), &(struct bus_header){
		.type = BUS_RETURN, .serial = ++serial, .reply_serial = call->serial,
		.destination = call->sender, .signature = signature});
	return &writer;
}

void dbus_answer_error(const struct bus_message *call, const char *error) {
	bus_write_begin(&writer, out, sizeof(out), &(struct bus_header){
		.type = BUS_ERROR, .serial = ++serial, .reply_serial = call->serial,
		.destination = call->sender, .error = error});
	send_out();
}

void dbus_match(const char *rule) {
	bus_write_string(dbus_call("org.freedesktop.DBus", "/org/freedesktop/DBus",
		"org.freedesktop.DBus", "AddMatch", "s"), rule);
	dbus_send(NULL, NULL);
}

bool dbus_open(void (*incoming)(const struct bus_message *message)) {
	incoming_handler = incoming;
	struct sockaddr_un address;
	socklen_t length = bus_address(&address);
	fd = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
	if (length == 0 || fd < 0 || connect(fd, (struct sockaddr *)&address, length) < 0) {
		dbus_close();
		return false;
	}
	/* The login: a NUL, then our user id as text, in hexadecimal. What
	 * follows does not wait for the answer, which dbus_read() takes. */
	char uid[16], login[96];
	int at = 0;
	snprintf(uid, sizeof(uid), "%u", (unsigned)getuid());
	login[at++] = '\0';
	at += sprintf(login + at, "AUTH EXTERNAL ");
	for (const char *digit = uid; *digit != '\0'; digit++) {
		at += sprintf(login + at, "%02x", (unsigned)*digit);
	}
	at += sprintf(login + at, "\r\nBEGIN\r\n");
	if (send(fd, login, (size_t)at, MSG_NOSIGNAL) != at) {
		dbus_close();
		return false;
	}
	dbus_call("org.freedesktop.DBus", "/org/freedesktop/DBus", "org.freedesktop.DBus",
		"Hello", NULL);
	dbus_send(NULL, NULL);
	return fd >= 0;
}

int dbus_fd(void) {
	return fd;
}

static void handle(const struct bus_message *message) {
	if (message->type == BUS_RETURN || message->type == BUS_ERROR) {
		for (size_t i = 0; i < PENDING_MAX; i++) {
			if (pending[i].reply != NULL && pending[i].serial == message->reply_serial) {
				dbus_reply reply = pending[i].reply;
				pending[i].reply = NULL;
				reply(message, pending[i].data);
				return;
			}
		}
	} else if (incoming_handler != NULL) {
		incoming_handler(message);
	}
}

bool dbus_read(void) {
	if (fd < 0) {
		return false;
	}
	size_t room = 65536;
	long whole = authenticated ? bus_message_size(in, in_size) : 0;
	if (whole > 0 && (size_t)whole > in_size + room) {
		room = (size_t)whole - in_size;
	}
	if (in_capacity - in_size < room) {
		uint8_t *grown = realloc(in, in_size + room);
		if (grown == NULL) {
			dbus_close();
			return false;
		}
		in = grown;
		in_capacity = in_size + room;
	}
	ssize_t count = recv(fd, in + in_size, in_capacity - in_size, MSG_DONTWAIT);
	if (count < 0 && (errno == EINTR || errno == EAGAIN)) {
		return true;
	}
	if (count <= 0) {
		dbus_close();
		return false;
	}
	in_size += (size_t)count;
	size_t pos = 0;
	if (!authenticated) {
		/* The answer to the login is one line of text. */
		uint8_t *line_end = memchr(in, '\n', in_size);
		if (line_end == NULL) {
			return true;
		}
		if (in_size < 2 || memcmp(in, "OK", 2) != 0) {
			dbus_close();
			return false;
		}
		authenticated = true;
		pos = (size_t)(line_end - in) + 1;
	}
	while (fd >= 0) {
		whole = bus_message_size(in + pos, in_size - pos);
		if (whole < 0) {
			dbus_close();
			return false;
		}
		if (whole == 0 || (size_t)whole > in_size - pos) {
			break;
		}
		struct bus_message message;
		if (bus_message_parse(in + pos, (size_t)whole, &message)) {
			handle(&message);
		}
		pos += (size_t)whole;
	}
	if (fd < 0) {
		return false;
	}
	memmove(in, in + pos, in_size - pos);
	in_size -= pos;
	return true;
}

void dbus_close(void) {
	if (fd >= 0) {
		close(fd);
	}
	fd = -1;
	authenticated = false;
	free(in);
	in = NULL;
	in_size = in_capacity = 0;
	/* Whoever waits for a reply hears that none will come. */
	for (size_t i = 0; i < PENDING_MAX; i++) {
		dbus_reply reply = pending[i].reply;
		pending[i].reply = NULL;
		if (reply != NULL) {
			reply(NULL, pending[i].data);
		}
	}
}

void dbus_properties(struct bus_reader *reader,
		void (*each)(const char *name, const char *type, struct bus_reader *value, void *data),
		void *data) {
	size_t end = bus_read_array(reader, 8);
	while (!reader->failed && reader->pos < end) {
		bus_read_align(reader, 8);
		const char *name = bus_read_string(reader);
		const char *type = bus_read_signature(reader);
		struct bus_reader value = *reader;
		bus_skip(reader, type);
		if (!reader->failed) {
			each(name, type, &value, data);
		}
	}
}
