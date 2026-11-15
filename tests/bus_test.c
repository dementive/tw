/*
 * Unit tests for the D-Bus message code of tw-notify (notify/bus.c). The
 * messages come off a socket, so most of this is about ones that are cut
 * short or wrong: none may be read past its end. The copies handed to the
 * parser are exactly as long as the message, so that in the sanitizer build
 * one byte too far is a failure.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "bus.h"

static int failures;

/* Not assert(): these must still run in release builds, where NDEBUG is set. */
#define CHECK(cond) \
	do { \
		if (!(cond)) { \
			fprintf(stderr, "%s:%d: check failed: %s\n", __FILE__, __LINE__, #cond); \
			failures++; \
		} \
	} while (0)

/* What tw-notify reads out of a Notify call. */
struct notify {
	char summary[64], body[64];
	uint32_t replaces;
	int actions;
	uint8_t urgency;
	int32_t expire;
	bool ok;
};

/* The same walk over a Notify call's body as in notify/notify.c. */
static struct notify notify_read(const struct bus_message *message) {
	struct notify notify = {.urgency = 1};
	struct bus_reader reader = message->body;
	bus_read_string(&reader);
	notify.replaces = bus_read_u32(&reader);
	bus_read_string(&reader);
	snprintf(notify.summary, sizeof(notify.summary), "%s", bus_read_string(&reader));
	snprintf(notify.body, sizeof(notify.body), "%s", bus_read_string(&reader));
	size_t end = bus_read_array(&reader, 4);
	while (reader.pos < end && !reader.failed) {
		bus_read_string(&reader);
		notify.actions++;
	}
	end = bus_read_array(&reader, 8);
	while (reader.pos < end && !reader.failed) {
		bus_read_align(&reader, 8);
		const char *key = bus_read_string(&reader);
		const char *type = bus_read_signature(&reader);
		if (strcmp(key, "urgency") == 0 && strcmp(type, "y") == 0) {
			notify.urgency = bus_read_byte(&reader);
		} else {
			bus_skip(&reader, type);
		}
	}
	notify.expire = (int32_t)bus_read_u32(&reader);
	notify.ok = !reader.failed;
	return notify;
}

static void variant(struct bus_writer *writer, const char *key, const char *type) {
	uint8_t length = (uint8_t)strlen(type);
	bus_write_align(writer, 8);
	bus_write_string(writer, key);
	bus_write_bytes(writer, &length, 1);
	bus_write_bytes(writer, type, (size_t)length + 1);
}

/* A Notify call with hints of every shape a program is likely to send. */
static size_t notify_write(uint8_t *data, size_t capacity) {
	struct bus_writer writer;
	bus_write_begin(&writer, data, capacity, &(struct bus_header){
		.type = BUS_CALL, .serial = 7,
		.destination = "org.freedesktop.Notifications",
		.path = "/org/freedesktop/Notifications",
		.interface = "org.freedesktop.Notifications",
		.member = "Notify", .signature = "susssasa{sv}i"});
	bus_write_string(&writer, "a program");
	bus_write_u32(&writer, 41);
	bus_write_string(&writer, "");
	bus_write_string(&writer, "Battery low");
	bus_write_string(&writer, "Plug in the charger.");
	size_t actions = bus_write_array_begin(&writer, 4);
	bus_write_string(&writer, "default");
	bus_write_string(&writer, "Open");
	bus_write_array_end(&writer, actions, 4);

	size_t hints = bus_write_array_begin(&writer, 8);
	/* A string, with an odd length to put what follows off its alignment. */
	variant(&writer, "desktop-entry", "s");
	bus_write_string(&writer, "claude");
	/* A picture: (width, height, rowstride, alpha, bits, channels, pixels). */
	variant(&writer, "image-data", "(iiibiiay)");
	bus_write_align(&writer, 8);
	for (int i = 0; i < 6; i++) {
		bus_write_u32(&writer, 2);
	}
	size_t pixels = bus_write_array_begin(&writer, 1);
	bus_write_bytes(&writer, "\1\2\3\4\5\6\7", 7);
	bus_write_array_end(&writer, pixels, 1);
	/* A 64-bit number, a boolean, a variant inside a variant. */
	variant(&writer, "when", "x");
	bus_write_align(&writer, 8);
	bus_write_bytes(&writer, "\0\0\0\0\0\0\0", 8);
	variant(&writer, "transient", "b");
	bus_write_u32(&writer, 1);
	variant(&writer, "nested", "v");
	bus_write_bytes(&writer, "\2as", 4);
	size_t strings = bus_write_array_begin(&writer, 4);
	bus_write_string(&writer, "one");
	bus_write_string(&writer, "two");
	bus_write_array_end(&writer, strings, 4);
	/* An array of dictionaries, empty, and one of structures. */
	variant(&writer, "empty", "aa{sv}");
	bus_write_array_end(&writer, bus_write_array_begin(&writer, 4), 4);
	variant(&writer, "pairs", "a(yu)");
	size_t pairs = bus_write_array_begin(&writer, 8);
	bus_write_bytes(&writer, "\1", 1);
	bus_write_u32(&writer, 5);
	bus_write_array_end(&writer, pairs, 8);
	/* The one that is read, after all of those. */
	variant(&writer, "urgency", "y");
	bus_write_bytes(&writer, "\2", 1);
	bus_write_array_end(&writer, hints, 8);
	bus_write_u32(&writer, (uint32_t)-1);
	return bus_write_end(&writer);
}

/* Parses a copy that ends exactly where the message does. */
static bool parse_copy(const uint8_t *data, size_t size, struct notify *notify) {
	uint8_t *copy = malloc(size > 0 ? size : 1);
	memcpy(copy, data, size);
	struct bus_message message;
	bool ok = bus_message_parse(copy, size, &message);
	if (ok) {
		struct notify read = notify_read(&message);
		if (notify != NULL) {
			*notify = read;
		}
		/* And as some other call would be read. */
		struct bus_reader reader = message.body;
		bus_skip(&reader, message.signature);
	}
	free(copy);
	return ok;
}

int main(void) {
	uint8_t data[1024];
	size_t size = notify_write(data, sizeof(data));
	CHECK(size > 0);
	CHECK(bus_message_size(data, size) == (long)size);

	/* What was written is what is read. */
	struct bus_message message;
	CHECK(bus_message_parse(data, size, &message));
	CHECK(message.type == BUS_CALL && message.wants_reply && message.serial == 7);
	CHECK(strcmp(message.path, "/org/freedesktop/Notifications") == 0);
	CHECK(strcmp(message.interface, "org.freedesktop.Notifications") == 0);
	CHECK(strcmp(message.member, "Notify") == 0);
	CHECK(strcmp(message.signature, "susssasa{sv}i") == 0);
	CHECK(strcmp(message.sender, "") == 0 && message.reply_serial == 0);
	struct notify notify = notify_read(&message);
	CHECK(notify.ok);
	CHECK(strcmp(notify.summary, "Battery low") == 0);
	CHECK(strcmp(notify.body, "Plug in the charger.") == 0);
	CHECK(notify.replaces == 41 && notify.actions == 2);
	CHECK(notify.urgency == 2 && notify.expire == -1);
	/* The whole body is passed over by its signature alone, to the byte. */
	struct bus_reader reader = message.body;
	bus_skip(&reader, message.signature);
	CHECK(!reader.failed && reader.pos == reader.size);

	/* The start of a message, byte for byte as the specification has it. */
	static const uint8_t expected[] = {
		'l', BUS_CALL, 0, 1,    /* little-endian, a call, no flags, version 1 */
		0, 0, 0, 0,             /* the body's length, not compared */
		7, 0, 0, 0,             /* serial */
		0, 0, 0, 0,             /* the fields' length, not compared */
		1, 1, 'o', 0,           /* field 1 (path), a variant of type "o" */
		30, 0, 0, 0,            /* the path's length */
		'/', 'o', 'r', 'g',
	};
	CHECK(memcmp(data, expected, 4) == 0);
	CHECK(memcmp(data + 8, expected + 8, 4) == 0);
	CHECK(memcmp(data + 16, expected + 16, sizeof(expected) - 16) == 0);
	/* Each field starts on a multiple of 8, and so does the body. */
	CHECK((size_t)(message.body.data - data) % 8 == 0);
	CHECK(message.body.data + message.body.size == data + size);

	/* A reply and a signal: no reply wanted, and the fields they carry. */
	uint8_t reply[256];
	struct bus_writer writer;
	bus_write_begin(&writer, reply, sizeof(reply), &(struct bus_header){
		.type = BUS_RETURN, .serial = 9, .reply_serial = 7,
		.destination = ":1.42", .signature = "u"});
	bus_write_u32(&writer, 1234);
	size_t reply_size = bus_write_end(&writer);
	CHECK(reply_size > 0 && bus_message_parse(reply, reply_size, &message));
	CHECK(message.type == BUS_RETURN && !message.wants_reply);
	CHECK(message.serial == 9 && message.reply_serial == 7);
	CHECK(strcmp(message.signature, "u") == 0 && message.body.size == 4);
	CHECK(bus_read_u32(&message.body) == 1234 && !message.body.failed);
	CHECK(bus_read_u32(&message.body) == 0 && message.body.failed);

	/* An empty dictionary is its length and the padding up to its first
	 * element, which it does not have. */
	bus_write_begin(&writer, reply, sizeof(reply), &(struct bus_header){
		.type = BUS_RETURN, .serial = 10, .reply_serial = 8, .signature = "a{sv}"});
	bus_write_array_end(&writer, bus_write_array_begin(&writer, 8), 8);
	reply_size = bus_write_end(&writer);
	CHECK(bus_message_parse(reply, reply_size, &message) && message.body.size == 8);
	reader = message.body;
	CHECK(bus_read_array(&reader, 8) == 8 && !reader.failed);

	/* A message that does not fit its buffer is not sent in part. */
	for (size_t capacity = 0; capacity < size; capacity++) {
		uint8_t small[1024];
		CHECK(notify_write(small, capacity) == 0);
	}

	/* Cut short anywhere, a message is not taken for a whole one, and
	 * parsing what is there reads nothing past it. */
	for (size_t length = 0; length < size; length++) {
		long told = bus_message_size(data, length);
		CHECK(told == 0 || told == (long)size);
		CHECK(!parse_copy(data, length, NULL));
	}

	/* Not ours to read: big-endian, another version, absurd lengths. */
	uint8_t wrong[1024];
	memcpy(wrong, data, size);
	wrong[0] = 'B';
	CHECK(bus_message_size(wrong, size) == -1 && !parse_copy(wrong, size, NULL));
	memcpy(wrong, data, size);
	wrong[3] = 2;
	CHECK(bus_message_size(wrong, size) == -1);
	memcpy(wrong, data, size);
	memset(wrong + 4, 0xff, 4);
	CHECK(bus_message_size(wrong, size) == -1);
	memcpy(wrong, data, size);
	memset(wrong + 12, 0xff, 4);
	CHECK(bus_message_size(wrong, size) == -1);

	/* Damaged anywhere: whatever comes of it, nothing is read out of
	 * bounds and nothing loops. Every byte gets every bit flipped, and then
	 * random damage in several places at once. */
	for (size_t at = 0; at < size; at++) {
		for (int bit = 0; bit < 8; bit++) {
			memcpy(wrong, data, size);
			wrong[at] ^= (uint8_t)(1 << bit);
			parse_copy(wrong, size, NULL);
		}
	}
	uint32_t random = 12345;
	for (int round = 0; round < 20000; round++) {
		memcpy(wrong, data, size);
		for (int i = 0; i < 1 + round % 4; i++) {
			random = random * 1664525u + 1013904223u;
			wrong[(random >> 8) % size] = (uint8_t)(random >> 24);
		}
		parse_copy(wrong, size, NULL);
	}

	/* Variants nested 200 deep, with a value at the bottom: well formed, and
	 * refused, because each level is a call and a message may be 128 MB of
	 * them. The specification allows 64. */
	uint8_t deep[1024];
	bus_write_begin(&writer, deep, sizeof(deep), &(struct bus_header){
		.type = BUS_CALL, .serial = 1, .path = "/", .member = "X", .signature = "v"});
	for (int i = 0; i < 200; i++) {
		bus_write_bytes(&writer, "\1v", 3);
	}
	bus_write_bytes(&writer, "\1y\0\7", 4);
	size_t deep_size = bus_write_end(&writer);
	CHECK(deep_size > 0 && bus_message_parse(deep, deep_size, &message));
	reader = message.body;
	bus_skip(&reader, "v");
	CHECK(reader.failed);
	/* Ten deep is fine. */
	bus_write_begin(&writer, deep, sizeof(deep), &(struct bus_header){
		.type = BUS_CALL, .serial = 1, .path = "/", .member = "X", .signature = "v"});
	for (int i = 0; i < 10; i++) {
		bus_write_bytes(&writer, "\1v", 3);
	}
	bus_write_bytes(&writer, "\1y\0\7", 4);
	deep_size = bus_write_end(&writer);
	CHECK(bus_message_parse(deep, deep_size, &message));
	reader = message.body;
	bus_skip(&reader, "v");
	CHECK(!reader.failed && reader.pos == reader.size);

	/* An array that claims to be longer than what is left of the message. */
	bus_write_begin(&writer, reply, sizeof(reply), &(struct bus_header){
		.type = BUS_RETURN, .serial = 11, .reply_serial = 8, .signature = "as"});
	bus_write_u32(&writer, 1000);
	bus_write_string(&writer, "short");
	reply_size = bus_write_end(&writer);
	CHECK(bus_message_parse(reply, reply_size, &message));
	reader = message.body;
	bus_read_array(&reader, 4);
	CHECK(reader.failed);

	if (failures == 0) {
		printf("ok: write and read back, layout, replies, truncation, damage, nesting\n");
	}
	return failures != 0;
}
