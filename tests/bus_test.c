/*
 * Unit tests for the D-Bus message code of tw-notify (notify/bus.c). The
 * messages come off a socket, so most of this is about ones that are cut
 * short or wrong: none may be read past its end. The copies handed to the
 * parser are exactly as long as the message, so that in the sanitizer build
 * one byte too far is a failure.
 */
#define _GNU_SOURCE /* memmem */
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
		.member = "Notify", .signature = "susssasa{sv}i", .sender = ":1.42"});
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
	/* The tag of a level, which replaces the last one with that tag. */
	variant(&writer, "x-canonical-private-synchronous", "s");
	bus_write_string(&writer, "volume");
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

/* A plain Notify call, whose actions or hints array is then made to claim
 * one byte less than it holds (`cut` 1 or 2), or whose body has a byte too
 * many after the last argument (`cut` 3). */
static size_t notify_cut(uint8_t *data, size_t capacity, int cut) {
	struct bus_writer writer;
	bus_write_begin(&writer, data, capacity, &(struct bus_header){
		.type = BUS_CALL, .serial = 8, .path = "/org/freedesktop/Notifications",
		.member = "Notify", .signature = "susssasa{sv}i"});
	bus_write_string(&writer, "a program");
	bus_write_u32(&writer, 0);
	bus_write_string(&writer, "");
	bus_write_string(&writer, "Title");
	bus_write_string(&writer, "");
	size_t actions = bus_write_array_begin(&writer, 4);
	bus_write_string(&writer, "default");
	bus_write_string(&writer, "Open");
	bus_write_array_end(&writer, actions, 4);
	size_t hints = bus_write_array_begin(&writer, 8);
	variant(&writer, "urgency", "y");
	bus_write_bytes(&writer, "\2", 1);
	bus_write_array_end(&writer, hints, 8);
	bus_write_u32(&writer, 0);
	if (cut == 3) {
		bus_write_bytes(&writer, "\0", 1);
	}
	size_t mark = cut == 1 ? actions : cut == 2 ? hints : 0;
	if (mark != 0) {
		data[mark]--; /* little-endian, and none of these lengths is 0 */
	}
	return bus_write_end(&writer);
}

/* Parses a copy that ends exactly where the message does, then reads its
 * body as tw-notify would. */
static bool parse_copy(const uint8_t *data, size_t size, struct bus_notify *notify) {
	uint8_t *copy = malloc(size > 0 ? size : 1);
	memcpy(copy, data, size);
	struct bus_message message;
	bool ok = bus_message_parse(copy, size, &message);
	/* What could not be read is there and empty, never NULL. */
	CHECK(message.path != NULL && message.interface != NULL && message.member != NULL &&
		message.sender != NULL && message.signature != NULL);
	if (ok) {
		struct bus_notify read;
		if (bus_notify_read(&message, &read) && notify != NULL) {
			*notify = read;
		}
		CHECK(read.summary != NULL && read.body != NULL);
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
	CHECK(strcmp(message.sender, ":1.42") == 0 && message.reply_serial == 0);
	struct bus_notify notify;
	CHECK(bus_notify_read(&message, &notify));
	CHECK(strcmp(notify.summary, "Battery low") == 0);
	CHECK(strcmp(notify.body, "Plug in the charger.") == 0);
	CHECK(notify.replaces == 41 && notify.has_default);
	CHECK(notify.urgency == 2 && notify.expire == -1);
	CHECK(strcmp(notify.tag, "volume") == 0);
	/* Under another signature it is not a Notify call. */
	struct bus_message other = message;
	other.signature = "susssasa{sv}";
	CHECK(!bus_notify_read(&other, &notify));
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

	/* Big-endian: its length is known, so that it can be passed over, but
	 * it is not taken apart. */
	uint8_t wrong[1024];
	memcpy(wrong, data, size);
	wrong[0] = 'B';
	for (int at = 4; at < 16; at += 4) {
		for (int i = 0; i < 4; i++) {
			wrong[at + i] = data[at + 3 - i];
		}
	}
	CHECK(bus_message_size(wrong, size) == (long)size && !parse_copy(wrong, size, NULL));
	CHECK(bus_message_parse(wrong, size, &message) == false && message.type == 0);
	/* Not even the length is known: another byte order, another version,
	 * absurd lengths. */
	memcpy(wrong, data, size);
	wrong[0] = 'x';
	CHECK(bus_message_size(wrong, size) == -1);
	memcpy(wrong, data, size);
	wrong[3] = 2;
	CHECK(bus_message_size(wrong, size) == -1);
	memcpy(wrong, data, size);
	memset(wrong + 4, 0xff, 4);
	CHECK(bus_message_size(wrong, size) == -1);
	memcpy(wrong, data, size);
	memset(wrong + 12, 0xff, 4);
	CHECK(bus_message_size(wrong, size) == -1);

	/* A header that breaks after the serial and the sender: what was read
	 * is kept, so that tw-notify can still answer the call. Here the
	 * signature field's type is made one that does not exist. */
	memcpy(wrong, data, size);
	uint8_t *field = memmem(wrong, size, "\10\1g", 4);
	CHECK(field != NULL);
	if (field != NULL) {
		field[2] = '!';
		CHECK(!bus_message_parse(wrong, size, &message));
		CHECK(message.type == BUS_CALL && message.wants_reply && message.serial == 7);
		CHECK(strcmp(message.sender, ":1.42") == 0 && strcmp(message.member, "Notify") == 0);
	}

	/* A Notify call is read to its last byte: an array that ends inside
	 * its last element, or a byte after the last argument, is refused. */
	uint8_t cut[256];
	size_t cut_size = notify_cut(cut, sizeof(cut), 0);
	CHECK(bus_message_parse(cut, cut_size, &message) && bus_notify_read(&message, &notify));
	CHECK(strcmp(notify.summary, "Title") == 0 && notify.has_default && notify.urgency == 2);
	CHECK(notify.expire == 0 && notify.replaces == 0 && notify.tag[0] == '\0');
	for (int damage = 1; damage <= 3; damage++) {
		cut_size = notify_cut(cut, sizeof(cut), damage);
		CHECK(bus_message_parse(cut, cut_size, &message));
		CHECK(!bus_notify_read(&message, &notify));
	}

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
		printf("ok: write and read back, layout, replies, truncation, byte order, damage, "
			"strict Notify bodies, nesting\n");
	}
	return failures != 0;
}
