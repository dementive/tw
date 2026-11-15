/*
 * D-Bus messages as they travel on the socket: taking one apart and putting
 * one together. Just what a notification service needs, with no library: a
 * program that is started for one notification and gone after it should not
 * have to load one.
 *
 * Pure functions on bytes, unit tested (tests/bus_test.c). Nothing here
 * trusts what it reads: every length is checked against the bytes that are
 * really there.
 */
#ifndef TW_NOTIFY_BUS_H
#define TW_NOTIFY_BUS_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

enum { BUS_CALL = 1, BUS_RETURN = 2, BUS_ERROR = 3, BUS_SIGNAL = 4 };

/* A position in a message's bytes. Once `failed` is set every read gives 0
 * or "", so a caller reads all it wants and checks once at the end. */
struct bus_reader {
	const uint8_t *data;
	size_t size, pos;
	bool failed;
};

/* A message taken apart. The strings point into the message's bytes and are
 * "" where the message has none. */
struct bus_message {
	uint8_t type;
	bool wants_reply;
	uint32_t serial, reply_serial;
	const char *path, *interface, *member, *sender, *signature;
	struct bus_reader body;
};

/*
 * How long the message at the start of `data` is, 0 if `size` bytes are too
 * few to tell, or -1 if this is not a message this code reads (big-endian,
 * or larger than the protocol allows).
 */
long bus_message_size(const uint8_t *data, size_t size);
/* Takes apart the message that fills `data`. False if it is malformed. */
bool bus_message_parse(const uint8_t *data, size_t size, struct bus_message *message);

uint8_t bus_read_byte(struct bus_reader *reader);
uint32_t bus_read_u32(struct bus_reader *reader);
const char *bus_read_string(struct bus_reader *reader);
const char *bus_read_signature(struct bus_reader *reader);
void bus_read_align(struct bus_reader *reader, size_t alignment);
/* Enters an array whose elements have this alignment, and returns the
 * position at which it ends. */
size_t bus_read_array(struct bus_reader *reader, size_t alignment);
/* Passes over the values this signature describes. */
void bus_skip(struct bus_reader *reader, const char *signature);

/* What a message says about itself. Leave out what does not apply. */
struct bus_header {
	uint8_t type;
	uint32_t serial, reply_serial;
	const char *destination, *path, *interface, *member, *error, *signature;
};

/* A message being put together in a buffer of the caller's. If it does not
 * fit, `failed` is set and bus_write_end() returns 0. */
struct bus_writer {
	uint8_t *data;
	size_t capacity, size, body;
	bool failed;
};

/* Writes the header; the body follows through the functions below. */
void bus_write_begin(struct bus_writer *writer, uint8_t *data, size_t capacity,
	const struct bus_header *header);
void bus_write_u32(struct bus_writer *writer, uint32_t value);
void bus_write_string(struct bus_writer *writer, const char *string);
void bus_write_align(struct bus_writer *writer, size_t alignment);
void bus_write_bytes(struct bus_writer *writer, const void *bytes, size_t size);
/* An array: begin returns a mark to hand to end, which fills in the length.
 * `alignment` is that of the elements, the same in both calls. */
size_t bus_write_array_begin(struct bus_writer *writer, size_t alignment);
void bus_write_array_end(struct bus_writer *writer, size_t mark, size_t alignment);
/* Fills in the lengths. Returns the size of the message, or 0 if it failed. */
size_t bus_write_end(struct bus_writer *writer);

#endif
