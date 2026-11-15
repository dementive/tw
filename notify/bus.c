#include <string.h>

#include "bus.h"

/* The protocol's limits: 64 MB of header fields, 128 MB for a message. */
#define FIELDS_MAX (1u << 26)
#define MESSAGE_MAX (1u << 27)
/* How deep containers and variants may nest; the protocol allows 64. */
#define DEPTH_MAX 64

static uint32_t le32(const uint8_t *bytes) {
	return (uint32_t)bytes[0] | (uint32_t)bytes[1] << 8 |
		(uint32_t)bytes[2] << 16 | (uint32_t)bytes[3] << 24;
}

static size_t round_up(size_t value, size_t alignment) {
	return (value + alignment - 1) & ~(alignment - 1);
}

/* --- Reading ---------------------------------------------------------------- */

void bus_read_align(struct bus_reader *reader, size_t alignment) {
	size_t pos = round_up(reader->pos, alignment);
	if (pos > reader->size) {
		reader->failed = true;
		return;
	}
	reader->pos = pos;
}

/* The next `size` bytes after aligning, or NULL if they are not there. */
static const uint8_t *take(struct bus_reader *reader, size_t alignment, size_t size) {
	bus_read_align(reader, alignment);
	if (reader->failed || size > reader->size - reader->pos) {
		reader->failed = true;
		return NULL;
	}
	const uint8_t *bytes = reader->data + reader->pos;
	reader->pos += size;
	return bytes;
}

uint8_t bus_read_byte(struct bus_reader *reader) {
	const uint8_t *bytes = take(reader, 1, 1);
	return bytes != NULL ? bytes[0] : 0;
}

uint32_t bus_read_u32(struct bus_reader *reader) {
	const uint8_t *bytes = take(reader, 4, 4);
	return bytes != NULL ? le32(bytes) : 0;
}

/* `length` bytes of text and the NUL after them, with no NUL among them. */
static const char *text(struct bus_reader *reader, size_t length) {
	if (reader->failed || length >= reader->size) {
		reader->failed = true;
		return "";
	}
	const uint8_t *bytes = take(reader, 1, length + 1);
	if (bytes == NULL || bytes[length] != '\0' || memchr(bytes, '\0', length) != NULL) {
		reader->failed = true;
		return "";
	}
	return (const char *)bytes;
}

const char *bus_read_string(struct bus_reader *reader) {
	return text(reader, bus_read_u32(reader));
}

const char *bus_read_signature(struct bus_reader *reader) {
	return text(reader, bus_read_byte(reader));
}

static size_t alignment_of(char type) {
	switch (type) {
	case 'y': case 'g': case 'v':
		return 1;
	case 'n': case 'q':
		return 2;
	case 'x': case 't': case 'd': case '(': case '{':
		return 8;
	default:
		return 4;
	}
}

size_t bus_read_array(struct bus_reader *reader, size_t alignment) {
	uint32_t length = bus_read_u32(reader);
	bus_read_align(reader, alignment);
	if (reader->failed || length > reader->size - reader->pos) {
		reader->failed = true;
		return reader->pos;
	}
	return reader->pos + length;
}

/* Where the type starting at `signature` ends: "a{sv}i" gives "i". */
static const char *type_end(const char *signature) {
	while (*signature == 'a') {
		signature++;
	}
	char open = *signature, close = open == '(' ? ')' : '}';
	if (open == '\0') {
		return signature;
	}
	signature++;
	if (open != '(' && open != '{') {
		return signature;
	}
	for (int depth = 1; depth > 0 && *signature != '\0'; signature++) {
		if (*signature == open) {
			depth++;
		} else if (*signature == close) {
			depth--;
		}
	}
	return signature;
}

/* Passes over one value of the type at *signature and moves *signature past
 * that type. */
static void skip_one(struct bus_reader *reader, const char **signature, int depth) {
	char type = **signature;
	if (type == '\0' || depth > DEPTH_MAX) {
		reader->failed = true;
		return;
	}
	(*signature)++;
	switch (type) {
	case 'y':
		take(reader, 1, 1);
		break;
	case 'n': case 'q':
		take(reader, 2, 2);
		break;
	case 'b': case 'i': case 'u': case 'h':
		take(reader, 4, 4);
		break;
	case 'x': case 't': case 'd':
		take(reader, 8, 8);
		break;
	case 's': case 'o':
		bus_read_string(reader);
		break;
	case 'g':
		bus_read_signature(reader);
		break;
	case 'v': {
		const char *inner = bus_read_signature(reader);
		skip_one(reader, &inner, depth + 1);
		break;
	}
	case 'a': {
		/* An array says how long it is, so its elements need no look. */
		if (**signature == '\0') {
			reader->failed = true;
			break;
		}
		size_t end = bus_read_array(reader, alignment_of(**signature));
		if (!reader->failed) {
			reader->pos = end;
		}
		*signature = type_end(*signature);
		break;
	}
	case '(': case '{': {
		char close = type == '(' ? ')' : '}';
		bus_read_align(reader, 8);
		while (**signature != close && !reader->failed) {
			skip_one(reader, signature, depth + 1);
		}
		if (!reader->failed) {
			(*signature)++;
		}
		break;
	}
	default:
		reader->failed = true;
	}
}

void bus_skip(struct bus_reader *reader, const char *signature) {
	while (*signature != '\0' && !reader->failed) {
		skip_one(reader, &signature, 0);
	}
}

long bus_message_size(const uint8_t *data, size_t size) {
	if (size < 16) {
		return 0;
	}
	uint32_t body = le32(data + 4), fields = le32(data + 12);
	/* 'l': little-endian, which is what every program on this machine sends.
	 * Then the protocol's version. */
	if (data[0] != 'l' || data[3] != 1 || fields > FIELDS_MAX || body > MESSAGE_MAX) {
		return -1;
	}
	size_t total = 16 + round_up(fields, 8) + body;
	return total <= MESSAGE_MAX ? (long)total : -1;
}

bool bus_message_parse(const uint8_t *data, size_t size, struct bus_message *message) {
	long total = bus_message_size(data, size);
	if (total <= 0 || (size_t)total != size) {
		return false;
	}
	size_t fields_end = 16 + le32(data + 12);
	*message = (struct bus_message){
		.type = data[1],
		.wants_reply = (data[2] & 1) == 0, /* the "no reply expected" flag */
		.serial = le32(data + 8),
		.path = "", .interface = "", .member = "", .sender = "", .signature = "",
		.body = {data + round_up(fields_end, 8), le32(data + 4), 0, false},
	};

	/* The header fields: an array of (code, variant). */
	struct bus_reader reader = {data, fields_end, 16, false};
	while (reader.pos < reader.size && !reader.failed) {
		bus_read_align(&reader, 8);
		uint8_t code = bus_read_byte(&reader);
		const char *type = bus_read_signature(&reader);
		const char **string = NULL;
		switch (code) {
		case 1: string = &message->path; break;
		case 2: string = &message->interface; break;
		case 3: string = &message->member; break;
		case 7: string = &message->sender; break;
		}
		if (string != NULL && (strcmp(type, "s") == 0 || strcmp(type, "o") == 0)) {
			*string = bus_read_string(&reader);
		} else if (code == 5 && strcmp(type, "u") == 0) {
			message->reply_serial = bus_read_u32(&reader);
		} else if (code == 8 && strcmp(type, "g") == 0) {
			message->signature = bus_read_signature(&reader);
		} else {
			skip_one(&reader, &type, 0);
		}
	}
	return !reader.failed && message->serial != 0;
}

/* --- Writing ---------------------------------------------------------------- */

void bus_write_align(struct bus_writer *writer, size_t alignment) {
	size_t size = round_up(writer->size, alignment);
	if (writer->failed || size > writer->capacity) {
		writer->failed = true;
		return;
	}
	memset(writer->data + writer->size, 0, size - writer->size);
	writer->size = size;
}

void bus_write_bytes(struct bus_writer *writer, const void *bytes, size_t size) {
	if (writer->failed || size > writer->capacity - writer->size) {
		writer->failed = true;
		return;
	}
	if (size > 0) {
		memcpy(writer->data + writer->size, bytes, size);
	}
	writer->size += size;
}

static void set_u32(struct bus_writer *writer, size_t at, size_t value) {
	if (!writer->failed) {
		uint8_t bytes[4] = {(uint8_t)value, (uint8_t)(value >> 8),
			(uint8_t)(value >> 16), (uint8_t)(value >> 24)};
		memcpy(writer->data + at, bytes, 4);
	}
}

void bus_write_u32(struct bus_writer *writer, uint32_t value) {
	const uint8_t zero[4] = {0};
	bus_write_align(writer, 4);
	bus_write_bytes(writer, zero, 4);
	set_u32(writer, writer->size - 4, value);
}

void bus_write_string(struct bus_writer *writer, const char *string) {
	size_t length = strlen(string);
	bus_write_u32(writer, (uint32_t)length);
	bus_write_bytes(writer, string, length + 1);
}

static void write_signature(struct bus_writer *writer, const char *signature) {
	uint8_t length = (uint8_t)strlen(signature);
	bus_write_bytes(writer, &length, 1);
	bus_write_bytes(writer, signature, (size_t)length + 1);
}

size_t bus_write_array_begin(struct bus_writer *writer, size_t alignment) {
	bus_write_u32(writer, 0);
	size_t mark = writer->size - 4;
	/* The padding before the first element is not part of the length, and
	 * is there even if the array is empty. */
	bus_write_align(writer, alignment);
	return mark;
}

void bus_write_array_end(struct bus_writer *writer, size_t mark, size_t alignment) {
	set_u32(writer, mark, writer->size - round_up(mark + 4, alignment));
}

/* One header field: its code, its type as a variant, its value. */
static void field(struct bus_writer *writer, uint8_t code, const char *type, const char *value) {
	if (value == NULL) {
		return;
	}
	bus_write_align(writer, 8);
	bus_write_bytes(writer, &code, 1);
	write_signature(writer, type);
	if (type[0] == 'g') {
		write_signature(writer, value);
	} else {
		bus_write_string(writer, value);
	}
}

void bus_write_begin(struct bus_writer *writer, uint8_t *data, size_t capacity,
		const struct bus_header *header) {
	*writer = (struct bus_writer){.data = data, .capacity = capacity};
	/* Little-endian, the type, "no reply expected" for all but a call, the
	 * protocol's version. Then the body's length (filled in at the end), the
	 * serial, and the length of the fields. */
	uint8_t start[4] = {'l', header->type, header->type != BUS_CALL, 1};
	bus_write_bytes(writer, start, 4);
	bus_write_u32(writer, 0);
	bus_write_u32(writer, header->serial);
	bus_write_u32(writer, 0);
	field(writer, 1, "o", header->path);
	field(writer, 2, "s", header->interface);
	field(writer, 3, "s", header->member);
	field(writer, 4, "s", header->error);
	if (header->reply_serial != 0) {
		/* Code 5, and its type "u" as a variant's signature. */
		const uint8_t reply[4] = {5, 1, 'u', 0};
		bus_write_align(writer, 8);
		bus_write_bytes(writer, reply, 4);
		bus_write_u32(writer, header->reply_serial);
	}
	field(writer, 6, "s", header->destination);
	field(writer, 8, "g", header->signature);
	set_u32(writer, 12, writer->size - 16);
	bus_write_align(writer, 8);
	writer->body = writer->size;
}

size_t bus_write_end(struct bus_writer *writer) {
	set_u32(writer, 4, writer->size - writer->body);
	return writer->failed ? 0 : writer->size;
}
